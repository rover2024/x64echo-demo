//===- HostRT/Catch.cpp - cross-boundary exception tunnel (host half) -----===//
//
// A native entry thunk wraps the real call in invoke + a catch-all landingpad;
// in the handler it calls __echo_catch_current(blk, obj), which records the
// in-flight exception into blk->eptr and marks the block pending. The matching
// guest-side rethrow lives in GuestRT/Rethrow.cpp. (01-abi-spec §7.)
//
// Tier-2 (faithful): if the concrete type is registered by the Echo compiler as
// __echo_exc$<type_info::name()> = { size, type_info, copy-ctor, dtor }, we
// copy-construct the object into a unified-heap buffer and record its type name,
// so the guest can rebuild the real object and rethrow it by its true type.
// Tier-1 (fallback): otherwise we classify by std:: type and copy what().
//
// NOTE: this file genuinely needs C++ exceptions; CMake compiles it with
// -fexceptions even though the project disables them by default.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <mutex>
#include <typeinfo>
#include <utility>
#include <vector>

#include <cxxabi.h>
#include <dlfcn.h>

#include "RuntimeAPI.h"

/// Compiler-emitted per-exception-type descriptor (matches CGException.cpp).
struct EchoExcDesc {
    unsigned long size;
    const void *type_info;
    void (*copy)(void *dst, const void *src); // placement copy-ctor (C1)
    void (*dtor)(void *);                      // complete dtor (D1)
};

using EchoKeptException = std::pair<void *, std::exception_ptr>;

/// Heap-own the registry infrastructure so process teardown never destroys a
/// still-live guest exception from host context. Entries normally have a short
/// lifetime and are removed as soon as the guest has acquired its own ref.
static std::vector<EchoKeptException> &keptExceptions()
{
    static auto *keep = new std::vector<EchoKeptException>;
    return *keep;
}

static std::mutex &keptExceptionsMutex()
{
    static auto *mutex = new std::mutex;
    return *mutex;
}

extern "C" void __echo_release_kept_exception(void *obj)
{
    std::lock_guard<std::mutex> lock(keptExceptionsMutex());
    auto &keep = keptExceptions();
    for (auto i = keep.begin(); i != keep.end(); ++i) {
        if (i->first != obj)
            continue;
        keep.erase(i); // guest already owns a ref, so this cannot destroy the object
        return;
    }
}

static void tag(EchoException *exc, int type, const char *what)
{
    exc->type = type;
    std::strncpy(exc->what, what, sizeof exc->what - 1);
    exc->what[sizeof exc->what - 1] = 0;
}

/// Translate a face self-stub to the real native body (echo_bind slot); a plain
/// host/guest address passes through. Defined in CheckCall.c.
extern "C" void *__echo_check_call(void *addr);
/// Classify an address as host body vs guest. Defined in CheckCall.c.
extern "C" int __echo_is_host(void *addr);

/// std::exception::what() through a POSSIBLY-REDIRECTED exception object. The host
/// runtime is not Echo-compiled, so a plain e.what() is a raw indirect call to the
/// object's vtable slot; for a host exception constructed with a C-1 redirected vptr
/// that slot is a face self-stub whose code unconditionally crosses to the guest --
/// fatal from host context. Read the slot and run it through __echo_check_call so a
/// stub resolves to the native what(); a native vptr is unchanged. what() is the
/// third vtable entry (after the two destructors) in the Itanium std::exception
/// layout that libstdc++ uses, and an override keeps that index in every subclass.
static const char *echoWhat(const std::exception &e)
{
    void *const *vptr = *reinterpret_cast<void *const *const *>(&e);
    void *slot = __echo_check_call(vptr[2]); // std::exception::what()
    // A GUEST-origin exception (a guest-defined type bouncing back through the host on a
    // shared __cxa unwinder) has a what() that is raw GUEST code -- calling it directly
    // from host context runs x86 bytes as arm64 and crashes. Don't; a placeholder keeps
    // the Tier-1 channel well-formed (the guest still carries the true type separately).
    if (!__echo_is_host(slot))
        return "x64echo guest exception";
    return reinterpret_cast<const char *(*)(const void *)>(slot)(&e);
}

