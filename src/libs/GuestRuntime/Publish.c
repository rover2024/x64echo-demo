//===- GuestRT/Publish.c - face-side publish handshake (B-3a) -------------===//
//
// Replaces the per-symbol GET_PROC_ADDRESS loop with a single cross: resolve the
// native __echo_publish, then INVOKE_PROC it once so the host writes every
// binding into the guest's echo_bind table and reports its version hash. Also
// reverse-publishes face vtable address points / guest redispatchers.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include <echo/ABI/Arena.h>

#include "RuntimeAPI.h"
#include "DLCall.h"

#if defined(__x86_64__)
#  include "x86_64/Syscall.c.inc"
#else
#  error "x64echo guest side currently x86_64 only"
#endif

/// Startup profiling (ECHO_TIME=1). The handshake dominates start-up on a big library.
/// It has THREE costs per translation unit, and they must be reported separately -- the
/// first two are cheap and the third is the one that grows: resolving this module's
/// publisher symbol on the host, the crossing that runs it, and the REVERSE publish
/// (__echo_publish_facevts -> the host's __echo_set_facevts, which stores into every
/// loaded object and replays every earlier table). Accumulate all three and report the
/// running totals, so the split is visible without a per-call flood.
static long echoPubNowUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}
static long echo_pub_sym_us, echo_pub_cross_us, echo_facevt_us;
static unsigned echo_pub_calls, echo_facevt_calls, echo_facevt_entries;

void __echo_bind_via_publish(void *handle, void *table, unsigned int count,
                             unsigned long long version, const char *publish_name,
                             const char **names)
{
    const int timing = getenv("ECHO_TIME") != NULL;
    long t0 = timing ? echoPubNowUs() : 0;
    void *pub = EchoGetProcAddress(handle, publish_name);
    if (timing)
        echo_pub_sym_us += echoPubNowUs() - t0;
    if (pub == NULL) {
        /// A translation unit can export functions in one world and nothing in the
        /// other: its body may be entirely inside an architecture guard. Crypto++'s
        /// cpu.cpp and donna_64.cpp are exactly that -- both are full of code on
        /// x86_64 and empty on riscv64 -- so the face carries a publisher for them
        /// and the native half has none.
        ///
        /// That is a real gap but not a fatal one, and it is the same gap the
        /// worklist-mismatch path already tolerates: the functions this unit would
        /// have bound simply do not exist natively, so their stubs stay unbound,
        /// exactly as any other unmatched function's would. Aborting instead turns
        /// "part of the library has no native counterpart" into "the process dies at
        /// startup", which is a far worse answer to the same fact. Reported always,
        /// never quietly, because an unbound stub that IS called still crashes and
        /// this line is the only warning of it.
        fprintf(stderr, "x64echo: native lib has no %s -- that unit exports nothing "
                        "natively; its stubs stay unbound (%s)\n",
                publish_name, EchoGetLibraryError());
        return;
    }

    EchoPublish req;
    req.table = table;
    req.count = count;
    req._pad = 0;
    req.host_version = 0;
    req.names = names;

    /// INVOKE_PROC turns this into __echo_publish(&req, 0) on the host; the second
    /// arg is ignored by the one-parameter publisher. The native side name-matches
    /// its directory against req.names and fills req.table.
    long t1 = timing ? echoPubNowUs() : 0;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) pub, (uint64_t) &req, (uint64_t) 0);
    if (timing) {
        echo_pub_cross_us += echoPubNowUs() - t1;
        if (++echo_pub_calls % 100 == 0)
            fprintf(stderr, "x64echo-time: %u resolvers so far: dlsym %ldms, "
                            "publish crossing %ldms, facevts %ldms "
                            "(%u calls, %u entries)\n",
                    echo_pub_calls, echo_pub_sym_us / 1000, echo_pub_cross_us / 1000,
                    echo_facevt_us / 1000, echo_facevt_calls, echo_facevt_entries);
    }

    if (req.host_version != version) {
        /// NOT fatal: with arch-conditional code (e.g. an x86-only class behind
        /// #ifdef) the face and native worklists legitimately differ, so their
        /// worklist hashes differ too. Name-based binding above already bound the
        /// common functions; each side's extras stay unbound (trap if ever called).
        fprintf(stderr,
                "x64echo: note: %s worklist differs (face=0x%llx native=0x%llx); "
                "common functions bound by name\n",
                publish_name, version, req.host_version);
    }
}

/// Reverse-publish the whole face vtable / redispatcher table to the native side
/// (02-implementation C-1, 04-reentry §2.5) in ONE crossing: hand (tab, n) to the host
/// __echo_set_facevts, which loops dlsym+store. Mirrors __echo_bind_via_publish's
/// single-cross batching.
void __echo_publish_facevts(void *handle, const EchoFaceVtEntry *tab,
                            unsigned int n)
{
    void *set = EchoGetProcAddress(handle, "__echo_set_facevts");
    if (set == NULL)
        return;
    const int timing = getenv("ECHO_TIME") != NULL;
    long t0 = timing ? echoPubNowUs() : 0;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) set, (uint64_t) tab, (uint64_t) n);
    if (timing) {
        echo_facevt_us += echoPubNowUs() - t0;
        echo_facevt_calls++;
        echo_facevt_entries += n;
    }
}

/// Bind this world's operator new/delete to the host-owned shared arena
/// (01-abi-spec §8.3): resolve the native __echo_arena_root, cross once to get the
/// root, and bind. A native lib without an arena (NULL) leaves the operators on
/// their malloc fallback.
void __echo_bind_arena(void *handle)
{
    void *fn = EchoGetProcAddress(handle, "__echo_arena_root");
    if (fn == NULL)
        return;
    EchoArena *root = NULL;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) fn, (uint64_t) &root, (uint64_t) 0);
    echo_arena_bind(root);
}

/// Generic bootstrap-time crossing primitive (the abstraction the runtime exposes;
/// no libc/module knowledge): resolve a host symbol by name and INVOKE_PROC it once
/// as host_fn(a1, a2). A pluggable module (e.g. the guest stdio module, GuestStdio.c)
/// uses this from an `echo_boot` hook to register itself with its host counterpart.
/// A no-op if the native lib does not export the symbol (feature simply absent).
void __echo_call_host(void *handle, const char *sym, void *a1, void *a2)
{
    void *fn = EchoGetProcAddress(handle, sym);
    if (fn == NULL)
        return;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) fn, (uint64_t) a1, (uint64_t) a2);
}
