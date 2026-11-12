//===- GuestRT/ReentryCatch.cpp - guest capture for exception-across-reentry -===//
//
// The REVERSE of the C-3 tunnel (04-reentry). A host function calls a guest callback
// (host->guest reentry) inside try/catch; when the guest callback throws, the guest
// redispatcher (compiler-emitted) catches it in a landingpad and calls this to record
// the in-flight GUEST exception into the EchoException channel the host handed in via
// EchoReentry.varargs. The host then rethrows a matching exception on its side
// (HostRT/ReentryRethrow.cpp) so its `catch` runs. Mirror of HostRT/Catch.cpp, in the
// guest world.
//
// Tier-2 (faithful): a library class type registered by the compiler as
// __echo_exc$<type_info::name()> is copy-constructed into a unified-heap buffer.
// Builtin: int/double/... have no descriptor (the compiler emits none for non-class
// throws), so copy the trivial value by size. Tier-1: otherwise classify + copy what().
//
// NOTE: genuinely needs C++ exceptions; CMake compiles it with -fexceptions.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <typeinfo>
#include <unordered_map>

#include <cxxabi.h>
#include <dlfcn.h>

#include "RuntimeAPI.h"

// Guest-origin exception stash. When a plain-g++ guest throws its OWN concrete type into
// a native callback, the guest cannot marshal it faithfully (no __echo_exc$ descriptor).
// But the object is a live guest exception heading for a round-trip back to the guest, so
// keep the exception_ptr alive here keyed by a token; __echo_rethrow rethrows the ORIGINAL
// by token, so a guest `catch (Concrete&)` matches. The stash is GLOBAL (not thread-local):
// a native library may store the exception and re-raise it on ANOTHER thread (QtConcurrent
// / QFuture propagate a worker's exception to the result-reading thread), so the token must
// resolve from any thread. A token that never returns (native swallowed it) leaves one live
// entry -- a bounded, rare leak.
static std::mutex g_echoStashMx;
static unsigned long g_echoStashNext;
static std::unordered_map<unsigned long, std::exception_ptr> g_echoStash;

extern "C" unsigned long echo_stash_guest_exc(void)
{
    std::exception_ptr ep = std::current_exception();
    std::lock_guard<std::mutex> lk(g_echoStashMx);
    unsigned long id = ++g_echoStashNext;
    g_echoStash.emplace(id, ep);
    return id;
}

/// Rethrow the stashed exception for `id` by its true type; returns 0 (does not throw)
/// only if the token is unknown, so the caller can fall back.
extern "C" int echo_unstash_guest_exc(unsigned long id)
{
    std::exception_ptr ep;
    {
        std::lock_guard<std::mutex> lk(g_echoStashMx);
        auto it = g_echoStash.find(id);
        if (it == g_echoStash.end())
            return 0;
        ep = it->second;
        g_echoStash.erase(it);
    }
    std::rethrow_exception(ep); // noreturn
}

struct EchoExcDesc {
    unsigned long size;
    const void *type_info;
    void (*copy)(void *dst, const void *src);
    void (*dtor)(void *);
};

// Size of a builtin exception object from its single-char Itanium mangling (no
// descriptor is emitted for non-class throws; the value is trivially copyable).
unsigned long echoBuiltinExcSize(const char *n)
{
    if (!n || n[1])
        return 0; // only single-char builtin manglings
    switch (n[0]) {
    case 'b': case 'c': case 'a': case 'h': return 1;             // bool,char,s/u char
    case 's': case 't': case 'w': return sizeof(short) > 2 ? 2 : 2; // short,ushort,wchar
    case 'i': case 'j': return 4;                                  // int, unsigned
    case 'f': return 4;                                            // float
    case 'l': case 'm': case 'x': case 'y': return 8;             // long,ulong,ll,ull
    case 'd': return 8;                                            // double
    case 'e': case 'n': case 'o': return 16;                     // long double, __int128
    default:
        if (n[0] == 'P') return sizeof(void *);                  // any pointer
        return 0;
    }
}

static void tag(EchoException *exc, int type, const char *what)
{
    exc->type = type;
    std::strncpy(exc->what, what, sizeof exc->what - 1);
    exc->what[sizeof exc->what - 1] = 0;
}

static void tier1(EchoException *exc)
{
    try {
        throw;
    } catch (const std::invalid_argument &e) {
        tag(exc, ECHO_EXC_INVALID_ARGUMENT, e.what());
    } catch (const std::exception &e) {
        tag(exc, ECHO_EXC_UNKNOWN, e.what());
    } catch (...) {
        tag(exc, ECHO_EXC_UNKNOWN, "unknown guest exception");
    }
}

/// Called from the guest redispatcher's catch-all landingpad (inside
/// __cxa_begin_catch). `obj` is the exception object; `exc` is the channel the host
/// gave us (on its stack, shared address space).
void __echo_catch_reentry(EchoException *exc, void *obj)
{
    if (!exc)
        return;
    exc->type = ECHO_EXC_NONE;
    exc->type_name = nullptr;
    exc->obj = nullptr;
    exc->size = 0;
    exc->what[0] = 0;

    const std::type_info *ti = abi::__cxa_current_exception_type();
    if (ti && obj) {
        const char *name = ti->name();
        std::string sym = std::string("__echo_exc$") + name;
        auto *d = (const EchoExcDesc *) dlsym(RTLD_DEFAULT, sym.c_str());
        if (d && d->copy) {
            void *buf = std::malloc(d->size); // unified heap -> host-readable
            if (buf) {
                d->copy(buf, obj);
                exc->type_name = name;
                exc->obj = buf;
                exc->size = d->size;
            }
        } else if (unsigned long sz = echoBuiltinExcSize(name)) {
            void *buf = std::malloc(sz);
            if (buf) {
                std::memcpy(buf, obj, sz);
                exc->type_name = name;
                exc->obj = buf;
                exc->size = sz;
            }
        }
    }
    if (!exc->obj) {
        tier1(exc); // classify + copy what() (diagnostics; fallback if the round-trip cut)
        // No faithful descriptor for a guest-defined type: stash the live exception and
        // mark it guest-origin, so it rethrows by its true type when it comes back.
        if (ti && obj) {
            exc->type = ECHO_EXC_GUEST_ORIGIN;
            exc->type_name = nullptr;
            exc->obj = (void *) (unsigned long) echo_stash_guest_exc(); // retrieval token
        }
    }
}
