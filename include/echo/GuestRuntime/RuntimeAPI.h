//===- echo/GuestRuntime/RuntimeAPI.h - guest-side runtime API (compiler-emitted) -===//
//
// The guest (face .so) runtime-support routines the Echo compiler emits calls to
// (libgcc/libsupc++ analog). Guest-only; the host must not see them. Shared data
// layout is in echo/ABI/CallBlock.h; the dlcall client is in echo/GuestRuntime/DLCall.h.
//
// Names are ABI contracts (the Echo compiler emits calls to them); not subject to
// the project's C++ naming style.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_GUESTRUNTIME_RUNTIMEAPI_H
#define ECHO_GUESTRUNTIME_RUNTIMEAPI_H

#include <echo/ABI/CallBlock.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Crossing primitive invoked by a face stub: issue the dlcall magic syscall and
/// drive the reentry loop until the host call completes.
void __echo_cross(long iid, EchoCallBlock *blk);

/// Resolve the native @publish_name (a module's __echo_publish_<srchash>), cross
/// once to fill `table` by NAME (matching @names against the native module's
/// directory). @version is this module's worklist hash; a native mismatch is only
/// a note (arch-conditional code legitimately diverges), not an abort.
void __echo_bind_via_publish(void *handle, void *table, unsigned int count,
                           unsigned long long version, const char *publish_name,
                           const char **names);

/// Hand the whole table to the native __echo_set_facevts in one INVOKE_PROC to
/// reverse-publish face vtable address points / guest redispatchers.
void __echo_publish_facevts(void *handle, const EchoFaceVtEntry *tab,
                          unsigned int n);

/// Bind this world's operator new/delete to the host-owned shared arena.
void __echo_bind_arena(void *handle);

/// A bootstrap-time hook a pluggable guest module registers in the linker-collected
/// `echo_boot` section; the bootstrap calls each with the native handle after the
/// handshake, so the module can pair with its host counterpart (see __echo_call_host).
typedef void (*EchoBootHook)(void *native_handle);

/// Generic cross-to-host primitive: resolve host symbol `sym` and INVOKE_PROC it as
/// host_fn(a1, a2). The runtime's module-agnostic abstraction; no-op if unresolved.
void __echo_call_host(void *handle, const char *sym, void *a1, void *a2);

/// Called from a face stub when status == ECHO_PENDING_EXCEPTION: throw a guest
/// exception matching the tagged type, unwinding out to the guest's catch.
void __echo_rethrow(EchoException *exc);

/// Drop the host's temporary keep-ref for a shared guest exception after this
/// world has acquired its own exception_ptr reference.
void __echo_release_host_exception(void *obj);

/// Run host-side TSD cleanup while the current guest pthread can still service
/// callbacks. Called by the guest pthread preload wrapper before start returns.
void __echo_thread_drain(void);

/// Reverse of the C-3 tunnel: called from the guest redispatcher's catch-all
/// landingpad when a reentered guest callback throws. Records the in-flight guest
/// exception into the channel the host handed in (EchoReentry.varargs), so the host
/// can rethrow a matching exception on its side (04-reentry).
void __echo_catch_reentry(EchoException *exc, void *obj);

#ifdef __cplusplus
}
#endif

#endif // ECHO_GUESTRUNTIME_RUNTIMEAPI_H
