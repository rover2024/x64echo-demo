/* Host heap trampoline. Loaded on the host by the plugin (dlopen); the guest shim
 * resolves echo_host_heap and drives it via INVOKE_PROC. It runs the guest's
 * malloc/free/realloc on the REAL host libc, so both worlds share one allocator
 * (the host heap) and a block allocated in either world is freeable in the other.
 *
 * invoke_proc discards the callee's return value, so the result comes back through
 * req->ret in the shared request struct (which the guest can read directly).
 *
 * DIAGNOSTIC: with ECHO_HEAP_GUARD=1 in the host env, each block is an mmap with a
 * PROT_NONE guard page right after it (electric-fence). A buffer overflow then faults
 * AT the offending write, so a host gdb backtrace names the culprit -- used to hunt a
 * wild-write heap corruption that unified-heap free() only trips on much later. Slow
 * and memory-hungry; for bounded runs only.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>

#include "echoheap_proto.h"

/* ---- guard-page diagnostic allocator (opt-in via ECHO_HEAP_GUARD) ---- */
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ECHO_GT_N 262144
static struct {
    void *user;
    void *base;
    unsigned long mlen;
    unsigned long size;
} g_gt[ECHO_GT_N];
static int g_guard = -1;
static unsigned long g_page;

static int guard_on(void)
{
    if (g_guard < 0) {
        g_guard = getenv("ECHO_HEAP_GUARD") ? 1 : 0;
        g_page = (unsigned long) sysconf(_SC_PAGESIZE);
    }
    return g_guard;
}

static void *guard_malloc(unsigned long n)
{
    if (n == 0)
        n = 1;
    unsigned long body = (n + 15) & ~15UL;           /* keep 16-byte alignment */
    unsigned long bodypages = (body + g_page - 1) & ~(g_page - 1);
    unsigned long mlen = bodypages + g_page;          /* + one guard page */
    char *base = (char *) mmap(0, mlen, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return 0;
    mprotect(base + bodypages, g_page, PROT_NONE);    /* overflow -> SIGSEGV here */
    void *user = base + bodypages - body;             /* block END abuts the guard */
    for (unsigned i = 0; i < ECHO_GT_N; ++i)
        if (!g_gt[i].user) {
            g_gt[i].user = user;
            g_gt[i].base = base;
            g_gt[i].mlen = mlen;
            g_gt[i].size = body;
            break;
        }
    return user;
}

static unsigned long guard_find(void *p)
{
    for (unsigned i = 0; i < ECHO_GT_N; ++i)
        if (g_gt[i].user == p)
            return i + 1; /* 1-based; 0 = not found */
    return 0;
}

static void guard_free(void *p)
{
    if (!p)
        return;
    unsigned long h = guard_find(p);
    if (h) {
        munmap(g_gt[h - 1].base, g_gt[h - 1].mlen);
        g_gt[h - 1].user = 0;
    }
    /* not ours (real-malloc block or table overflow): leak; fine for a diagnostic */
}

static void *guard_realloc(void *p, unsigned long n)
{
    void *q = guard_malloc(n);
    if (p && q) {
        unsigned long h = guard_find(p);
        unsigned long old = h ? g_gt[h - 1].size : n;
        memcpy(q, p, old < n ? old : n);
        guard_free(p);
    }
    return q;
}

void echo_host_heap(void *reqp, void *unused)
{
    (void) unused;
    struct EchoHeapReq *r = (struct EchoHeapReq *) reqp;
    int g = guard_on();
    switch (r->op) {
    case ECHO_HEAP_MALLOC:
        r->ret = g ? guard_malloc(r->n) : malloc(r->n);
        break;
    case ECHO_HEAP_FREE:
        g ? guard_free(r->p) : free(r->p);
        r->ret = 0;
        break;
    case ECHO_HEAP_REALLOC:
        r->ret = g ? guard_realloc(r->p, r->n) : realloc(r->p, r->n);
        break;
    case ECHO_HEAP_BATCH_MALLOC:
        for (unsigned long i = 0; i < r->count; ++i)
            r->vec[i] = g ? guard_malloc(r->n) : malloc(r->n);
        break;
    case ECHO_HEAP_BATCH_FREE:
        for (unsigned long i = 0; i < r->count; ++i)
            g ? guard_free(r->vec[i]) : free(r->vec[i]);
        break;
    case ECHO_HEAP_ALIGNED: {
        unsigned long align = r->count ? r->count : 16;
        unsigned long sz = (r->n + align - 1) & ~(align - 1); /* aligned_alloc: size %align==0 */
        r->ret = g ? guard_malloc(sz) : aligned_alloc(align, sz);
        break;
    }
    default:
        r->ret = 0;
        break;
    }
}
