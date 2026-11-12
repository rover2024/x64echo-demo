//===- HostRT/ReentryRethrow.cpp - host rethrow for exception-across-reentry --===//
//
// The host half of exception-across-reentry (the reverse of the C-3 tunnel). After a
// host->guest reentry, the native dispatcher (compiler-emitted) checks the
// EchoException channel it passed in via EchoReentry.varargs; if the guest callback
// threw and the guest captured it (GuestRT/ReentryCatch.cpp), the dispatcher calls
// this to rebuild and throw a matching HOST exception, which unwinds out of the
// dispatcher into the host's own `catch`. Mirror of GuestRT/Rethrow.cpp, host-side.
//
// Tier-2 (faithful): a class type registered here as __echo_exc$<name>. Builtin:
// int/double/... carry no descriptor, so allocate + memcpy the value and throw it by
// the host's own typeid (so `catch (int)` matches). Tier-1: throw a std:: by message.
//
// NOTE: genuinely needs C++ exceptions; CMake compiles it with -fexceptions.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <typeinfo>

#include <dlfcn.h>

#include "RuntimeAPI.h"

extern "C" void *__cxa_allocate_exception(unsigned long) noexcept;
extern "C" [[noreturn]] void __cxa_throw(void *, void *, void (*)(void *));

struct EchoExcDesc {
    unsigned long size;
    void *type_info;
    void (*copy)(void *dst, const void *src);
    void (*dtor)(void *);
};

// Map a builtin's single-char Itanium mangling to its size + the HOST's type_info,
// so a value thrown in the guest (int/double/...) is rethrown by the host's own
// typeid and a host `catch (int)` matches.
static bool builtinInfo(const char *n, unsigned long *sz, const std::type_info **ti)
{
    if (!n || n[1])
        return false;
#define ECHO_BI(ch, T)                                                             \
    case ch:                                                                       \
        *sz = sizeof(T);                                                           \
        *ti = &typeid(T);                                                          \
        return true
    switch (n[0]) {
        ECHO_BI('b', bool);
        ECHO_BI('c', char);
        ECHO_BI('a', signed char);
        ECHO_BI('h', unsigned char);
        ECHO_BI('s', short);
        ECHO_BI('t', unsigned short);
        ECHO_BI('i', int);
        ECHO_BI('j', unsigned int);
        ECHO_BI('l', long);
        ECHO_BI('m', unsigned long);
        ECHO_BI('x', long long);
        ECHO_BI('y', unsigned long long);
        ECHO_BI('f', float);
        ECHO_BI('d', double);
        ECHO_BI('e', long double);
        ECHO_BI('w', wchar_t);
    default:
        return false;
    }
#undef ECHO_BI
}

[[noreturn]] static void tier1(EchoException *exc)
{
    if (exc->type == ECHO_EXC_INVALID_ARGUMENT)
        throw std::invalid_argument(exc->what);
    throw std::runtime_error(std::string("x64echo foreign exception: ") + exc->what);
}

/// Called from the native dispatcher after a reentry that threw. Rebuild the host
/// exception and throw it (unwinds out to the host's catch). Never returns when an
/// exception was captured.
void __echo_rethrow_reentry(EchoException *exc)
{
    if (!exc)
        return;

    // Guest-origin: the guest stashed the live exception and gave us only a token. Throw a
    // host carrier that unwinds the native frames between here and the outer boundary; the
    // outer __echo_catch_current hands the token back to the guest, which rethrows the real
    // exception by its true type.
    if (exc->type == ECHO_EXC_GUEST_ORIGIN)
        throw EchoGuestOriginException{(unsigned long) (unsigned long) exc->obj};

    if (exc->obj && exc->type_name) {
        // Tier-2: a registered class type -> copy-construct a real host object.
        std::string sym = std::string("__echo_exc$") + exc->type_name;
        auto *d = (const EchoExcDesc *) dlsym(RTLD_DEFAULT, sym.c_str());
        if (d && d->copy && d->type_info) {
            void *hobj = __cxa_allocate_exception(d->size);
            d->copy(hobj, exc->obj); // host copy-ctor: deep copy + host vptr
            if (d->dtor)
                d->dtor(exc->obj);
            std::free(exc->obj);
            exc->obj = nullptr;
            __cxa_throw(hobj, d->type_info, d->dtor);
        }
        // Builtin: allocate + memcpy the trivial value, throw by the host typeid.
        unsigned long sz;
        const std::type_info *ti;
        if (builtinInfo(exc->type_name, &sz, &ti)) {
            void *hobj = __cxa_allocate_exception(sz);
            std::memcpy(hobj, exc->obj, sz < exc->size ? sz : exc->size);
            std::free(exc->obj);
            exc->obj = nullptr;
            __cxa_throw(hobj, (void *) ti, nullptr);
        }
        // The host has no descriptor for this guest type and cannot rebuild it.
        // Its transport buffer is still unified-heap storage and must not leak.
        std::free(exc->obj);
        exc->obj = nullptr;
    }
    // Tier-1: throw a matching std:: exception carrying the message.
    if (exc->type != ECHO_EXC_NONE || exc->what[0])
        tier1(exc);
}
