//===- HostRT/Publish.c - host half of the reverse-publish handshake ------===//
//
// The guest face reverse-publishes its vtable address points / guest
// redispatchers (02-implementation C-1, 04-reentry §2.5). __echo_set_facevts fills this native
// library's matching slots from a guest-built table in ONE crossing: each entry
// names a slot (__echo_facevt$<vtable> or __echo_redisp$<key>, resolved by
// dlsym) and the value to store there. Mirrors GuestRT/Publish.c.
//
// (The forward direction — filling the guest's echo_bind — is the
// compiler-emitted __echo_publish, not a runtime function.)
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// dlsym, RTLD_DEFAULT, dlinfo, struct link_map
#endif

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   /// deep-home back-fill: uintptr_t
#include <sys/mman.h> /// deep-home back-fill: mprotect
#include <unistd.h>   /// deep-home back-fill: sysconf

#include "RuntimeAPI.h"

/// Defined in CheckCall.c; a no-op when the boundary symbol is present.
void __echo_refresh_host_ranges(void);
#include "echo/ABI/CallBlock.h"

/// B-3a name-based binding. __echo_publish_<srchash> (Echo-compiler-emitted) calls
/// this to fill the guest's binding table by NAME: merge-join the face's neutral-
/// name list (names[0..count), sorted) against this native module's directory
/// (dir[0..ndir), sorted). On a match, bind the face slot to the native body + its
/// thunk. Unmatched entries on either side are arch-only functions (e.g. an x86-only
/// class behind #ifdef, absent from the aarch64 native) and are left unbound -- so a
/// face and native whose worklists legitimately differ still bind their common set
/// instead of aborting on a whole-module version-hash mismatch. Both lists are sorted
/// by the same neutral name, so a single linear merge suffices.
void __echo_bind_match(EchoBinding *table, unsigned int count, const char **names,
                       const EchoNatDirEntry *dir, unsigned int ndir)
{
    if (!table || !names)
        return;
    unsigned int i = 0, j = 0;
    while (i < count && j < ndir) {
        int c = strcmp(names[i], dir[j].name);
        if (c == 0) {
            table[i].native_target = dir[j].native_target;
            table[i].entry_thunk = dir[j].thunk;
            ++i;
            ++j;
        } else if (c < 0) {
            ++i; /// face-only slot: leave it null (trap if ever called)
        } else {
            ++j; /// native-only function: nothing on the face to bind
        }
    }
}

void echo_apply_smo_patches(void); /// deep-home back-fill (defined below)

/// Head of this process's link map, or NULL.
static struct link_map *echoLinkMapHead(void)
{
    struct link_map *head = NULL;
    void *self = dlopen(NULL, RTLD_NOW | RTLD_NOLOAD);
    if (self)
        dlinfo(self, RTLD_DI_LINKMAP, &head);
    while (head && head->l_prev)
        head = head->l_prev;
    return head;
}

/// Cached dlsym handles for the loaded objects. Reopening every object per publish was
/// by far the largest start-up cost: a face publishes ONCE PER TRANSLATION UNIT -- 660
/// times across QtCore/QtGui/QtWidgets -- and each pass did dlopen(RTLD_NOLOAD)+dlclose
/// on all 13 loaded objects, about eight seconds of pure loader bookkeeping (measured:
/// the handshake was 87% of a 17.9s start-up, and neither the crossings nor the host
/// dlsyms accounted for it). The object set only changes when something is loaded, so
/// keep the handles and rebuild only when the link map's object count moves.
///
/// Holding a reference defers a real dlclose until the next rebuild. That is safe --
/// the module stays mapped, so a registered SMO/mutinit table stays valid rather than
/// dangling -- it only postpones the unload.
static void **g_objHandles;
static unsigned int g_objCount;
static int g_objValid;
static uintptr_t g_objStamp;

/// A cheap fingerprint of the current object set: the count folded together with each
/// object's load address. Comparing the COUNT ALONE would miss an unload paired with a
/// load, and could not be compared against g_objCount either (that is the number of
/// handles we managed to open, which is smaller whenever a dlopen fails -- then every
/// call would look like a change and rebuild, defeating the cache).
static uintptr_t echoObjectStamp(void)
{
    uintptr_t s = 0;
    for (struct link_map *m = echoLinkMapHead(); m; m = m->l_next)
        if (m->l_name && m->l_name[0])
            s = s * 1000003u + (uintptr_t) m->l_addr + 1u;
    return s;
}

