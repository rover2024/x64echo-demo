//===- HostRT/CheckCall.c - host-side indirect-call dispatch --------------===//
//
// Routes native indirect/virtual calls (02-implementation 地基 D / 04-reentry). __echo_check_call
// translates a face stub to the real native body, else passes the address
// through; __echo_is_host classifies host vs guest so the per-signature
// dispatcher can choose native-call vs reentry.
//
// Two mutually-exclusive ways to classify "host body vs guest", picked at compile
// time:
//   - default (boundary B): a QEMU-exported symbol address as the host/guest
//     divider. Needs guest LOW / host HIGH (e.g. qemu's reserved_va patch).
//   - -DECHO_CHECK_DLADDR: dladdr(addr); the host ld's dladdr sees only
//     HOST-loaded objects, so a hit => host body. Layout-agnostic.
//
// Boundary B degrades gracefully. When the QEMU symbol is absent the divider would be
// NULL and every address would classify as host -- correct for a plain native process
// (there is no guest), but WRONG under an emulator that has no such symbol and does not
// put the guest below the host: box64 loads the x86 guest around 0x7fff_00000000 while
// the aarch64 host libraries sit higher, so a face stub would be called as host code and
// the CPU would try to execute x86 bytes. So the NULL case falls back to an explicit map
// of the host-loaded objects, built from the program headers. That is layout-agnostic
// like the dladdr mode but costs a binary search instead of a symbol lookup, which
// matters because this runs on every native indirect call.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// dladdr, RTLD_DEFAULT
#endif

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "RuntimeAPI.h"

#ifndef ECHO_CHECK_DLADDR
#  include <link.h>
#  include <stdlib.h>
#  include <string.h>

/// boundary-B mode: host-region lower bound, looked up once.
static void *s_boundary;

/// Fallback map, used only when s_boundary is NULL: the address ranges of every object
/// the HOST loader has mapped. An address inside one of them is host code; anything else
/// is the guest's. Sorted, so the lookup is a binary search.
typedef struct { uintptr_t lo, hi; } EchoRange;
#  define ECHO_MAX_HOST_RANGES 512
static EchoRange s_ranges[ECHO_MAX_HOST_RANGES];
static unsigned s_nranges;

/// Negative cache, by page. A MISS is neither rare nor cheap: a face stub lives in the
/// GUEST's memory, so it misses the map by construction, and dladdr then walks the host
/// loader's whole link map to answer "no" -- on every native indirect call that goes
/// through a stub. Under QEMU this path does not exist at all (s_boundary settles the
/// classification in one comparison), which is why only box64 and FEX ever paid for it,
/// and why they paid so much: Qt's software renderer reaches its blend routines through
/// hand-written function-pointer tables, which are face-homed, so the cost lands once per
/// span -- 3000 scene-graph nodes of 48 rows each measured 26 ms per frame, five times the
/// whole frame's native cost.
///
/// A page the host loader does not own can only become host-owned after the host maps
/// something there, and every host load goes through the publish handshake, which calls
/// __echo_refresh_host_ranges. Clearing the cache there keeps the answer correct without
/// giving up the fast path. Direct-mapped and lock-free on purpose: this runs on every
/// native indirect call, entries are pointer-sized aligned stores, and the only effect of
/// a race is that some thread repeats a lookup it could have skipped.
#  define ECHO_GUEST_CACHE 1024 ///< power of two
static uintptr_t s_guestpage[ECHO_GUEST_CACHE]; ///< page+1; 0 = empty

static inline unsigned echoPageSlot(uintptr_t page)
{
    return (unsigned) ((page * 2654435761u) & (ECHO_GUEST_CACHE - 1));
}

static int echoRangeCmp(const void *a, const void *b)
{
    uintptr_t x = ((const EchoRange *) a)->lo, y = ((const EchoRange *) b)->lo;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int echoCollectRanges(struct dl_phdr_info *info, size_t size, void *data)
{
    (void) size;
    (void) data;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
            continue;
        if (s_nranges >= ECHO_MAX_HOST_RANGES)
            return 1;
        s_ranges[s_nranges].lo = info->dlpi_addr + ph->p_vaddr;
        s_ranges[s_nranges].hi = s_ranges[s_nranges].lo + ph->p_memsz;
        s_nranges++;
    }
    return 0;
}

/// Rebuild the map. Called at startup and again whenever the host loads another native
/// library, which the publish handshake is the choke point for (see Publish.c).
void __echo_refresh_host_ranges(void)
{
    if (s_boundary)
        return; /// boundary B is in force; the map is not consulted
    s_nranges = 0;
    dl_iterate_phdr(echoCollectRanges, NULL);
    qsort(s_ranges, s_nranges, sizeof s_ranges[0], echoRangeCmp);
    memset(s_guestpage, 0, sizeof s_guestpage); ///< the object set moved; re-verify misses
}

