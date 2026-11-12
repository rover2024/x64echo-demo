//===- HostRT/Entry.c - host call entry + reentry coroutine ---------------===//
//
// Mirror of qemu-passthrough-test/2-callback's Invocation + HostExecContext,
// adapted to Echo's EchoCallBlock currency (04-reentry §2.4):
//
//   __echo_entry(IID_CALL, blk)   -> run blk->entry_thunk on a fresh coroutine
//                                    stack; returns when it completes (status
//                                    DONE) or parks (NEED_REENTRY).
//   __echo_reenter(re)            -> from within a thunk: stash re on the block,
//                                    set NEED_REENTRY, switch back to main.
//   __echo_entry(IID_RESUME, blk) -> switch back into the parked coroutine.
//
// A per-thread context keeps a small invocation stack so nested crossings work,
// LIFO, exactly as Lorelei.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "RuntimeAPI.h"

#include <pthread.h>

// Thread-bridge cleanup: run THIS host thread's pthread-key (TSD) destructors NOW,
// while the thread-entry crossing is still active (a valid invocation on the stack,
// the guest servicing loop alive). Native QtCore registers a key destructor
// (destroy_current_thread_data) that tears down the thread's event dispatcher and
// its child objects -- including guest functor slots whose destruction reenters the
// guest. glibc would otherwise run those destructors at the qemu-cpu-thread's exit,
// AFTER echoThreadEntry has popped its invocation and the guest thread has left its
// __echo_cross loop -- a reentry with no context (invCount 0) that crashes. Draining
// them here moves the reentry inside the crossing; glibc's later pass finds the keys
// cleared and is a no-op. Mirrors glibc's __nptl_deallocate_tsd (its iteration cap).
#ifndef PTHREAD_KEYS_MAX
#  define PTHREAD_KEYS_MAX 1024
#endif
#ifndef PTHREAD_DESTRUCTOR_ITERATIONS
#  define PTHREAD_DESTRUCTOR_ITERATIONS 4
#endif
struct echo_pthread_key_struct {
    uintptr_t seq;              // odd == key in use (glibc KEY_UNUSED tests bit 0)
    void (*destr)(void *);
};
extern struct echo_pthread_key_struct __pthread_keys[] __attribute__((weak));

/// Latched once the guest has told us it is running its own destructors. Process-wide,
/// not per-thread: it is the guest PROCESS that is going away.
static int s_guestExiting;

/// The main image's load range. Identifying the emulator by NAME does not work:
/// dladdr reports the main executable's dli_fname from the link map's l_name, which
/// box64 has rewritten to the guest program's path by the time we look ("./prog/bench"
/// for a destructor that is plainly box64's own emuthread_destroy). The address range
/// is not something the emulator can rename out from under us.
static uintptr_t s_exeLo, s_exeHi;

static int echoMainImagePhdr(struct dl_phdr_info *info, size_t size, void *data)
{
    (void) size;
    (void) data;
    uintptr_t lo = (uintptr_t) -1, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD)
            continue;
        uintptr_t a = (uintptr_t) info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        if (a < lo)
            lo = a;
        if (a + info->dlpi_phdr[i].p_memsz > hi)
            hi = a + info->dlpi_phdr[i].p_memsz;
    }
    s_exeLo = lo;
    s_exeHi = hi;
    return 1; ///< glibc reports the main executable first; stop there
}