/// Rebuild the handle cache if the object set moved. Returns 1 when it did, which is
/// also the signal a replay is needed (see __echo_set_facevts). Caller holds g_pubMu.
static int echoRefreshObjectCache(void)
{
    uintptr_t stamp = echoObjectStamp();
    if (g_objValid && stamp == g_objStamp)
        return 0; /// nothing loaded or unloaded since last time
    for (unsigned int i = 0; i < g_objCount; i++)
        dlclose(g_objHandles[i]);
    free(g_objHandles);
    g_objHandles = NULL;
    g_objCount = 0;
    g_objValid = 0;

    unsigned int n = 0;
    for (struct link_map *m = echoLinkMapHead(); m; m = m->l_next)
        if (m->l_name && m->l_name[0])
            n++;
    if (!n)
        return 1;
    void **h = (void **) malloc(n * sizeof *h);
    if (!h)
        return 1; /// out of memory: fall back to global-scope-only stores
    unsigned int k = 0;
    for (struct link_map *m = echoLinkMapHead(); m && k < n; m = m->l_next) {
        if (!m->l_name || !m->l_name[0])
            continue; /// main program / vdso: covered by RTLD_DEFAULT
        void *o = dlopen(m->l_name, RTLD_NOW | RTLD_NOLOAD);
        if (o)
            h[k++] = o;
    }
    g_objHandles = h;
    g_objCount = k;
    /// Re-stamp AFTER the opens: dlopen(RTLD_NOLOAD) does not load anything, but taking
    /// the fingerprint again keeps the cache consistent with whatever the map is now.
    g_objStamp = echoObjectStamp();
    g_objValid = 1;
    return 1;
}

/// Apply one publish table across the whole process: the global scope plus every
/// loaded object's own linkonce-ODR slot copy (see __echo_set_facevts for why).
/// Caller holds g_pubMu.
static void echoApplyFaceVts(const EchoFaceVtEntry *tab, unsigned int n)
{
    /// First store into the global scope (covers the main program + RTLD_GLOBAL libs).
    for (unsigned int i = 0; i < n; i++) {
        void **slot = (void **) dlsym(RTLD_DEFAULT, tab[i].name);
        if (slot)
            *slot = tab[i].ap;
    }

    /// Then store into every object's own copy. A raw struct link_map* is NOT a valid
    /// dlsym handle (glibc faults in do_lookup_x), hence the by-name handles above; the
    /// object itself is searched first, so this reaches its own local copy of the slot.
    echoRefreshObjectCache();
    for (unsigned int o = 0; o < g_objCount; o++)
        for (unsigned int i = 0; i < n; i++) {
            void **slot = (void **) dlsym(g_objHandles[o], tab[i].name);
            if (slot)
                *slot = tab[i].ap;
        }
}

/// Retained publish tables, for REPLAY into natives loaded after the fact. A face's
/// publish only reaches modules loaded AT THAT MOMENT; a native library that arrives
/// later (a runtime-dlopen'd plugin's QPA library, or simply a library whose bootstrap
/// runs after its dependency's -- ld.so initialises face QtCore before face QtGui, so
/// native QtGui loads after QtCore's publish) would keep NULL slots for the earlier
/// faces' names. With the mutable-global sync gate that is fatal: the null slot falls
/// back to the never-written native copy while gated writers hit the face copy
/// (QCoreApplication::self read as null in QtGui -> qGuiApp crash). So every table is
/// heap-copied here (names too: the source lives in the face module, which can in
/// principle unload) and ALL retained tables are re-applied on each publish -- stores
/// are idempotent, one name always maps to one canonical value.
static struct {
    EchoFaceVtEntry *tab;
    unsigned int n;
} *g_pubTabs;
static unsigned int g_pubCount, g_pubCap;
static pthread_mutex_t g_pubMu = PTHREAD_MUTEX_INITIALIZER;

