//===- Arena/Arena.c - shared cross-boundary heap arena -------------------===//
//
// A small first-fit free-list allocator over one shared region (01-abi-spec §8.3).
// Correctness-first: a single free list, no coalescing (fragments over time, fine
// for the current milestone). The control block lives at the region start so both
// worlds operate on the same state through their bound pointer. Compiled into both
// world runtimes; the region itself is created once by the host and published.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#include <echo/ABI/Arena.h>

/// Each block: [ Block header | payload ]. When free, `next` links the free list;
/// when allocated, the payload starts right after `size`. `size` is the whole
/// block size (header included), 16-byte rounded.
typedef struct Block {
    size_t size;         /* total block size incl. header, 16-aligned */
    struct Block *next;  /* free-list link (valid only while free)    */
} Block;

struct EchoArena {
    uintptr_t base;      /* region start (== this)      */
    uintptr_t end;       /* region end                  */
    uintptr_t bump;      /* next never-yet-allocated byte */
    Block *freelist;     /* freed blocks                */
    volatile int lock;   /* test-and-set spinlock       */
};

/* Payload begins after `size`; the header reserves 16 bytes so payloads stay
 * 16-aligned. */
#define ECHO_HDR 16
#define ECHO_ALIGN(n) (((n) + 15u) & ~(size_t) 15u)

/// This world's bound arena (host and guest each hold a pointer to the same
/// shared control block).
static EchoArena *g_arena;

static void lock(EchoArena *a)
{
    while (__atomic_test_and_set(&a->lock, __ATOMIC_ACQUIRE))
        ;
}
static void unlock(EchoArena *a) { __atomic_clear(&a->lock, __ATOMIC_RELEASE); }

EchoArena *echo_arena_create(size_t bytes)
{
    void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;
    EchoArena *a = (EchoArena *) mem;
    a->base = (uintptr_t) mem;
    a->end = a->base + bytes;
    a->bump = ECHO_ALIGN(a->base + sizeof(EchoArena));
    a->freelist = NULL;
    a->lock = 0;
    return a;
}

void echo_arena_bind(EchoArena *arena) { g_arena = arena; }

int echo_arena_owns(const void *p)
{
    EchoArena *a = g_arena;
    uintptr_t u = (uintptr_t) p;
    return a && u >= a->base && u < a->end;
}

void *echo_arena_alloc(size_t n)
{
    EchoArena *a = g_arena;
    if (!a)
        return NULL;
    size_t need = ECHO_ALIGN(n + ECHO_HDR);
    lock(a);
    /* first fit over the free list */
    Block **pp = &a->freelist;
    for (Block *b = a->freelist; b; pp = &b->next, b = b->next) {
        if (b->size >= need) {
            *pp = b->next;
            unlock(a);
            return (void *) ((uintptr_t) b + ECHO_HDR);
        }
    }
    /* bump-allocate a fresh block */
    if (a->bump + need <= a->end) {
        Block *b = (Block *) a->bump;
        b->size = need;
        a->bump += need;
        unlock(a);
        return (void *) ((uintptr_t) b + ECHO_HDR);
    }
    unlock(a);
    return NULL;
}

void echo_arena_free(void *p)
{
    EchoArena *a = g_arena;
    if (!p || !a)
        return;
    Block *b = (Block *) ((uintptr_t) p - ECHO_HDR);
    lock(a);
    b->next = a->freelist;
    a->freelist = b;
    unlock(a);
}
