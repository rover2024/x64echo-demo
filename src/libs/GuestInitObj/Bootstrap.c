//===- GuestRT/Bootstrap.c - face .so constructor / handshake -------------===//
//
// Runs in the guest as the face .so's constructor. Minimal handshake:
//   1. confirm the emulator is qemu (dlcall present)
//   2. determine + dlopen this face's paired native Echo library on the host
//   3. resolve the host __echo_entry (the crossing target)
//   4. bind every face symbol's native body + entry thunk (__echo_gen_resolve)
//   5. flip the self-stub section to R+X
//
// face<->native pairing (each face has exactly ONE native partner, co-built):
//   - if the face defines __echo_native_lib() it names the partner explicitly;
//   - otherwise derive by convention from the face's own soname:
//     libX.so -> libX_host.so (resolved on the host via its LD_LIBRARY_PATH).
// There is no global env var: many Echo faces can coexist in one process, each
// finding its own native.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// dladdr
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <time.h>

#include <echo/GuestRuntime/DLCall.h>
#include <echo/GuestRuntime/RuntimeAPI.h>

/// Defined in GuestRT/Cross.c; the host address of __echo_entry.
extern void *echo_host_entry;

/// Per-module resolver (Echo compiler, runFace): fills that module's echo_bind by
/// crossing to its native __echo_publish_<hash>. Each face translation unit
/// registers one in the linker-collected `echo_resolvers` section; the bootstrap
/// runs them all, so a multi-.cpp library binds every module.
typedef void (*EchoResolver)(void *native_handle);
extern EchoResolver __start_echo_resolvers[] __attribute__((weak));
extern EchoResolver __stop_echo_resolvers[] __attribute__((weak));

/// Bounds of the `echo_boot` section: bootstrap hooks a pluggable guest module
/// registers (EchoBootHook, RuntimeAPI.h). Weak so a face with no such module links
/// (then __start == __stop and the loop is empty).
extern EchoBootHook __start_echo_boot[] __attribute__((weak));
extern EchoBootHook __stop_echo_boot[] __attribute__((weak));

/// Optional per-face override: a face may define this to name its native partner
/// explicitly. Weak so the common case (derive by convention) links without it.
const char *__echo_native_lib(void) __attribute__((weak));

/// This face's native partner handle, kept for the phase-3 commit below.
static void *echo_native_handle;

/// Bounds of the self-identifying stub section (02-implementation 地基 D). The linker emits
/// __start_/__stop_ for C-identifier section names; weak so it links even if a
/// face .so has no stubs.
extern char __start_echo_stubs[] __attribute__((weak));
extern char __stop_echo_stubs[] __attribute__((weak));