static void echoRetainFaceVts(const EchoFaceVtEntry *tab, unsigned int n)
{
    if (g_pubCount == g_pubCap) {
        unsigned int cap = g_pubCap ? g_pubCap * 2 : 8;
        void *p = realloc(g_pubTabs, cap * sizeof *g_pubTabs);
        if (!p)
            return; /// replay best-effort; the direct apply below still ran
        g_pubTabs = p;
        g_pubCap = cap;
    }
    EchoFaceVtEntry *copy = malloc(n * sizeof *copy);
    if (!copy)
        return;
    for (unsigned int i = 0; i < n; i++) {
        copy[i].name = strdup(tab[i].name);
        copy[i].ap = tab[i].ap;
        if (!copy[i].name) {
            for (unsigned int j = 0; j < i; j++)
                free((void *) copy[j].name);
            free(copy);
            return;
        }
    }
    g_pubTabs[g_pubCount].tab = copy;
    g_pubTabs[g_pubCount].n = n;
    g_pubCount++;
}

/// docs/07 §5 three-phase mutable init. A MUTABLE global that is storage-synced has TWO
/// initialisations: the native module's static initialisers run during the host dlopen
/// (phase 1, BEFORE this face publishes, so they land in the NATIVE copy), and the face's
/// own x86 static initialisers run afterwards (phase 2, into the FACE copy, which is the
/// canonical storage). Without a third step the native side's initialised value would be
/// lost -- so the compiler emits, per native module, a table of
/// {slot, &native_G, sizeof G} and the face triggers echo_commit_mutinit() once its own
/// static init is done (phase 3): copy NATIVE -> FACE, so canonical holds the value the
/// native world initialised. Native-constructed objects are the right ones to keep: the
/// heap is host-owned and vptrs are redirected to the face vtable, so a native-initialised
/// pointer is callable from both worlds, while the face's duplicate is an orphan.
///
/// Committing is ONCE PER TABLE: a later library's commit must not re-copy an earlier
/// table, or a value written at run time (qApp) would be reverted to its init value.
typedef struct { void **slot; void *native; unsigned long size; } EchoMutInit;

#define ECHO_MUTINIT_MAX_TABS 256
static struct {
    const EchoMutInit *tab;
    unsigned n;
    int committed;
} echo_mutinit_tabs[ECHO_MUTINIT_MAX_TABS];
static unsigned echo_mutinit_ntabs;
static pthread_mutex_t echo_mutinit_mutex = PTHREAD_MUTEX_INITIALIZER;

/// Called from a native module's priority-101 constructor (compiler-emitted), i.e. BEFORE
/// that module's own static initialisers. Two jobs:
///   - register this module's mutable-init table for the later commit;
///   - pull every publish that already happened into THIS module's slot copies, so a
///     static initialiser here that reads a global synced by an EARLIER library reads the
///     canonical (face) storage instead of the stale native copy. (The replay inside
///     __echo_set_facevts covers modules that load before a publish; this covers a module
///     that loads after one.)
void __echo_register_mutinit(const void *tab, unsigned int n)
{
    if (tab && n) {
        pthread_mutex_lock(&echo_mutinit_mutex);
        unsigned i = 0;
        for (; i < echo_mutinit_ntabs; i++)
            if (echo_mutinit_tabs[i].tab == tab)
                break;
        if (i == echo_mutinit_ntabs && echo_mutinit_ntabs < ECHO_MUTINIT_MAX_TABS) {
            echo_mutinit_tabs[echo_mutinit_ntabs].tab = (const EchoMutInit *) tab;
            echo_mutinit_tabs[echo_mutinit_ntabs].n = n;
            echo_mutinit_tabs[echo_mutinit_ntabs].committed = 0;
            echo_mutinit_ntabs++;
        }
        pthread_mutex_unlock(&echo_mutinit_mutex);
    }

    pthread_mutex_lock(&g_pubMu);
    for (unsigned int t = 0; t < g_pubCount; t++)
        echoApplyFaceVts(g_pubTabs[t].tab, g_pubTabs[t].n);
    pthread_mutex_unlock(&g_pubMu);
}

