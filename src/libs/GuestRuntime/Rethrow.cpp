//===- GuestRT/Rethrow.cpp - cross-boundary exception tunnel (guest half) -===//
//
// After echo_cross returns, the face stub checks blk->status and, if it is
// ECHO_PENDING_EXCEPTION, calls __echo_rethrow(eptr), which throws a guest
// exception that unwinds out through the stub into the guest's own catch. The
// host-side capture lives in HostRT/Catch.cpp. (01-abi-spec §7.)
//
// Tier-2 (faithful): if the concrete type is registered here as
// __echo_exc$<type_info::name()>, copy-construct a real object of that type from
// the host-copied buffer, dispose the buffer (its members live on the unified
// heap, so the guest destructor + free suffice), and __cxa_throw it by its true
// type_info — a guest `catch (Concrete&)` matches and the members survive.
// Tier-1 (fallback): otherwise throw a matching std:: exception with the message.
//
// NOTE: this file genuinely needs C++ exceptions; CMake compiles it with
// -fexceptions even though the project disables them by default.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <dlfcn.h>

#include "RuntimeAPI.h"

extern "C" void *__cxa_allocate_exception(unsigned long) noexcept;
extern "C" [[noreturn]] void __cxa_throw(void *, void *, void (*)(void *));
/// libstdc++ std::__exception_ptr::exception_ptr::exception_ptr(void *) -- wrap a shared
/// __cxa exception OBJECT in an exception_ptr (bumps its refcount). No public API exists for
/// this, but the symbol is stable; used to rethrow a host-kept guest exception by true type.
extern "C" void _ZNSt15__exception_ptr13exception_ptrC1EPv(void *self, void *obj);

/// Rethrow a guest exception the host caught+kept, by its true type (shared __cxa object).
[[noreturn]] static void echoRethrowHostKept(void *obj)
{
    std::exception_ptr ep;                                 // null
    _ZNSt15__exception_ptr13exception_ptrC1EPv(&ep, obj);  // wrap obj (addref)
    __echo_release_host_exception(obj);                    // host ref no longer needed
    std::rethrow_exception(ep);                            // noreturn; catch(True&) matches
}

/// Rethrow a stashed guest-origin exception by token (GuestRT/ReentryCatch.cpp); returns
/// 0 without throwing only if the token is unknown.
extern "C" int echo_unstash_guest_exc(unsigned long id);

/// Compiler-emitted per-exception-type descriptor (matches CGException.cpp).
struct EchoExcDesc {
    unsigned long size;
    void *type_info;
    void (*copy)(void *dst, const void *src); // placement copy-ctor (C1)
    void (*dtor)(void *);                      // complete dtor (D1)
};

[[noreturn]] static void tier1(EchoException *exc)
{
    if (exc->type == ECHO_EXC_INVALID_ARGUMENT)
        throw std::invalid_argument(exc->what);
    throw std::runtime_error(std::string("x64echo foreign exception: ") + exc->what);
}

/// Called from a face stub when status == ECHO_PENDING_EXCEPTION.
void __echo_rethrow(EchoException *exc)
{
    if (exc == nullptr)
        return;

    // A guest exception object the host caught (native std::rethrow_exception of a guest
    // exception_ptr) and kept alive: rethrow the shared __cxa exception by its true type.
    if (exc->type == ECHO_EXC_HOST_RETHROW)
        echoRethrowHostKept(exc->obj); // noreturn

    // Guest-origin round-trip: the guest stashed the live exception on the way out; rethrow
    // the ORIGINAL by its true type so a guest `catch (Concrete&)` matches.
    if (exc->type == ECHO_EXC_GUEST_ORIGIN) {
        if (echo_unstash_guest_exc((unsigned long) (unsigned long) exc->obj))
            return;      // unreachable: rethrew the stashed exception
        tier1(exc);      // stash miss (native swallowed + re-raised?): message fallback
    }

    // Tier-2: rebuild the real object and throw it by its true type.
    if (exc->obj && exc->type_name) {
        std::string sym = std::string("__echo_exc$") + exc->type_name;
        auto *d = (const EchoExcDesc *) dlsym(RTLD_DEFAULT, sym.c_str());
        if (d && d->copy) {
            void *gobj = __cxa_allocate_exception(d->size);
            d->copy(gobj, exc->obj);       // guest copy-ctor: deep copy + guest vptr
            if (d->dtor)
                d->dtor(exc->obj);         // destruct the host-copied buffer...
            std::free(exc->obj);           // ...and free it (unified heap)
            __cxa_throw(gobj, d->type_info, d->dtor); // real type -> catch(Concrete&)
        }
        // Guest doesn't know the type: can't reconstruct. Free without a dtor
        // (leaks any owning members) and fall through to Tier-1.
        std::free(exc->obj);
    }

    tier1(exc);
}