/// ...but only for keys the PORTED LIBRARIES registered. Three owners must be left
/// alone, and all three were found the hard way:
///
///   the guest -- a key whose destructor is a GUEST address (the guest program or a
///   face library registered it) must never be called from here: this is host code,
///   so calling it executes x86 bytes on the host CPU. It does not necessarily fault;
///   in the case that found this, it returned, and the only symptom was that the
///   program's entire stdout had vanished while it still exited 0.
///
///   the emulator -- box64 keeps the thread's x64emu_t behind a key whose destructor
///   frees it, and running that here pulls the emulator's context out from under a
///   thread that is still emulating; the heap is corrupt a moment later, in a place
///   with nothing to do with threads (a vector<std::thread> growing on another
///   thread). The emulator is the main executable in qemu-user, box64 and FEX alike.
///
///   Echo's own runtime -- its per-thread state is infrastructure for the crossing
///   we are standing in, not something to tear down in the middle of it.
///
/// glibc runs all three at the real thread exit, which is where they belong.
static int echoDestrIsOurs(void (*destr)(void *))
{
    if (!__echo_is_host((void *) destr))
        return 0;

    if (!s_exeHi)
        dl_iterate_phdr(echoMainImagePhdr, NULL);
    uintptr_t a = (uintptr_t) destr;
    if (a >= s_exeLo && a < s_exeHi)
        return 0; ///< the emulator's own

    Dl_info info;
    if (!dladdr((void *) destr, &info) || !info.dli_fname)
        return 1; ///< host, and not one of the three above
    const char *base = strrchr(info.dli_fname, '/');
    base = base ? base + 1 : info.dli_fname;
    return strncmp(base, "libEcho", 7) != 0 && strncmp(base, "libecho", 7) != 0;
}

static void echo_run_tsd_destructors(void)
{
    if (!&__pthread_keys[0])
        return;
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
        int ran = 0;
        for (unsigned k = 0; k < PTHREAD_KEYS_MAX; ++k) {
            if ((__pthread_keys[k].seq & 1) == 0) // key slot free
                continue;
            void (*destr)(void *) = __pthread_keys[k].destr;
            if (!destr || !echoDestrIsOurs(destr))
                continue;
            void *val = pthread_getspecific((pthread_key_t) k);
            if (!val)
                continue;
            static int tdbg = -1;
            if (tdbg < 0)
                tdbg = getenv("ECHO_DEBUG_TSD") != NULL;
            if (tdbg) {
                Dl_info di;
                if (dladdr((void *) destr, &di))
                    fprintf(stderr, "[tsd] key=%u destr=%p(%s) host=%d in %s\n", k,
                            (void *) destr,
                            di.dli_sname ? di.dli_sname : "?",
                            __echo_is_host((void *) destr),
                            di.dli_fname ? di.dli_fname : "?");
            }
            pthread_setspecific((pthread_key_t) k, NULL);
            destr(val);
            ran = 1;
        }
        if (!ran)
            break;
    }
}

/// Must match the arch's Coroutine.S field order. `sp` is the only field the C
/// glue reads (for nested-coroutine stack placement).
#if defined(__x86_64__)
typedef struct {
    uintptr_t rbx, sp, rbp, r12, r13, r14, r15, rip /* unused */;
} EchoRegState;
#elif defined(__aarch64__)
typedef struct {
    uintptr_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; /* 0..72  */
    uintptr_t fp, lr;                                           /* 80, 88 */
    uintptr_t sp;                                               /* 96     */
    uintptr_t d8, d9, d10, d11, d12, d13, d14, d15;             /* 104..  */
} EchoRegState;
#elif defined(__riscv) && __riscv_xlen == 64
typedef struct {
    uintptr_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11; /* 0..88  */
    uintptr_t ra;                                               /* 96     */
    uintptr_t sp;                                               /* 104    */
    uintptr_t fs0, fs1, fs2, fs3, fs4, fs5,
              fs6, fs7, fs8, fs9, fs10, fs11;                   /* 112..  */
} EchoRegState;
#else
#  error "x64echo host runtime: unsupported host architecture"
#endif

int64_t echo_coro_start(void *arg1, void *arg2,
                        int64_t (*proc)(void *, void *),
                        void *new_stack, EchoRegState *from);
int64_t echo_coro_switch(EchoRegState *from, EchoRegState *to, int64_t ret);