void __echo_unregister_mutinit(const void *tab)
{
    pthread_mutex_lock(&echo_mutinit_mutex);
    for (unsigned i = 0; i < echo_mutinit_ntabs; i++) {
        if (echo_mutinit_tabs[i].tab != tab)
            continue;
        echo_mutinit_tabs[i] = echo_mutinit_tabs[echo_mutinit_ntabs - 1];
        echo_mutinit_tabs[echo_mutinit_ntabs - 1].tab = NULL;
        echo_mutinit_tabs[echo_mutinit_ntabs - 1].n = 0;
        echo_mutinit_tabs[echo_mutinit_ntabs - 1].committed = 0;
        echo_mutinit_ntabs--;
        break;
    }
    pthread_mutex_unlock(&echo_mutinit_mutex);
}


/// Is `v` an address of CODE in a native (host) module?
///
/// docs/07 §5 phase 3 copies the native module's statically-initialised value into the
/// canonical face storage, on the stated grounds that "a native-initialised pointer is
/// callable from both worlds". That holds for a data pointer (the heap is host-owned) and
/// for a vptr (redirected to the face vtable). It does NOT hold for a bare pointer to
/// native CODE. The guest loads such a value out of the shared slot and calls it
/// directly, which jumps into the host region and faults.
///
/// libxml2 is the case that exposed it: `xmlMallocFunc xmlMalloc = malloc;` is an exported
/// mutable global whose static initialiser is a function address. Committing the native
/// copy put the HOST libc malloc into the shared slot, and the guest's first
/// `xmlMalloc(...)` jumped there and died (testdict, testparser, testchar).
///
/// The test walks the loaded objects' program headers and asks whether the address falls
/// inside an executable PT_LOAD segment. That separates a code address from a data address
/// exactly, and unlike a symbol lookup it also catches a STATIC function: dladdr and
/// dladdr1 resolve only DYNAMIC symbols, so a file-local function is invisible to them.
/// libxml2 has one on the same page as the problem it exposed -- `xmlStrdupFunc
/// xmlMemStrdup = xmlPosixStrdup;`, where xmlPosixStrdup is a local `t` symbol -- and an
/// earlier dladdr1-based version of this check let its native address through, so
/// testparser still jumped into the host from a guest callback.
///
/// Checking the host/guest boundary instead would be wrong: a native DATA pointer sits in
/// the host region too, and copying that one is correct.
struct EchoCodeProbe { const void *addr; int isCode; };

static int echoCodeProbeCb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void) size;
    struct EchoCodeProbe *p = (struct EchoCodeProbe *) data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
            continue;
        const char *lo = (const char *) (info->dlpi_addr + ph->p_vaddr);
        if ((const char *) p->addr >= lo && (const char *) p->addr < lo + ph->p_memsz) {
            p->isCode = 1;
            return 1; /// stop iterating
        }
    }
    return 0;
}

static int echoValueIsNativeCode(void *v)
{
    if (!v)
        return 0;
    struct EchoCodeProbe p = { v, 0 };
    dl_iterate_phdr(echoCodeProbeCb, &p);
    return p.isCode;
}

/// [lo,hi) of the loaded object that contains `addr`, or 0/0 if no object does.
///
/// Used to check that a mutable-init table's `&native_G` still points into the module
/// that emitted the table. It normally must: the compiler emits one table per native
/// module, listing globals that module defines. But the field is filled by a SYMBOLIC
/// relocation (R_AARCH64_ABS64 against G), so the host dynamic linker is free to bind it
/// to any definition of G in the global scope -- and under box64 there is one. box64
/// exports its own copy of the versioned data symbols it interposes for the guest
/// (src/librarian/globalsymbols.c: `EXPORT int xsltMaxVars;` and friends), and box64 is
/// the MAIN EXECUTABLE, so it sits ahead of every library in the search order. The
/// native half's `&xsltMaxVars` then resolves to box64's zero-filled .bss word, phase 3
/// copies that zero over the canonical storage, and libxslt runs with maxTemplateVars=0
/// -- every transform dies as "a potential infinite template recursion". qemu and FEX
/// have no such definition, so the same build is correct there; the divergence looks like
/// an emulator bug in out-parameter writes and is nothing of the sort.
///
/// Skipping the copy is the right repair, not a fallback: the face's own copy already
/// holds the same source-level initialiser, so leaving it alone yields the value the
/// program was written to see.
struct EchoObjRange { uintptr_t addr, lo, hi; };

