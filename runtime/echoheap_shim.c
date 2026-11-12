/* Guest malloc shim (LD_PRELOAD into the GUEST only, via `qemu -E LD_PRELOAD=`).
 *
 * Routes the C heap to the host libc through the dlcall plugin: every malloc/free/
 * realloc/calloc becomes an INVOKE_PROC to echo_host_heap on the host. Because the
 * whole process shares one address space, host-heap pointers are usable in the
 * guest, so both worlds end up on ONE allocator (the host heap) — a block from
 * either side is freeable by the other.
 *
 * ZERO DEPENDENCIES on purpose: nothing here calls libc or references an external
 * symbol, so it has no relocations to resolve and is safe to interpose from the
 * very first allocation the guest's ld.so makes — which is exactly what killed the
 * in-runtime arena version (its __libc_* fallback re-entered malloc during PLT
 * resolution). The lazy init below issues only raw syscalls.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

#include "echoheap_proto.h"

/* --- raw dlcall syscalls (no libc) --- */
enum { ECHO_SYS = 4096, DL_LOAD_LIBRARY = 1, DL_GET_PROC = 2, DL_INVOKE = 5 };
#define ECHO_RTLD_NOW 2

static inline uint64_t sys4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("mov %1, %%rax\n mov %2, %%rdi\n mov %3, %%rsi\n mov %4, %%rdx\n"
                     "syscall\n mov %%rax, %0"
                     : "=r"(ret)
                     : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(r10)
                     : "%rax", "%rdi", "%rsi", "%rdx", "%rcx", "%r11", "memory");
    return ret;
}

/* Spinlock guarding the batch cache below. The shim is the guest's malloc, so EVERY
 * guest thread uses it; the process-global prefetch/pending-free arrays would race
 * without this (e.g. two concurrent free()s racing g_pf_free_count++ -> a duplicated
 * slot -> double-free -> host heap corruption -- exactly what QtTest's worker threads
 * trip). Zero-dependency: __atomic builtins lower to inline instructions (no libc,
 * no libatomic for an int on x86_64). A global lock (not per-thread TLS) keeps the
 * dependency-free property; the crossing is held under it, which serialises heap
 * crossings but is correct. heap_call issues only raw syscalls, so no reentrancy. */
static volatile int g_lock;
static inline void heap_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}
static inline void heap_unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

/* Host address of echo_host_heap, resolved once on first use. */
static void *g_heap_fn;

static void ensure_init(void)
{
    if (g_heap_fn)
        return;
    void *h = 0;
    sys4(ECHO_SYS, DL_LOAD_LIBRARY, (uint64_t) "libechoheap_host.so", ECHO_RTLD_NOW,
         (uint64_t) &h);
    void *fn = 0;
    if (h)
        sys4(ECHO_SYS, DL_GET_PROC, (uint64_t) h, (uint64_t) "echo_host_heap",
             (uint64_t) &fn);
    g_heap_fn = fn;
}

static void heap_call(struct EchoHeapReq *r)
{
    ensure_init();
    if (!g_heap_fn) {
        r->ret = 0;
        return;
    }
    sys4(ECHO_SYS, DL_INVOKE, (uint64_t) g_heap_fn, (uint64_t) r, 0);
}

/* --- batched cache ---------------------------------------------------------
 * Every block is a host libc block, so the cache needs no per-block header or
 * size: malloc pulls from a per-size-class prefetch stack (refilled one BATCH per
 * crossing), free collects pointers and returns them one BATCH per crossing. This
 * cuts crossings by ~BATCH. Blocks freed by the host directly (e.g. a guest block
 * passed to a host free) never re-enter these stacks, which is correct — they are
 * just host libc frees. Oversized requests bypass the cache and cross singly.
 *
 * State is process-global here (the tests are single-threaded). Making it truly
 * per-thread needs initial-exec TLS (which stays dependency-free for a preloaded
 * lib, unlike general-dynamic __thread that pulls __tls_get_addr) — a follow-up.
 */
#define ECHO_GRAN 16u
#define ECHO_MAX_CLASS 512u
#define ECHO_NCLASS (ECHO_MAX_CLASS / ECHO_GRAN) /* 32 classes: 16,32,...,512 */
#define ECHO_BATCH 32