#define ECHO_CORO_STACK (2 * 1024 * 1024) ///< per-thread coroutine stack
#define ECHO_MAX_NEST 64                  ///< reentry nesting depth

typedef struct {
    EchoCallBlock *blk;       ///< the invocation's block
    EchoRegState coroState;   ///< parked coroutine state for this level
} EchoInv;

typedef struct {
    char *stack;              ///< lazily allocated coroutine stack
    char *stackTop;           ///< 16-byte-aligned high end
    EchoInv invs[ECHO_MAX_NEST];
    int invCount;
    EchoRegState mainState;   ///< the dispatcher (__echo_entry) context
} EchoExec;

static __thread EchoExec s_ctx;

/// errno as of the most recent native body's return on THIS thread (docs/07).
/// errno is per-thread and the two worlds have one each; the guest fetches this
/// slot's address once (ECHO_IID_ERRNO_LOC) and reads it after every crossing.
static __thread int s_lastErrno;

/// The coroutine stack is mapped, not malloc'd, and gets a PROT_NONE guard page at
/// its low end. Host code that runs on it is ordinary library code written for a
/// thread stack (8 MB by default), and under box64 the emulator's own dynarec frames
/// land here too whenever that host code reenters the guest. On a malloc'd stack an
/// overflow does not fault -- it writes through into the heap, and the process dies
/// later somewhere unrelated ("malloc(): unsorted double linked list corrupted", on
/// another thread). The guard page turns that into a SIGSEGV at the point of blame.
static void echoExecEnsure(void)
{
    if (s_ctx.stack)
        return;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    size_t total = ECHO_CORO_STACK + (size_t) pg;
    char *m = (char *) mmap(NULL, total, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED)
        return;
    mprotect(m, (size_t) pg, PROT_NONE);
    s_ctx.stack = m + pg;
    s_ctx.stackTop = (char *) (((uintptr_t) (s_ctx.stack + ECHO_CORO_STACK)) & ~(uintptr_t) 0xF);
}

/// Coroutine entry: runs one invocation's thunk, then unwinds the level.
static int64_t echoInvEntry(void *a1, void *a2)
{
    EchoCallBlock *blk = (EchoCallBlock *) a1;
    (void) a2;

    /// Seed the host's errno with the guest's, run the body, then snapshot what it
    /// left. The slot is written by the guest just before the crossing (Cross.c), so
    /// the pair behaves exactly as one libc would: a call that never touches errno
    /// leaves the caller's value alone, and a call that fails hands its value back.
    /// Copying only in the return direction is not enough -- it overwrites the
    /// guest's freshly-zeroed errno with whatever the host last left there, which is
    /// how libevent's util/mm_calloc came back with EINVAL instead of 0.
    errno = s_lastErrno;

    /// The level was pushed by __echo_entry before the switch.
    blk->entry_thunk(blk);

    /// Snapshot errno **as the native body left it**, before any of the runtime's own
    /// bookkeeping can touch it. The guest reads this slot, not the live host errno:
    /// copying the live one hands the guest whatever the crossing machinery itself
    /// last failed at (EINVAL from a probe, say), which is worse than the stale value
    /// it had before. See the guest half in GuestRuntime/Cross.c.
    s_lastErrno = errno;

    s_ctx.invCount--; ///< pop on completion
    return 0;
}

/// Coroutine entry for the thread bridge: run the host worker's start_routine on
/// a fresh guest-context thread. blk->native_target is start_routine,
/// blk->argblock is its arg; the return value goes to *blk->ret.
static int64_t echoThreadEntry(void *a1, void *a2)
{
    EchoCallBlock *blk = (EchoCallBlock *) a1;
    (void) a2;
    void *(*start_routine)(void *) = (void *(*)(void *)) blk->native_target;
    void *r = start_routine(blk->argblock);
    if (blk->ret)
        *(void **) blk->ret = r;
    /// Drain this thread's TSD destructors while the crossing is still live, so the
    /// native thread-data teardown (which reenters the guest to free guest functor
    /// slots) runs with a valid invocation context instead of at qemu-thread exit.
    echo_run_tsd_destructors();
    s_ctx.invCount--;
    return 0;
}

