//===- echo/ABI/Arena.h - shared cross-boundary heap arena ----------------===//
//
// The unified heap (01-abi-spec §8.3). Both worlds override operator new/delete
// to route C++ allocations here, so an object allocated on one side (e.g. a
// host-returned std::string's buffer) can be freed on the other. The arena lives
// in one shared region (single address space); the host creates it and publishes
// its root at the handshake, and each side binds a process-global pointer to it.
//
// Only C++ operator new/delete are routed (they are weak in libstdc++, so a
// strong definition wins). C malloc/free are not interposed; an allocation made
// before the arena is bound falls back to malloc and is freed by ownership check.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_ABI_ARENA_H
#define ECHO_ABI_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The arena control block, placed at the start of the shared region.
typedef struct EchoArena EchoArena;

/// Host: reserve `bytes` and initialise an arena over it. Returns its root (a
/// pointer valid in the shared address space), or NULL on failure.
EchoArena *echo_arena_create(size_t bytes);

/// Set this world's process-global arena pointer (host after create, guest after
/// the handshake publishes the root). Idempotent.
void echo_arena_bind(EchoArena *arena);

/// 1 if `p` was allocated from the bound arena (an in-region pointer).
int echo_arena_owns(const void *p);

/// Allocate `n` bytes from the bound arena (16-byte aligned). NULL if unbound or
/// out of space — callers (operator new) fall back to malloc when unbound.
void *echo_arena_alloc(size_t n);

/// Free a pointer previously returned by echo_arena_alloc.
void echo_arena_free(void *p);

#ifdef __cplusplus
}
#endif

#endif // ECHO_ABI_ARENA_H