static inline int echoInHostRanges(uintptr_t a)
{
    unsigned lo = 0, hi = s_nranges;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (a < s_ranges[mid].lo)
            hi = mid;
        else if (a >= s_ranges[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/// True when addr belongs to a host-loaded object. Only reached with s_boundary NULL.
///
/// A HIT is conclusive; a MISS is not. The map is a snapshot, and it is only rebuilt when
/// a publish notices the object set moved -- so a native library the host dlopens without
/// a publish behind it, or one that arrives after the last refresh, is simply absent, and
/// every address in it reads as guest. What that produces is not a wrong answer at the
/// call site but a crash somewhere else entirely: the per-signature dispatcher sees
/// "guest" and reenters, there is no live crossing to reenter into, and __echo_reenter
/// writes through invs[-1].blk == NULL. FFmpeg's avcodec-fft died exactly there, with the
/// target sitting in libavcodec.so.58's own executable segment.
///
/// So verify a miss against dladdr, which asks the host loader itself rather than a cached
/// copy of its answer, and rebuild the map when the miss turns out to have been staleness.
/// The fast path is untouched: a hit still costs one binary search, and a genuine guest
/// address -- which is what a real miss means -- pays for a lookup that was going to end
/// in a reentry anyway.
static inline int echoIsHostMapped(void *addr)
{
    if (!s_nranges)
        __echo_refresh_host_ranges();
    if (echoInHostRanges((uintptr_t) addr))
        return 1;
    const uintptr_t page = (uintptr_t) addr >> 12;
    const unsigned slot = echoPageSlot(page);
    if (s_guestpage[slot] == page + 1)
        return 0; ///< dladdr already said "not host" for this page since the last refresh
    Dl_info info;
    if (dladdr(addr, &info) && info.dli_fbase) {
        __echo_refresh_host_ranges(); ///< we were stale; make the next lookup cheap
        return 1;
    }
    s_guestpage[slot] = page + 1;
    return 0;
}

static __attribute__((constructor)) void echoCheckCallInit(void)
{
    s_boundary = dlsym(RTLD_DEFAULT, "qemu_plugin_register_vcpu_syscall_filter_cb");
    if (!s_boundary)
        __echo_refresh_host_ranges();
}
#endif

int __echo_is_host(void *addr)
{
#ifdef ECHO_CHECK_DLADDR
    Dl_info info;
    return dladdr(addr, &info) != 0;
#else
    /// With a divider, one comparison settles it. Without one, ask the host object map:
    /// in a plain native process every address it is asked about is host-mapped, so the
    /// library stays fully usable stand-alone, and under an emulator whose guest does not
    /// sit below the host the guest's own pages are simply absent from the map.
    if (s_boundary)
        return (char *) addr >= (char *) s_boundary;
    return echoIsHostMapped(addr);
#endif
}

void *__echo_check_call(void *addr)
{
#ifdef ECHO_CHECK_DLADDR
    Dl_info info;
    if (dladdr(addr, &info))
        return addr; ///< host-loaded object -> host body
#else
    if (s_boundary ? ((char *) addr >= (char *) s_boundary) : echoIsHostMapped(addr))
        return addr; ///< host region -> host body
#endif

    EchoStub *s = (EchoStub *) addr; ///< guest region: self-stub or guest code
    if (s->magic == ECHO_STUB_MAGIC) {
        void *t = *s->target_slot; ///< real native body (echo_bind[i])
        /// A stub whose slot does not hold a HOST address never got bound: the slot
        /// still holds whatever the face put there, typically its own lazy resolver.
        /// The call then goes wherever that points instead of to a native body, and
        /// the crash lands in a page with no symbol and no usable stack, so the one
        /// fact worth having is that a binding was missing and which stub it was.
        /// Opt-in because this runs on every native indirect call.
        static int dbg = -1;
        if (dbg < 0)
            dbg = getenv("ECHO_DEBUG_STUB") != NULL;
        /// A NULL slot has to be reported too, and it used to be the one case this
        /// skipped: the `t &&` guard excluded exactly the commonest way a binding
        /// goes missing. The caller then treats NULL as an ordinary guest address,
        /// reenters with a null function pointer, and the crash surfaces inside the
        /// face's redispatcher with nothing to say which stub it was.
#ifdef ECHO_CHECK_DLADDR
        int unresolved = !t || !__echo_is_host(t);
#else
        int unresolved = !t || (s_boundary
                                ? ((char *) t < (char *) s_boundary)
                                : !echoIsHostMapped(t));
#endif
        if (dbg && unresolved)
            fprintf(stderr, "x64echo: stub %p resolves to %s %p\n",
                    addr, t ? "non-host" : "UNBOUND", t);
        return t;
    }
    return addr; ///< real guest code: dispatcher reenters (R-2)
}