static int echoObjRangeCb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void) size;
    struct EchoObjRange *r = (struct EchoObjRange *) data;
    uintptr_t lo = (uintptr_t) -1, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uintptr_t s = (uintptr_t) info->dlpi_addr + ph->p_vaddr;
        if (s < lo)
            lo = s;
        if (s + ph->p_memsz > hi)
            hi = s + ph->p_memsz;
    }
    if (hi && r->addr >= lo && r->addr < hi) {
        r->lo = lo;
        r->hi = hi;
        return 1; /// stop iterating
    }
    return 0;
}

/// One dl_iterate_phdr per TABLE, not per entry: every entry of a table belongs to the
/// same module, so the range is computed once and each `native` is a range check.
static void echoObjectRange(const void *addr, uintptr_t *lo, uintptr_t *hi)
{
    struct EchoObjRange r = { (uintptr_t) addr, 0, 0 };
    dl_iterate_phdr(echoObjRangeCb, &r);
    *lo = r.lo;
    *hi = r.hi;
}

/// Did a foreign definition capture this entry's `&native_G`? See echoObjectRange.
static int echoMutInitHijacked(const void *native, uintptr_t lo, uintptr_t hi)
{
    return hi && ((uintptr_t) native < lo || (uintptr_t) native >= hi);
}

/// Phase 3, called from the face's LAST constructor (after its own static init).
void echo_commit_mutinit(void)
{
    pthread_mutex_lock(&echo_mutinit_mutex);
    for (unsigned t = 0; t < echo_mutinit_ntabs; t++) {
        if (echo_mutinit_tabs[t].committed)
            continue;
        const EchoMutInit *e = echo_mutinit_tabs[t].tab;
        uintptr_t objLo = 0, objHi = 0;
        echoObjectRange(e, &objLo, &objHi);
        for (unsigned i = 0; i < echo_mutinit_tabs[t].n; i++) {
            void *face = e[i].slot ? *e[i].slot : NULL;
            if (!face || face == e[i].native || !e[i].size)
                continue; /// unpublished (stand-alone native) or already canonical
            /// `&native_G` bound outside this module -- a foreign definition captured it.
            /// Its contents say nothing about G, so keep the face's own initialiser.
            if (echoMutInitHijacked(e[i].native, objLo, objHi)) {
                if (getenv("ECHO_DEBUG"))
                    fprintf(stderr, "x64echo: mutinit skips a hijacked &native (%p is "
                                    "outside [%p,%p) of its own table)\n",
                            e[i].native, (void *) objLo, (void *) objHi);
                continue;
            }
            /// A pointer-sized global whose native value is a native CODE address must
            /// keep the FACE's own initialiser: that one is a guest address, which the
            /// guest can call directly and which a native indirect call reaches through
            /// the D-3 dispatcher's reentry. Copying the native address would leave the
            /// shared slot callable from the host only. See echoValueIsNativeCode.
            if (e[i].size == sizeof(void *) &&
                echoValueIsNativeCode(*(void **) e[i].native)) {
                if (getenv("ECHO_DEBUG"))
                    fprintf(stderr, "x64echo: mutinit keeps face value for a native code "
                                    "pointer (native=%p)\n", *(void **) e[i].native);
                continue;
            }
            memcpy(face, e[i].native, e[i].size);
        }
        echo_mutinit_tabs[t].committed = 1;
    }
    pthread_mutex_unlock(&echo_mutinit_mutex);
}