/// Ordinary guest pthread teardown: drain host TSD while the guest wrapper is
/// still inside __echo_cross and can service destructor callbacks.
/// The exit barrier (see s_guestExiting). Registered with on_exit() at the moment the
/// guest starts its own destructors, so it is the LAST handler registered and
/// therefore the FIRST to run -- ahead of every static destructor in the ported
/// libraries' native halves.
///
/// Those destructors are the native half of a library whose guest half has already
/// been destroyed; running them is not "tearing down", it is tearing down a second
/// time, in a world that no longer has the other side. tesseract's
/// ObjectCache<Dawg>::~ObjectCache is the one that made this necessary: it segfaults
/// out of __run_exit_handlers with the whole workload already finished and printed.
///
/// This is not a new policy, it is the EXISTING one made uniform: qemu-user turns the
/// guest's exit_group into a host _exit(), so under qemu none of this ever runs, and
/// every Iuno measurement to date was taken that way. box64 and FEX exit the host
/// process normally. on_exit gives us the real status, and fflush(NULL) keeps the
/// host's own buffered output, which a bare _exit would drop.
/// ECHO_NO_EXIT_BARRIER=1 turns it off for debugging.
static void echoExitAfterGuest(int status, void *arg)
{
    (void) arg;
    fflush(NULL);
    _exit(status);
}

/// Arm it. Called every time a native half comes on line (__echo_set_facevts), which
/// is right AFTER that library was dlopen'd and therefore after its static destructors
/// registered themselves -- and exit handlers run last-registered-first, so this keeps
/// the barrier in front of all of them.
///
/// Arming at the guest's exit instead is not enough, and the reason is worth keeping:
/// box64 runs the guest's own fini from a handler on the SAME host atexit list, and
/// the ported libraries are dlopen'd later than box64 registered that handler, so
/// their destructors run BEFORE the guest's fini -- the guest-side drain never gets a
/// turn. qtquick died exactly there (QQuickPixmapCache::~QQuickPixmapCache), with all
/// four sub-benchmarks already finished and printed, while tesseract happened to get
/// the opposite order and looked fine.
void __echo_arm_exit_barrier(void)
{
    static int armed;
    /// Re-arm per library, so the last one on line is the first handler to run. Capped
    /// because a process with hundreds of ported libraries would otherwise fill the
    /// exit list with copies; the earlier ones are harmless, the latest is what counts.
    if (armed >= 64 || getenv("ECHO_NO_EXIT_BARRIER"))
        return;
    ++armed;
    on_exit(echoExitAfterGuest, NULL);
}

static int64_t echoThreadDrain(void *a1, void *a2)
{
    EchoCallBlock *blk = (EchoCallBlock *) a1;
    /// a2 != 0 means the whole guest process is exiting, not just this thread.
    /// The latch goes up only AFTER the destructors have run: they may themselves
    /// reenter the guest, and that is still serviceable -- the guest is sitting in
    /// __echo_cross waiting to service exactly this.
    echo_run_tsd_destructors();
    if (a2) {
        s_guestExiting = 1;
        __echo_arm_exit_barrier(); ///< belt and braces; normally already armed
        if (getenv("ECHO_DEBUG_TSD"))
            fprintf(stderr, "[tsd] guest exiting: drained, barrier %s\n",
                    getenv("ECHO_NO_EXIT_BARRIER") ? "OFF" : "armed");
    }
    blk->status = ECHO_DONE;
    s_ctx.invCount--;
    return 0;
}