/// Fill the Tier-1 message channel from the in-flight exception (already being
/// handled), by rethrowing and classifying.
static void tier1(EchoException *exc)
{
    try {
        throw;
    } catch (const std::invalid_argument &e) {
        tag(exc, ECHO_EXC_INVALID_ARGUMENT, echoWhat(e));
    } catch (const std::exception &e) {
        tag(exc, ECHO_EXC_UNKNOWN, echoWhat(e));
    } catch (...) {
        tag(exc, ECHO_EXC_UNKNOWN, "unknown host exception");
    }
}

/// Called from a thunk's catch handler (inside __cxa_begin_catch). `obj` is the
/// exception object. Record it into blk->eptr and mark the block pending.
void __echo_catch_current(EchoCallBlock *blk, void *obj)
{
    EchoException *exc = (EchoException *) blk->eptr;
    if (exc) {
        exc->type_name = nullptr;
        exc->obj = nullptr;
        exc->size = 0;

        // Tier-2: copy-construct a faithful copy keyed by the dynamic type.
        const std::type_info *ti = abi::__cxa_current_exception_type();

        // A GUEST-origin exception rounding back through the host: carry its token
        // through unchanged, so the guest rethrows the ORIGINAL by its true type.
        if (ti && *ti == typeid(EchoGuestOriginException)) {
            exc->type = ECHO_EXC_GUEST_ORIGIN;
            exc->obj = (void *) ((EchoGuestOriginException *) obj)->token;
            blk->status = ECHO_PENDING_EXCEPTION;
            return;
        }

        // A raw GUEST exception object bouncing back through host frames on the shared
        // __cxa unwinder (a guest type rethrown inside a reentry that unwinds a host
        // callee). Its type_info/vtable are guest code: host catch-matching (tier1's
        // `throw; catch(std::exception&)`) or what() would run x86 from arm64 and crash.
        // Marshal a type-only message without touching it -- the guest loses the true
        // type on this bounce but does not crash. (A faithful async round-trip through
        // host frames is a separate, harder problem.)
        if (ti && obj && !__echo_is_host((void *) ti)) {
            // A GUEST exception object (a guest type rethrown on the host by a native
            // std::rethrow_exception, or bouncing back through host frames): its type_info /
            // vtable / dtor are guest code, so host EH machinery on it (tier1's catch-match,
            // what(), the thunk's __cxa_end_catch destructor) would run x86 from arm64 and
            // crash. The __cxa exception is shared, so keep a ref (a live std::exception_ptr,
            // so the thunk's __cxa_end_catch does not destroy it) and hand the object pointer
            // back; the guest rethrows it faithfully by its true type.
            {
                std::lock_guard<std::mutex> lock(keptExceptionsMutex());
                keptExceptions().emplace_back(obj, std::current_exception());
            }
            exc->type = ECHO_EXC_HOST_RETHROW;
            exc->type_name = nullptr;
            exc->obj = obj;
            blk->status = ECHO_PENDING_EXCEPTION;
            return;
        }
        if (ti && obj) {
            std::string sym = std::string("__echo_exc$") + ti->name();
            auto *d = (const EchoExcDesc *) dlsym(RTLD_DEFAULT, sym.c_str());
            if (d && d->copy) {
                void *buf = std::malloc(d->size);
                if (buf) {
                    d->copy(buf, obj);
                    exc->type_name = ti->name();
                    exc->obj = buf;
                    exc->size = d->size;
                }
            }
        }

        // Tier-1 only when we could NOT capture the concrete object. tier1
        // virtual-dispatches what(), which for a C-1-redirected object would jump
        // into a guest face-vtable stub from host context; the Tier-2 path already
        // carries the full object, so skip it there.
        if (!exc->obj)
            tier1(exc);
    }
    blk->status = ECHO_PENDING_EXCEPTION;
}
