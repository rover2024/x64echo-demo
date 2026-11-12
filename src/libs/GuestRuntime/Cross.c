//===- Guest/Cross.c - guest-side crossing primitive ----------------------===//
//
// __echo_cross issues the dlcall magic syscall INVOKE_PROC against the host's
// __echo_entry (resolved at bootstrap), which the plugin turns into
// __echo_entry((void*)iid, &blk) on the host, then drives the reentry loop while
// the host is parked waiting on a guest callback.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include "RuntimeAPI.h"
#include "DLCall.h"

#if defined(__x86_64__)
#  include "x86_64/Syscall.c.inc"
#else
#  error "x64echo guest side currently x86_64 only"
#endif

/// Host address of __echo_entry; filled by the bootstrap handshake.
void *echo_host_entry;

void __echo_thread_drain(void)
{
    EchoCallBlock blk;
    memset(&blk, 0, sizeof blk);
    __echo_cross(ECHO_IID_THREAD_DRAIN, &blk);
}

/// Same drain for the MAIN thread, at guest exit, while both worlds are still up.
///
/// Whether the host's own exit-time teardown ever runs is the emulator's choice, and
/// the two disagree: qemu-user turns the guest's exit_group into a host _exit(), so
/// host atexit handlers and library destructors are simply skipped; box64 exits the
/// host process normally, so they all run -- after the guest is gone. Native QtCore's
/// runs there (EarlyMainThread::~EarlyMainThread -> destroy_current_thread_data ->
/// emit QThread::finished) and dereferences a QMetaObject that lives in the guest,
/// which is a SIGSEGV in a place that looks nothing like exit. Draining here clears
/// those keys while a crossing can still service them, so the host's later pass finds
/// nothing to do. Harmless under qemu, where that pass never happens.
__attribute__((destructor)) static void echoGuestExitDrain(void)
{
    if (getenv("ECHO_DEBUG_TSD")) {
        static const char m[] = "[tsd] guest destructor ran\n";
        ssize_t n = write(STDERR_FILENO, m, sizeof m - 1);
        (void) n;
    }
    if (!echo_host_entry)
        return;
    EchoCallBlock blk;
    memset(&blk, 0, sizeof blk);
    __echo_cross(ECHO_IID_GUEST_EXITING, &blk);
}

void __echo_release_host_exception(void *obj)
{
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) echo_host_entry,
                  (uint64_t) ECHO_IID_RELEASE_EXCEPTION, (uint64_t) obj);
}

/// Thread bridge (guest side). A thread-create reentry request (dispatcher==NULL,
/// see HostRT/Entry.c) carries the host worker's start_routine + arg; the guest
/// spawns a real guest thread (-> the emulator's own clone path -> a thread WITH a
/// guest context,
/// which the EmuThreadHook records back to the host) that crosses in via
/// ECHO_IID_THREAD_ENTRY to run start_routine, so a callback on the worker thread
/// reenters the guest normally.
typedef struct {
    void *start_routine;
    void *arg;
} EchoThreadInfo;

static void *echoGuestThreadEntry(void *p)
{
    EchoThreadInfo ti = *(EchoThreadInfo *) p;
    free(p);
    EchoCallBlock blk;
    memset(&blk, 0, sizeof blk);
    blk.native_target = ti.start_routine;
    blk.argblock = ti.arg;
    void *ret = NULL;
    blk.ret = &ret;
    __echo_cross(ECHO_IID_THREAD_ENTRY, &blk);
    return ret;
}

static int echoSpawnThread(void *start_routine, void *arg)
{
    EchoThreadInfo *ti = (EchoThreadInfo *) malloc(sizeof *ti);
    if (!ti)
        return ENOMEM;
    ti->start_routine = start_routine;
    ti->arg = arg;
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, echoGuestThreadEntry, ti);
    if (rc != 0)
        free(ti);
    return rc;
}

/// This thread's HOST errno address, fetched once (docs/07 errno bridge).
///
/// errno is a per-thread libc variable and the two worlds have one each. A native
/// body that fails sets the HOST's; the guest then reads its OWN and sees whatever
/// was left there -- libevent's suite catches this twice, once as
/// `open("/bad/file")` reporting EINVAL instead of ENOENT and once as a failed
/// calloc leaving errno at 0 instead of ENOMEM. Both worlds share one address
/// space, so one crossing per thread buys the address and every later call is a
/// plain load.
static __thread int *g_host_errno;
static __thread int g_host_errno_busy;

static void echoFetchHostErrno(void)
{
    if (g_host_errno || g_host_errno_busy)
        return;
    g_host_errno_busy = 1; ///< the fetch is itself a crossing; don't recurse
    void *p = NULL;
    EchoCallBlock blk;
    __builtin_memset(&blk, 0, sizeof blk);
    blk.ret = &p;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) echo_host_entry, (uint64_t) ECHO_IID_ERRNO_LOC,
                  (uint64_t) &blk);
    g_host_errno = (int *) p;
    g_host_errno_busy = 0;
}

void __echo_cross(long iid, EchoCallBlock *blk)
{
    /// Hand the guest's errno over before the crossing (and pick the host's up
    /// after, below). The slot is per-thread on the host side and doubles as the
    /// carrier in both directions.
    if (iid == ECHO_IID_CALL) {
        if (!g_host_errno)
            echoFetchHostErrno();
        if (g_host_errno)
            *g_host_errno = errno;
    }

    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) echo_host_entry, (uint64_t) iid, (uint64_t) blk);

    /// Reentry loop (04-reentry §2.3): while the host is parked waiting on a guest
    /// call, service its request, then resume it. status stays ECHO_DONE for a
    /// plain outbound call, so this never iterates unless a native body called
    /// back into the guest (or asked to spawn a worker thread).
    while (blk->status == ECHO_NEED_REENTRY) {
        EchoReentry *re = (EchoReentry *) blk->reentry;
        if (re->dispatcher == NULL) {
            int rc = echoSpawnThread(re->guest_fn, re->argblock);
            if (re->ret)
                *(int *) re->ret = rc;
        } else
            ((EchoDispatcher) re->dispatcher)(re->guest_fn, re->argblock, re->ret,
                                              re->varargs);
        echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                      (uint64_t) echo_host_entry, (uint64_t) ECHO_IID_RESUME, (uint64_t) blk);
    }
    if (blk->status == ECHO_NEST_OVERFLOW) {
        static const char msg[] =
            "x64echo: maximum cross-world reentry depth exceeded (64)\n";
        ssize_t written = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void) written;
        abort();
    }

    /// Copy the host's errno through. Unconditional, exactly as it would be if the
    /// two halves shared one libc: a caller checks errno right after the call that
    /// failed, and that call was the crossing.
    if (iid == ECHO_IID_CALL && g_host_errno)
        errno = *g_host_errno;
}