//===----------------------------------------------------------------------===//
// Crossing counter (opt-in). The break-even question -- "how much native work must
// one crossing carry before this beats full emulation" -- needs, for every real
// workload, the number of crossings it actually made, so that its measured time can
// be turned into work per crossing. Nothing outside the process can supply that: how
// often a workload crosses is decided by the library's own call graph.
//
// Off unless ECHO_COUNT_CROSSINGS is set, because this sits on the hottest path in
// the system. When on it is two relaxed atomic increments.
//
// The counters live in a MAPPED FILE, not in a report printed at exit, because there
// is no exit to print from: qemu-user turns the guest's exit_group into a host
// _exit(), so atexit handlers and library destructors in the host runtime never run.
// A shared mapping needs no hook at all -- the kernel already holds the final value
// when the process dies, however it dies. It also costs nothing per crossing, which
// an fd-and-write scheme would not.
//
// ECHO_COUNT_CROSSINGS names the file. Its contents are two little-endian 64-bit
// counters, calls then reentries; read them with
//   python3 -c 'import struct,sys;print(struct.unpack("<QQ",open(sys.argv[1],"rb").read(16)))' FILE
// Setting it to "1" counts into ordinary memory instead, which is only useful when
// something else is going to read the process's memory.
//===----------------------------------------------------------------------===//
static int s_count_on = -1;             ///< -1 = not yet read from the environment
static unsigned long s_local[2];        ///< fallback when no file was named
static unsigned long *s_counts = s_local; ///< [0] = guest->host calls, [1] = reentries