/// Point every published slot back at nothing, so a native destructor that reads a
/// canonicalised global reads its OWN copy again.
///
/// The face copies live in the guest's address space, and how long that survives the
/// program's end is the emulator's business, not ours. qemu-user ends the process with
/// exit_group, so the host's atexit handlers and _dl_fini never run and a native
/// destructor is never reached. box64 lets its own main return, so glibc runs the full
/// teardown -- by which point box64 has already unmapped the guest ELF, and pixman's
/// destructor, whose read of global_implementation the compiler redirected to the face
/// copy, faults on an address that no longer exists.
///
/// Clearing the slots restores the fallback the redirection already has (a null slot
/// means "use the native copy"), so teardown reads a copy that is merely stale rather
/// than unmapped. Stale is harmless here: the process is on its way out, and the worst
/// case is a destructor that frees nothing.
///
/// Registered with atexit, which glibc drains BEFORE _dl_fini -- so this runs ahead of
/// every library destructor, including the ones in libraries loaded after us.
static void echoTeardownFaceSlots(void)
{
    pthread_mutex_lock(&g_pubMu);
    echoRefreshObjectCache();
    for (unsigned int t = 0; t < g_pubCount; t++) {
        const EchoFaceVtEntry *tab = g_pubTabs[t].tab;
        for (unsigned int i = 0; i < g_pubTabs[t].n; i++) {
            void **slot = (void **) dlsym(RTLD_DEFAULT, tab[i].name);
            if (slot)
                *slot = NULL;
            for (unsigned int o = 0; o < g_objCount; o++) {
                slot = (void **) dlsym(g_objHandles[o], tab[i].name);
                if (slot)
                    *slot = NULL;
            }
        }
    }
    g_pubCount = 0;
    pthread_mutex_unlock(&g_pubMu);

    /// The native copies are stale in the other direction: every run-time write went to
    /// the face copy, so what is left here is whatever the library's own constructor put
    /// there before the handshake. pixman's destructor would walk that old implementation
    /// chain and free nodes the live chain already owns ("double free or corruption").
    /// Zero them instead. Every one of these is a global the compiler chose to share, and
    /// a destructor reading zero takes the "nothing to release" path -- which is the right
    /// answer at this point, since the process is exiting and the real state lives in an
    /// address space that is already gone.
    pthread_mutex_lock(&echo_mutinit_mutex);
    for (unsigned t = 0; t < echo_mutinit_ntabs; t++) {
        const EchoMutInit *e = echo_mutinit_tabs[t].tab;
        uintptr_t objLo = 0, objHi = 0;
        echoObjectRange(e, &objLo, &objHi);
        for (unsigned i = 0; i < echo_mutinit_tabs[t].n; i++)
            /// Same guard as phase 3: a hijacked `&native_G` is some other module's
            /// storage, and zeroing that is a write into a stranger's data.
            if (e[i].native && e[i].size &&
                !echoMutInitHijacked(e[i].native, objLo, objHi))
                memset(e[i].native, 0, e[i].size);
    }
    pthread_mutex_unlock(&echo_mutinit_mutex);
}

/// Make the face's stub pages executable ON THE HOST SIDE.
///
/// The guest's own mprotect is not enough. qemu-user forwards a guest mprotect to the
/// host with PROT_EXEC stripped -- correct for it, since guest code runs through TCG and
/// the host never executes those bytes. But the host DOES land on a stub whenever a call
/// site reaches one without going through __echo_check_call, and then the page has no
/// execute permission and the process dies with SEGV_ACCERR at an address that resolves
/// to whatever stub happens to sit there. Nothing about that points at the real cause:
/// the core dump's program headers show R--, the bootstrap's own diagnostic shows it
/// asked for R+X, and only an strace of the host shows the EXEC bit going missing in
/// transit.
///
/// Called from the guest's bootstrap through dlcall, so the mprotect is issued by the
/// host and keeps PROT_EXEC.
void __echo_host_exec_stubs(void *lo, void *hi)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0 || !lo || (char *) hi <= (char *) lo)
        return;
    uintptr_t s = (uintptr_t) lo & ~((uintptr_t) pg - 1);
    uintptr_t e = ((uintptr_t) hi + (uintptr_t) pg - 1) & ~((uintptr_t) pg - 1);
    if (mprotect((void *) s, (size_t) (e - s), PROT_READ | PROT_EXEC) != 0)
        fputs("x64echo: host-side mprotect of echo_stubs failed\n", stderr);
}

