//===- Arena/Operators.cpp - operator new/delete over the unified heap -----===//
//
// Strong global operator new/delete that override libstdc++'s weak ones on both
// sides, routing every C++ allocation to malloc/free. Both worlds end up on ONE
// allocator — the host libc heap — because the guest's malloc is interposed by
// the dependency-free preload shim (tests/echoheap_shim.c) that forwards to the
// host libc, while host-side malloc is the host libc directly. So an object
// allocated in one world is freeable in the other (01-abi-spec §8.3). Compiled
// into each world runtime. OOM aborts (keeping the runtime -fno-exceptions).
//
// (This replaces the earlier bespoke shared-arena allocator: a real allocator was
// needed once the C malloc family had to cross too, and reusing the host libc via
// the shim gives that for free while unifying both heaps into one.)
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstdlib>
#include <new>

static inline void *echo_new(std::size_t n)
{
    void *p = std::malloc(n ? n : 1);
    if (!p)
        std::abort();
    return p;
}

static inline void echo_del(void *p) noexcept
{
    if (p)
        std::free(p);
}

void *operator new(std::size_t n) { return echo_new(n); }
void *operator new[](std::size_t n) { return echo_new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return echo_new(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return echo_new(n); }

void operator delete(void *p) noexcept { echo_del(p); }
void operator delete[](void *p) noexcept { echo_del(p); }
void operator delete(void *p, std::size_t) noexcept { echo_del(p); }
void operator delete[](void *p, std::size_t) noexcept { echo_del(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { echo_del(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { echo_del(p); }
