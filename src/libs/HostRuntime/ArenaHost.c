//===- HostRT/ArenaHost.c - host creates & publishes the shared arena -----===//
//
// The host owns the shared heap arena (01-abi-spec §8.3): it creates the region
// once, binds its own operator new/delete to it, and hands the root to the guest
// via __echo_arena_root (dlsym'd + INVOKE'd by the face bootstrap). Both worlds
// then allocate/free through the same arena.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stddef.h>

#include <echo/ABI/Arena.h>

/// 256 MiB reserved region (lazily mmap'd on first use; only touched pages fault
/// in, so the cost is small until the boundary actually allocates).
#define ECHO_ARENA_BYTES ((size_t) 256 << 20)

static EchoArena *g_host_arena;

/// Return the shared arena root (creating + binding it on first call). Invoked by
/// the guest bootstrap via dlcall INVOKE_PROC(out, _): the host writes *out.
void __echo_arena_root(void **out)
{
    if (!g_host_arena) {
        g_host_arena = echo_arena_create(ECHO_ARENA_BYTES);
        echo_arena_bind(g_host_arena);
    }
    if (out)
        *out = g_host_arena;
}
