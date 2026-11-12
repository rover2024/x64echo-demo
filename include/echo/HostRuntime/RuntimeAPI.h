//===- echo/HostRuntime/RuntimeAPI.h - host-side runtime API (compiler-emitted) --===//
//
// The host (native .so) runtime-support routines the Echo compiler emits calls to
// (libgcc/libsupc++ analog). Host-only; the guest must not see them (it reaches
// __echo_entry / __echo_publish / __echo_set_facevts by dlsym name, never as C
// symbols). Shared data layout is in echo/ABI/CallBlock.h.
//
// Names are ABI contracts (the Echo compiler emits calls to them); not subject to
// the project's C++ naming style.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_HOSTRUNTIME_RUNTIMEAPI_H
#define ECHO_HOSTRUNTIME_RUNTIMEAPI_H

#include <echo/ABI/CallBlock.h>

#ifdef __cplusplus
#include <exception>
extern "C" {
#endif

/// The single host entry that dlcall INVOKE_PROC targets. Shape matches dlcall's
/// void(void*, void*): a1 = iid, a2 = EchoCallBlock*.
void __echo_entry(void *a1, void *a2);

/// Host-side reenter primitive (04-reentry §2.4): from within a running entry thunk,
/// park the host coroutine and switch back to the guest, which runs the
/// dispatcher and resumes via ECHO_IID_RESUME. Returns once the guest call ends.
void __echo_reenter(EchoReentry *re);

/// Translate a native indirect/virtual call target: a face stub -> the real
/// native body (*target_slot), else the address unchanged.
void *__echo_check_call(void *addr);

/// Classify host vs guest (04-reentry §2.6): 1 if addr is in a host-loaded object.
int __echo_is_host(void *addr);

/// Reverse-publish handler: fill this native lib's reverse-published slots
/// (__echo_facevt$<vtable> / __echo_redisp$<key>) from a guest-built table in one
/// crossing. Each entry's name is dlsym-resolved; the slot is set to its ap.
void __echo_set_facevts(const EchoFaceVtEntry *tab, unsigned int n);

/// docs/07 §6 deep-home registration. A native module's load-time ctor calls this with
/// its Echo-compiler-emitted table of { void **slot; unsigned long field_off; void *nat; }
/// descriptors -- one per exported-const-object pointer field that targets an internal
/// data blob (a QMetaObject SMO's d.data/stringdata). echo_apply_smo_patches (run at the
/// publish handshake) writes nat into (*slot)+field_off, homing the blob at the host.
void __echo_register_smo_patches(const void *tab, unsigned int n);

/// Remove a module's deep-home descriptor table before that module is unmapped.
void __echo_unregister_smo_patches(const void *tab);

/// Tag the in-flight C++ exception into blk->eptr and mark the block pending
/// (called from a thunk's catch-all handler). `obj` is the exception object from
/// __cxa_begin_catch, used to copy-construct a faithful copy (Tier-2).
void __echo_catch_current(EchoCallBlock *blk, void *obj);

/// Drop one keep-ref installed by __echo_catch_current for a raw guest exception.
void __echo_release_kept_exception(void *obj);

/// Reverse of the C-3 tunnel: called from the native dispatcher after a host->guest
/// reentry whose guest callback threw. Rebuilds and throws a matching HOST exception
/// (unwinds out to the host's catch). Never returns when one was captured.
void __echo_rethrow_reentry(EchoException *exc);

/// Emitted by the Echo compiler in native mode (as __echo_publish_<hash> per
/// module): fill the binding table + version. Reached by the guest via dlsym.
void __echo_publish(EchoPublish *p);

/// Return the host-owned shared arena root (creating it on first call). The face
/// bootstrap dlsym's + INVOKE's this to bind the guest's operator new/delete.
void __echo_arena_root(void **out);

#ifdef __cplusplus
}

/// Host-side carrier for a GUEST-origin exception (ECHO_EXC_GUEST_ORIGIN) crossing the
/// host on its round-trip. It holds only the guest's retrieval token: the host never
/// needs the concrete object, just to unwind its own frames and hand the token back.
/// __echo_rethrow_reentry throws it; the outer __echo_catch_current recognises it (by
/// typeid) and carries the token into the guest-bound channel. Derives from std::exception
/// so generic host exception handling in a native library it unwinds through (QtConcurrent
/// wrapping a worker exception, a catch(std::exception&), a what() probe) stays well-defined
/// -- if it gets wrapped/swallowed the token is simply lost and the guest falls back to a
/// message-only rethrow, never a crash. See HostRT/{ReentryRethrow,Catch}.cpp.
struct EchoGuestOriginException : std::exception {
    unsigned long token;
    explicit EchoGuestOriginException(unsigned long t) : token(t) {}
    const char *what() const noexcept override { return "x64echo guest-origin exception"; }
};
#endif

#endif // ECHO_HOSTRUNTIME_RUNTIMEAPI_H