static void *g_prefetch[ECHO_NCLASS][ECHO_BATCH];
static int g_pf_count[ECHO_NCLASS];
static void *g_pending_free[ECHO_BATCH];
static int g_pf_free_count;

static int size_class(size_t n)
{
    if (n == 0)
        n = 1;
    if (n > ECHO_MAX_CLASS)
        return -1;
    return (int) ((n - 1) / ECHO_GRAN); /* 1..16->0, 17..32->1, ... */
}

void *malloc(size_t n)
{
    int c = size_class(n);
    if (c < 0) { /* oversized: single crossing */
        struct EchoHeapReq r = {ECHO_HEAP_MALLOC, 0, n, 0, 0, 0};
        heap_call(&r);
        return r.ret;
    }
    heap_lock();
    if (g_pf_count[c] == 0) { /* refill this class in one crossing */
        struct EchoHeapReq r = {ECHO_HEAP_BATCH_MALLOC, 0,
                                (unsigned long) ((c + 1) * ECHO_GRAN), 0,
                                g_prefetch[c], ECHO_BATCH};
        heap_call(&r);
        g_pf_count[c] = ECHO_BATCH;
    }
    void *p = g_prefetch[c][--g_pf_count[c]];
    heap_unlock();
    return p;
}

void free(void *p)
{
    if (!p)
        return;
    heap_lock();
    g_pending_free[g_pf_free_count++] = p;
    if (g_pf_free_count == ECHO_BATCH) { /* bulk-return in one crossing */
        struct EchoHeapReq r = {ECHO_HEAP_BATCH_FREE, 0, 0, 0, g_pending_free,
                                ECHO_BATCH};
        heap_call(&r);
        g_pf_free_count = 0;
    }
    heap_unlock();
}

void *realloc(void *p, size_t n)
{
    struct EchoHeapReq r = {ECHO_HEAP_REALLOC, p, n, 0, 0, 0};
    heap_call(&r);
    return r.ret;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size; /* prototype: no overflow guard */
    void *p = malloc(n);
    if (p) {
        char *c = (char *) p;
        for (size_t i = 0; i < n; ++i)
            c[i] = 0;
    }
    return p;
}

/* The rest of the allocation family MUST route to the unified heap too: glibc's own
 * versions allocate from the GUEST libc heap and hand the pointer back, so a later
 * free() (which we DO interpose -> host free) is a cross-allocator bad-free that
 * corrupts the host heap. (Found via ASan: a low guest-heap pointer freed on the
 * host.) aligned_alloc/posix_memalign/memalign/valloc/pvalloc go through a single
 * ECHO_HEAP_ALIGNED crossing; strdup/strndup/reallocarray build on malloc/realloc. */
static unsigned long echo_slen(const char *s)
{
    const char *p = s;
    while (*p)
        ++p;
    return (unsigned long) (p - s);
}
static void echo_mcpy(void *d, const void *s, unsigned long n)
{
    char *dd = (char *) d;
    const char *ss = (const char *) s;
    while (n--)
        *dd++ = *ss++;
}

void *aligned_alloc(size_t align, size_t n)
{
    struct EchoHeapReq r = {ECHO_HEAP_ALIGNED, 0, n, 0, 0, (unsigned long) align};
    heap_call(&r);
    return r.ret;
}
int posix_memalign(void **memptr, size_t align, size_t n)
{
    struct EchoHeapReq r = {ECHO_HEAP_ALIGNED, 0, n, 0, 0, (unsigned long) align};
    heap_call(&r);
    *memptr = r.ret;
    return r.ret ? 0 : 12 /* ENOMEM */;
}
void *memalign(size_t align, size_t n) { return aligned_alloc(align, n); }
void *valloc(size_t n) { return aligned_alloc(4096, n); }
void *pvalloc(size_t n) { return aligned_alloc(4096, (n + 4095) & ~4095UL); }
void *reallocarray(void *p, size_t nmemb, size_t size) { return realloc(p, nmemb * size); }

char *strdup(const char *s)
{
    unsigned long n = echo_slen(s) + 1;
    char *p = (char *) malloc(n);
    if (p)
        echo_mcpy(p, s, n);
    return p;
}
char *strndup(const char *s, size_t maxn)
{
    unsigned long n = 0;
    while (n < maxn && s[n])
        ++n;
    char *p = (char *) malloc(n + 1);
    if (p) {
        echo_mcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}
char *__strdup(const char *s) { return strdup(s); }