/// Resolve this face's paired native library name into `buf`. Returns buf, or
/// NULL if it could not be determined.
static const char *echoResolveNativeLib(char *buf, size_t n)
{
    /// 1) explicit override defined by the face.
    if (__echo_native_lib)
        return __echo_native_lib();

    /// 2) convention: derive from the face's own soname (dladdr on a symbol that
    /// lives in this very .so), libX.so -> libX_host.so. Pass the basename; the
    /// host's dlopen resolves it via LD_LIBRARY_PATH.
    Dl_info info;
    if (dladdr((void *) &echoResolveNativeLib, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        base = base ? base + 1 : info.dli_fname;
        const char *dot = strstr(base, ".so");
        if (dot) {
            size_t pre = (size_t) (dot - base);
            if (pre + sizeof("_host") + strlen(dot) <= n) {
                memcpy(buf, base, pre);
                memcpy(buf + pre, "_host", sizeof("_host") - 1);
                strcpy(buf + pre + sizeof("_host") - 1, dot);
                return buf;
            }
        }
    }
    return NULL;
}

/// This face's own basename (libX.so.6.8.4). Used as the second candidate: in the
/// productization layout (docs/06 §7) the native library keeps its REAL name and
/// lives on the host's library path while the face keeps the same soname on the
/// guest's path, so the host's dlopen of the plain basename resolves to the native.
static const char *echoFaceBasename(char *buf, size_t n)
{
    Dl_info info;
    if (dladdr((void *) &echoFaceBasename, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        base = base ? base + 1 : info.dli_fname;
        if (strlen(base) < n) {
            strcpy(buf, base);
            return buf;
        }
    }
    return NULL;
}

/// Startup profiling (ECHO_TIME=1): milliseconds since this face's constructor began.
/// Bootstrap is the one place that can attribute start-up cost, because it is the only
/// code that sees the phase boundaries -- loading the native partner, running every
/// module's resolver (the publish handshake), and flipping the stub pages.
static long echoNowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/// Priority 101: before any demo/library constructor that might issue a call.
static __attribute__((constructor(101))) void echoBootstrap(void)
{
    const int timing = getenv("ECHO_TIME") != NULL;
    long t0 = timing ? echoNowMs() : 0, tmark = t0;
    /// Any emulator that answers the attribute query has the dlcall syscall wired up:
    /// an emulator without it leaves the magic syscall unhandled, the kernel returns
    /// -ENOSYS, and `emu` stays NULL. So the check is "did anyone answer", not "which
    /// emulator" -- qemu, box64 and FEX all reach here and each reports its own name.
    const char *emu = EchoGetHostAttribute("emu");
    if (emu == NULL) {
        fprintf(stderr, "x64echo: not running under a dlcall-capable emulator\n");
        abort();
    }
    if (timing)
        fprintf(stderr, "x64echo: emulator reports '%s'\n", emu);

    /// Two candidates: the `_host`-tagged name (tests / same-dir layout) and the
    /// plain basename (productization: native keeps its real name on a separate
    /// host path). Try the tagged one first, then fall back to the plain name.
    char namebuf[256], plainbuf[256];
    const char *lib = echoResolveNativeLib(namebuf, sizeof namebuf);
    const char *plain = echoFaceBasename(plainbuf, sizeof plainbuf);

    /// ECHO_DEEPBIND=1 adds RTLD_DEEPBIND to the native load. Needed when the HOST
    /// process already contains a DIFFERENT copy of the library under test: on an
    /// aarch64 host, qemu itself pulls in libglib, which pulls in the system
    /// libpcre2 (and libz). That copy is loaded first and globally, so our native
    /// half's own plain-named symbols lose to it and the calls land in the wrong
    /// version -- pcre2 10.47 face + test binary served by the system's 10.42, which
    /// bootstraps cleanly and then fails a third of the suite for no visible reason.
    /// Off by default: DEEPBIND also defeats deliberate interposition, and every
    /// library that does NOT collide with the emulator's own dependencies is better
    /// off without it.
    int flags = RTLD_NOW;
    {
        const char *db = getenv("ECHO_DEEPBIND");
        if (db && db[0] == '1')
            flags |= RTLD_DEEPBIND;
    }
    void *h = lib ? EchoLoadLibrary(lib, flags) : NULL;

    /// Capture the primary (_host-tagged) load's error the instant it fails -- reading
    /// it clears the host dlerror, so we must stash it before any other host call.
    /// Only fall back to the plain basename when the _host native simply is NOT PRESENT
    /// (productization layout: the native keeps its real name on a separate host path).
    /// If the _host native EXISTS but failed to load (e.g. an undefined symbol), that is
    /// the real error the user needs -- do NOT fall back to the plain name, which in a
    /// shared face+native directory would resolve to the FACE itself on the host, whose
    /// constructor re-enters this bootstrap in host context (magic syscall unintercepted
    /// -> emu==NULL) and aborts with a misleading "not dlcall-capable", masking the true
    /// error. (glibc's dlerror says "...: cannot open shared object file: No such file
    /// or directory" for a missing file.)
    char primary_err[512];
    primary_err[0] = '\0';
    int missing = (lib == NULL);
    if (h == NULL && lib) {
        const char *e = EchoGetLibraryError();
        snprintf(primary_err, sizeof primary_err, "%s", e ? e : "");
        missing = (strstr(primary_err, "No such file") != NULL);
    }
    if (h == NULL && missing && plain && (lib == NULL || strcmp(plain, lib) != 0)) {
        h = EchoLoadLibrary(plain, flags);
        if (h)
            lib = plain;
    }
    if (h == NULL) {
        fprintf(stderr, "x64echo: failed to load native lib '%s': %s\n",
                lib ? lib : (plain ? plain : "?"),
                primary_err[0] ? primary_err : EchoGetLibraryError());
        abort();
    }

    if (timing) {
        fprintf(stderr, "x64echo-time: native dlopen %ldms\n", echoNowMs() - tmark);
        tmark = echoNowMs();
    }

    echo_host_entry = EchoGetProcAddress(h, "__echo_entry");
    if (echo_host_entry == NULL) {
        fprintf(stderr, "x64echo: native lib missing __echo_entry: %s\n",
                EchoGetLibraryError());
        abort();
    }

    /// Bind operator new/delete to the host-owned shared arena before any object
    /// crosses (01-abi-spec §8.3).
    __echo_bind_arena(h);

    /// Run every pluggable guest module's bootstrap hook (the `echo_boot` section),
    /// each paired with a host counterpart via __echo_call_host. The core names no
    /// module here -- e.g. the guest stdio module (GuestStdio.c) registers its
    /// standard streams with the host stdio bridge through its own hook.
    for (EchoBootHook *b = __start_echo_boot; b < __stop_echo_boot; ++b)
        (*b)(h);

    if (timing) {
        fprintf(stderr, "x64echo-time: arena+boot hooks %ldms\n", echoNowMs() - tmark);
        tmark = echoNowMs();
    }

    /// Run every module's resolver (one per face translation unit).
    unsigned nres = 0;
    for (EchoResolver *r = __start_echo_resolvers; r < __stop_echo_resolvers; ++r) {
        (*r)(h);
        ++nres;
    }
    if (timing) {
        fprintf(stderr, "x64echo-time: %u resolvers (publish handshake) %ldms\n",
                nres, echoNowMs() - tmark);
        tmark = echoNowMs();
    }

    /// Make the self-identifying stubs executable (02-implementation 地基 D §3). They are emitted
    /// into a data section (R/W after load); flip its pages to R+X so a guest
    /// `call` into a stub runs. (Decay the section-bound arrays to pointers so
    /// this is a pointer comparison, not a deprecated array comparison.)
    char *stubsLo = __start_echo_stubs;
    char *stubsHi = __stop_echo_stubs;
    if (stubsLo && stubsHi > stubsLo) {
        uintptr_t pg = (uintptr_t) sysconf(_SC_PAGESIZE);
        uintptr_t s = (uintptr_t) stubsLo & ~(pg - 1);
        uintptr_t e = ((uintptr_t) stubsHi + pg - 1) & ~(pg - 1);
        if (mprotect((void *) s, (size_t) (e - s), PROT_READ | PROT_EXEC) != 0) {
            fprintf(stderr, "x64echo: mprotect echo_stubs failed\n");
            abort();
        }
        /// Which range actually became executable. A stub that faults with SEGV_ACCERR
        /// is either outside this range (a second face library whose bootstrap has not
        /// run yet) or inside it and re-protected afterwards; the range is the only way
        /// to tell those apart, and neither shows up in a core dump's program headers.
        if (getenv("ECHO_DEBUG"))
            fprintf(stderr, "x64echo: echo_stubs %p..%p now R+X (section %p..%p)\n",
                    (void *) s, (void *) e, (void *) stubsLo, (void *) stubsHi);

        /// The guest mprotect above is not enough on its own. qemu-user forwards it to
        /// the host with PROT_EXEC stripped, which is right for qemu (guest code runs
        /// through TCG, the host never executes those bytes) but leaves the pages
        /// non-executable for the HOST -- and the host lands on a stub whenever a call
        /// site reaches one without going through __echo_check_call. The process then
        /// dies with SEGV_ACCERR at whatever stub happens to sit at that address, and
        /// nothing points at the cause: the core dump shows R--, this diagnostic shows
        /// R+X was requested, and only an strace of the host reveals the EXEC bit going
        /// missing in transit. So ask the host to redo it in its own currency.
        void (*hostExec)(void *, void *) =
            (void (*)(void *, void *)) EchoGetProcAddress(h, "__echo_host_exec_stubs");
        if (hostExec)
            EchoInvokeProc((void *) hostExec, (void *) s, (void *) e);
    }

    echo_native_handle = h; /// kept for the phase-3 commit below

    if (timing)
        fprintf(stderr, "x64echo-time: stub mprotect %ldms; TOTAL %ldms\n",
                echoNowMs() - tmark, echoNowMs() - t0);

    fprintf(stderr, "x64echo: bootstrap ok (native=%s)\n", lib);
}

/// docs/07 §5 phase 3 (mutable storage sync). Runs with NO priority, and this object is
/// linked AFTER the library's own translation units -- prioritised constructors sort ahead
/// of unprioritised ones, and unprioritised ones run in link order, so this is the LAST
/// constructor of the face .so: every one of its static initialisers has finished writing
/// the (canonical) face copies. Cross once and let the host copy each synced mutable
/// global's NATIVE copy -- initialised during the host dlopen in echoBootstrap, i.e.
/// before this face published its slots -- over the face copy, which the face's own x86
/// static init has meanwhile overwritten with a duplicate. After this, canonical storage
/// holds the value the native world initialised, and both worlds read it.
static __attribute__((constructor)) void echoCommitMutInit(void)
{
    if (echo_native_handle)
        __echo_call_host(echo_native_handle, "echo_commit_mutinit", NULL, NULL);
}
