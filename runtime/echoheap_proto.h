/* Wire format between the guest malloc shim and the host heap trampoline. The
 * shim fills a request on its (guest) stack and hands its address to INVOKE_PROC;
 * the plugin calls echo_host_heap(&req, 0) on the host, which reads/writes it and
 * the batch vectors in the shared address space. Dependency-free layout.
 *
 * Batch ops amortise the boundary crossing: BATCH_MALLOC fills a guest-provided
 * vector with `count` blocks of size `n`; BATCH_FREE frees `count` pointers from
 * one. Because every block (from either world) is a host libc block, free never
 * needs a per-block size or header — it just collects pointers to bulk-free.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ECHOHEAP_PROTO_H
#define ECHOHEAP_PROTO_H

enum {
    ECHO_HEAP_MALLOC = 0,
    ECHO_HEAP_FREE = 1,
    ECHO_HEAP_REALLOC = 2,
    ECHO_HEAP_BATCH_MALLOC = 3, /* n = block size, vec[0..count) <- malloc(n) */
    ECHO_HEAP_BATCH_FREE = 4,   /* free(vec[0..count)) */
    ECHO_HEAP_ALIGNED = 5,      /* ret <- aligned_alloc(count=align, n=size) */
};

struct EchoHeapReq {
    long op;              /* ECHO_HEAP_* */
    void *p;              /* in: pointer for free/realloc */
    unsigned long n;      /* in: size for malloc/realloc/batch-malloc block */
    void *ret;            /* out: result pointer (single ops) */
    void **vec;           /* in/out: batch pointer vector (guest memory) */
    unsigned long count;  /* in: batch element count */
};

#endif