void __echo_set_facevts(const EchoFaceVtEntry *tab, unsigned int n)
{
    /// This native half is on line: (re)arm the exit barrier so it stays ahead of the
    /// static destructors this library registered when it was dlopen'd. See
    /// __echo_arm_exit_barrier in Entry.c for why the guest's exit is too late.
    void __echo_arm_exit_barrier(void);
    __echo_arm_exit_barrier();

    static int teardown_armed;
    if (!teardown_armed) {
        teardown_armed = 1;
        atexit(echoTeardownFaceSlots);
    }

    /// Each native library carries its OWN linkonce-ODR copy of a slot symbol
    /// (__echo_redisp$<key>, __echo_facevt$..., __echo_var$..., __echo_faceaddr$...).
    /// The host loads the native libraries into a local scope (RTLD_LOCAL), so cross-
    /// library interposition never unifies those copies: each library's own
    /// echo_dispatch / vptr-redirect reads the copy defined IN that library. A single
    /// dlsym(RTLD_DEFAULT, name) resolves only the first global definition, so with
    /// more than one native library (e.g. QtCore + QtTest) every other library's slot
    /// would stay NULL -- and a NULL __echo_redisp$<key> aliases the thread-create
    /// marker in the guest reentry loop (GuestRT/Cross.c), silently turning a host->
    /// guest virtual call into a thread spawn (metaObject() -> garbage). So fill the
    /// slot in EVERY loaded object. Each name maps to one canonical value (a per-
    /// signature redispatcher, a vtable address point, a canonical global/face addr),
    /// so storing the same value into every copy is always correct.
    pthread_mutex_lock(&g_pubMu);

    /// Replay every earlier face's table, but ONLY when a module actually arrived since
    /// the last publish -- that arrival is the entire reason replay exists (the new module
    /// carries slots for names published before it existed). A face publishes once per
    /// TRANSLATION UNIT, hundreds of times in a row with nothing loading in between, and
    /// replaying unconditionally makes start-up quadratic: publish t re-applies t tables,
    /// each storing into every loaded object, which measured as ~1.1s of pure dlsym for a
    /// Core+Gui+Widgets process. When the object set has not moved, every retained table
    /// has already been applied to every object present, so the replay is a no-op by
    /// construction and skipping it changes nothing.
    if (echoRefreshObjectCache()) {
        /// A new object also moves the host/guest classification map that
        /// __echo_check_call falls back on when there is no boundary symbol.
#ifndef ECHO_CHECK_DLADDR
        __echo_refresh_host_ranges();
#endif
        for (unsigned int t = 0; t < g_pubCount; t++)
            echoApplyFaceVts(g_pubTabs[t].tab, g_pubTabs[t].n);
    }

    echoApplyFaceVts(tab, n);
    echoRetainFaceVts(tab, n);
    pthread_mutex_unlock(&g_pubMu);

    /// docs/07 §6 deep-home: now that the __echo_var$G slots hold the FACE copies,
    /// back-fill the deep-home fields (a QMetaObject SMO's d.data / stringdata) so the
    /// face copy's pointer field targets the authoritative NATIVE data blob.
    echo_apply_smo_patches();
}

/// docs/07 §6 deep-home back-fill. An exported const object is address-homed to the FACE
/// copy (__echo_var), but a pointer FIELD inside it may target an INTERNAL data blob that
/// is not homed -- so a cross-world pointer-identity comparison INTO that blob splits
/// (Qt QMetaMethod compares data.d, a pointer into the moc qt_meta_data[]). The blob is
/// homed at HOST: the native copy is authoritative, and the FACE copy's field is rewritten
/// to point at it here. The Echo compiler emits, per native module, a descriptor table
///   { void **slot;  uint64_t field_off;  void *native_blob; }
/// naming the __echo_var$G slot (holds the face copy after publish), the field's byte
/// offset, and this module's native blob; a load-time ctor registers each table below.
/// kind 0 (native-home): a is the __echo_var$G slot (face copy), b the native value ->
///   *(*a + off) = b. kind 1 (face-home): a is this module's own (local) global, b the
///   __echo_faceaddr$F slot (face stub) -> *(a + off) = *b.
typedef struct { void *a; unsigned long off; void *b; unsigned long kind; } EchoSmoPatch;

#define ECHO_SMO_MAX_TABS 256
static struct { const EchoSmoPatch *tab; unsigned n; } echo_smo_tabs[ECHO_SMO_MAX_TABS];
static unsigned echo_smo_ntabs;
static pthread_mutex_t echo_smo_mutex = PTHREAD_MUTEX_INITIALIZER;