static void echoCountInit(void)
{
    const char *w = getenv("ECHO_COUNT_CROSSINGS");
    s_count_on = w != NULL;
    if (!w || !w[0] || strcmp(w, "1") == 0)
        return;
    int fd = open(w, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (ftruncate(fd, (off_t) sizeof(unsigned long) * 2) == 0) {
        void *m = mmap(NULL, sizeof(unsigned long) * 2, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        if (m != MAP_FAILED)
            s_counts = (unsigned long *) m;
    }
    close(fd);
}

//===----------------------------------------------------------------------===//
// Currency (which world owns this OS thread right now). The thread bridge needs it
// and cannot ask the emulator: only qemu can be asked, through a plugin, and box64
// and FEX have no plugin interface at all. But the runtime already knows -- host
// code runs exactly between entering __echo_entry and returning from it. A park for
// reentry RETURNS from __echo_entry (the coroutine switch goes back to its caller),
// so the flag falls back to guest currency for free, and the save/restore makes it
// nest correctly when the guest calls host again from inside a reentry.
//===----------------------------------------------------------------------===//

static __thread int s_hostCurrency;

/// 1 while this OS thread is running host code on behalf of the guest. Read by the
/// thread-bridge interposer (EmuThreadHook), which is LD_PRELOAD'd into whichever
/// emulator is in use, to tell a host-origin pthread_create from a guest one.
int __echo_in_host_currency(void) { return s_hostCurrency; }

void __echo_entry(void *a1, void *a2)
{
    intptr_t iid = (intptr_t) a1;
    EchoCallBlock *blk = (EchoCallBlock *) a2;
    int savedCurrency = s_hostCurrency;
    s_hostCurrency = 1;

    echoExecEnsure();

    if (s_count_on < 0)
        echoCountInit();
    if (s_count_on && iid == ECHO_IID_CALL)
        __atomic_fetch_add(&s_counts[0], 1, __ATOMIC_RELAXED);

    switch (iid) {
    case ECHO_IID_CALL: {
        /// Default DONE; the thunk may set PENDING_EXCEPTION, __echo_reenter sets
        /// NEED_REENTRY. A nested call runs on the stack just below the parked
        /// caller's SP so coroutine frames don't overlap (Lorelei trick).
        blk->status = ECHO_DONE;
        if (s_ctx.invCount >= ECHO_MAX_NEST) {
            blk->status = ECHO_NEST_OVERFLOW;
            break;
        }
        char *sp = s_ctx.invCount == 0
                       ? s_ctx.stackTop
                       : (char *) (s_ctx.invs[s_ctx.invCount - 1].coroState.sp & ~(uintptr_t) 0xF);

        EchoInv *inv = &s_ctx.invs[s_ctx.invCount++];
        inv->blk = blk;
        echo_coro_start(blk, NULL, echoInvEntry, sp, &s_ctx.mainState);
        break;
    }

    case ECHO_IID_RESUME: {
        /// Resume the most recently parked (top) invocation. Optimistically mark
        /// DONE before re-entering the coroutine: if it parks again
        /// __echo_reenter re-sets NEED_REENTRY, if it throws the thunk sets
        /// PENDING, otherwise it ran to completion. Without this the stale
        /// NEED_REENTRY from the last park would make the guest loop issue one
        /// resume too many.
        if (s_ctx.invCount <= 0)
            break;
        EchoInv *inv = &s_ctx.invs[s_ctx.invCount - 1];
        blk->status = ECHO_DONE;
        echo_coro_switch(&s_ctx.mainState, &inv->coroState, 0);
        break;
    }

    case ECHO_IID_THREAD_ENTRY: {
        /// A newly spawned guest-context thread crossing in to run its host
        /// start_routine on a coroutine (so it can itself park for reentry when
        /// the worker calls back into the guest). Same shape as ECHO_IID_CALL,
        /// but runs echoThreadEntry instead of the block's entry thunk.
        blk->status = ECHO_DONE;
        if (s_ctx.invCount >= ECHO_MAX_NEST) {
            blk->status = ECHO_NEST_OVERFLOW;
            break;
        }
        char *sp = s_ctx.invCount == 0
                       ? s_ctx.stackTop
                       : (char *) (s_ctx.invs[s_ctx.invCount - 1].coroState.sp & ~(uintptr_t) 0xF);
        EchoInv *inv = &s_ctx.invs[s_ctx.invCount++];
        inv->blk = blk;
        echo_coro_start(blk, NULL, echoThreadEntry, sp, &s_ctx.mainState);
        break;
    }

    case ECHO_IID_ERRNO_LOC:
        /// Hand the guest the address of THIS thread's host errno. Both worlds live
        /// in one address space, so once the guest has the pointer it can read the
        /// host's errno with a plain load -- no crossing per call.
        if (blk->ret)
            *(void **) blk->ret = (void *) &s_lastErrno;
        blk->status = ECHO_DONE;
        break;

    case ECHO_IID_RELEASE_EXCEPTION:
        __echo_release_kept_exception(a2);
        break;

    case ECHO_IID_GUEST_EXITING:
    case ECHO_IID_THREAD_DRAIN: {
        blk->status = ECHO_DONE;
        if (s_ctx.invCount >= ECHO_MAX_NEST) {
            blk->status = ECHO_NEST_OVERFLOW;
            break;
        }
        char *sp = s_ctx.invCount == 0
                       ? s_ctx.stackTop
                       : (char *) (s_ctx.invs[s_ctx.invCount - 1].coroState.sp &
                                  ~(uintptr_t) 0xF);
        EchoInv *inv = &s_ctx.invs[s_ctx.invCount++];
        inv->blk = blk;
        echo_coro_start(blk, (void *) (intptr_t) (iid == ECHO_IID_GUEST_EXITING),
                        echoThreadDrain, sp, &s_ctx.mainState);
        break;
    }

    default:
        blk->status = ECHO_DONE;
        break;
    }

    s_hostCurrency = savedCurrency;
}

void __echo_reenter(EchoReentry *re)
{
    /// No crossing to park in. Two very different situations, and conflating them
    /// used to read `s_ctx.invs[-1]` and die in both:
    ///
    ///   the guest is gone -- the host half's own exit-time code is still running
    ///   (box64 lets it; qemu does not) and something in it wants a guest callback.
    ///   There is nobody to answer, and nothing to do at exit anyway: return.
    ///
    ///   the guest is up -- then this is a host thread that never came in through a
    ///   crossing, which is exactly what the thread bridge exists to prevent. Say so
    ///   instead of dying at invs[-1]: that crash names neither the thread nor the
    ///   missing bridge, and it cost two separate debugging sessions to recognise.
    if (s_ctx.invCount <= 0) {
        if (s_guestExiting)
            return;
        static const char msg[] =
            "x64echo: reentry on a thread with no active crossing -- the thread "
            "bridge is not loaded (LD_PRELOAD libecho_thread_hook.so into the "
            "emulator; see docs/04-reentry.md section 6)\n";
        ssize_t written = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void) written;
        abort();
    }

    EchoInv *inv = &s_ctx.invs[s_ctx.invCount - 1]; ///< current (top) invocation

    /// Opt-in trace of every host->guest reentry. A null guest_fn here is the
    /// signature of a face stub that never got bound: the dispatcher classified the
    /// null target as "guest" and reentered with it, and the crash then lands inside
    /// the face's redispatcher with nothing to say what was being called. Pairs with
    /// ECHO_DEBUG_STUB, which names the stub. Cached, because this is the hot path.
    static int rdbg = -1;
    if (rdbg < 0)
        rdbg = getenv("ECHO_DEBUG_REENTER") != NULL;
    if (rdbg)
        fprintf(stderr, "[reenter] guest_fn=%p dispatcher=%p argblock=%p ret=%p inv=%d\n",
                re->guest_fn, re->dispatcher, re->argblock, re->ret, s_ctx.invCount);

    if (s_count_on > 0)
        __atomic_fetch_add(&s_counts[1], 1, __ATOMIC_RELAXED);

    inv->blk->reentry = re;
    inv->blk->status = ECHO_NEED_REENTRY;

    /// Park: save this coroutine's state, switch back to __echo_entry's caller.
    /// Resumes here when the guest issues ECHO_IID_RESUME.
    echo_coro_switch(&inv->coroState, &s_ctx.mainState, 1);
}

//===----------------------------------------------------------------------===//
// Thread bridge (host side). The EmuThreadHook (LD_PRELOAD'd into QEMU) inverts
// a host library's pthread_create in host currency by calling these, which we
// register with it at load. A thread-create parks the current coroutine with a
// reentry request whose dispatcher is NULL (the marker); the guest recognises it,
// spawns the thread on QEMU's clone path, and resumes.
//===----------------------------------------------------------------------===//

/// Registered by the EmuThreadHook (weak: no-op when the hook is not preloaded).
extern void echo_register_thread_ops(int (*create)(void *, void *, void *, void *),
                                     void (*exit_fn)(void *)) __attribute__((weak));

/// Also weak, and separate from the pair above, so a hook built before currency was
/// published still links and still works off qemu's syscall observer.
extern void echo_register_currency_fn(int (*fn)(void)) __attribute__((weak));

int __echo_reenter_thread_create(void *thread, void *attr, void *start_routine,
                                 void *arg)
{
    (void) thread; /* the pthread_t is filled by the hook during the guest clone */
    (void) attr;
    int rc = EAGAIN; /* retained if the guest cannot service the request */
    EchoReentry re;
    re.guest_fn = start_routine; /* the host worker's start_routine */
    re.dispatcher = NULL;        /* marker: this is a thread-create request */
    re.argblock = arg;           /* the host worker's arg */
    re.ret = &rc;
    re.varargs = NULL;
    __echo_reenter(&re);
    return rc;
}

void __echo_reenter_thread_exit(void *retval)
{
    (void) retval; /* the guest thread unwinds naturally; nothing to do here yet */
}

__attribute__((constructor)) static void echoThreadBridgeInit(void)
{
    if (echo_register_thread_ops)
        echo_register_thread_ops(__echo_reenter_thread_create,
                                 __echo_reenter_thread_exit);
    if (echo_register_currency_fn)
        echo_register_currency_fn(__echo_in_host_currency);
}