/// Return the current protection bits for the mapping containing addr. The face
/// object is loaded by the guest loader and is not necessarily present in the
/// host's link_map, but both worlds share /proc/self/maps.
static int echo_page_prot(const void *addr, int *prot)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return -1;

    uintptr_t needle = (uintptr_t) addr;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, maps)) {
        unsigned long start, end;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3 ||
            needle < (uintptr_t) start || needle >= (uintptr_t) end)
            continue;
        *prot = (perms[0] == 'r' ? PROT_READ : 0) |
                (perms[1] == 'w' ? PROT_WRITE : 0) |
                (perms[2] == 'x' ? PROT_EXEC : 0);
        found = 0;
        break;
    }
    fclose(maps);
    return found;
}

void __echo_register_smo_patches(const void *tab, unsigned int n)
{
    pthread_mutex_lock(&echo_smo_mutex);
    for (unsigned i = 0; i < echo_smo_ntabs; i++) {
        if (echo_smo_tabs[i].tab == tab) {
            pthread_mutex_unlock(&echo_smo_mutex);
            return;
        }
    }
    if (echo_smo_ntabs < ECHO_SMO_MAX_TABS) {
        echo_smo_tabs[echo_smo_ntabs].tab = (const EchoSmoPatch *) tab;
        echo_smo_tabs[echo_smo_ntabs].n = n;
        echo_smo_ntabs++;
    }
    pthread_mutex_unlock(&echo_smo_mutex);
}

void __echo_unregister_smo_patches(const void *tab)
{
    pthread_mutex_lock(&echo_smo_mutex);
    for (unsigned i = 0; i < echo_smo_ntabs; i++) {
        if (echo_smo_tabs[i].tab != tab)
            continue;
        echo_smo_ntabs--;
        if (i != echo_smo_ntabs)
            echo_smo_tabs[i] = echo_smo_tabs[echo_smo_ntabs];
        echo_smo_tabs[echo_smo_ntabs].tab = NULL;
        echo_smo_tabs[echo_smo_ntabs].n = 0;
        break;
    }
    pthread_mutex_unlock(&echo_smo_mutex);
}

/// Apply every registered table: for each descriptor whose slot is now filled (the face
/// copy was published), write native_blob into face_copy + field_off. Idempotent and
/// skips unfilled slots, so calling it after each __echo_set_facevts crossing (one per
/// face library) converges as later libraries publish their slots. The face copy lives in
/// guest memory; temporarily add write permission to its page and restore the exact old
/// protection afterwards (the page may be RELRO or ordinary writable data).
void echo_apply_smo_patches(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    pthread_mutex_lock(&echo_smo_mutex);
    for (unsigned t = 0; t < echo_smo_ntabs; t++) {
        const EchoSmoPatch *p = echo_smo_tabs[t].tab;
        for (unsigned i = 0; i < echo_smo_tabs[t].n; i++) {
            void *target;  /// the object holding the field to patch
            void *value;   /// what to write into the field
            if (p[i].kind == 1) {
                /// face-home: a is this module's own global, b the __echo_faceaddr$F slot
                target = p[i].a;
                value = p[i].b ? *(void **) p[i].b : NULL;
            } else {
                /// native-home: a is the __echo_var$G slot (face copy), b the native value
                target = p[i].a ? *(void **) p[i].a : NULL;
                value = p[i].b;
            }
            if (!target || !value)
                continue; /// slot not published yet (or never): leave native fallback
            void **field = (void **) ((char *) target + p[i].off);
            if (*field == value)
                continue; /// already patched: this runs after EVERY publish (hundreds of
                          /// times, once per translation unit) over every registered
                          /// descriptor, and the write is idempotent -- so re-doing it
                          /// costs a /proc/self/maps parse plus two mprotect calls PER
                          /// DESCRIPTOR for no effect. That was the dominant remaining
                          /// start-up cost once the replay storm was fixed.
            uintptr_t a = (uintptr_t) field;
            void *base = (void *) (a & ~((uintptr_t) pg - 1));
            int oldprot;
            if (echo_page_prot(field, &oldprot) != 0)
                continue;
            int changed = !(oldprot & PROT_WRITE);
            if (changed && mprotect(base, (size_t) pg, oldprot | PROT_WRITE) != 0)
                continue;
            *field = value;
            if (changed && mprotect(base, (size_t) pg, oldprot) != 0) {
                fputs("x64echo: failed to restore deep-home page protection\n", stderr);
                abort();
            }
        }
    }
    pthread_mutex_unlock(&echo_smo_mutex);
}
