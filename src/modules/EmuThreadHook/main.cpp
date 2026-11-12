/*
 * EmuThreadHook — LD_PRELOAD'd into the emulator process to invert host-origin
 * thread creation (04-reentry / thread bridge). The problem: a native host
 * library that spawns its own worker thread (pthread_create in "host currency")
 * gets a raw host OS thread with NO guest CPU context, so a guest callback can
 * never run on it. The fix: intercept that pthread_create and route it back
 * through a guest reentry, so the thread is created on the emulator's own clone
 * path and therefore carries a guest context -- the host start_routine then runs
 * on it and can call back into guest code.
 *
 * The one thing this needs from outside is currency: which world owns the calling
 * OS thread at this instant. Two sources, in order:
 *   1. qemu's syscall number, published per-vCPU-thread by the QEMUSyscallObserver
 *      plugin via echo_set_syscall_num(). == 4096 (the dlcall magic syscall) is
 *      host currency; anything else, including a guest clone (56), is guest.
 *   2. otherwise the Echo host runtime's own answer, registered through
 *      echo_register_currency_fn(). Host code runs exactly between entering
 *      __echo_entry and returning from it, so the runtime knows without being told.
 * Source 2 is what makes this work under box64 and FEX, which have no plugin
 * interface: NEITHER EMULATOR IS MODIFIED, this file is the whole bridge.
 *
 * The host runtime registers its reentry ops when it loads; without them (or
 * without this preload) everything passes through.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" {

/* --- published to the observer plugin + the host runtime (this lib is the one
 * LD_PRELOAD'd, so its symbols are global for the RTLD_LOCAL plugin/runtime). --- */
static __thread int64_t tls_syscall_number = -1;
static bool g_observer_present;
void echo_set_syscall_num(int64_t num)
{
    g_observer_present = true;
    tls_syscall_number = num;
}

typedef int (*EchoThreadCreateFn)(pthread_t *, const pthread_attr_t *,
                                  void *(*)(void *), void *);
typedef void (*EchoThreadExitFn)(void *);
typedef int (*EchoCurrencyFn)(void);
static EchoThreadCreateFn g_reenter_create;
static EchoThreadExitFn g_reenter_exit;
static EchoCurrencyFn g_currency;
void echo_register_thread_ops(EchoThreadCreateFn create, EchoThreadExitFn exit_fn)
{
    g_reenter_create = create;
    g_reenter_exit = exit_fn;
}
void echo_register_currency_fn(EchoCurrencyFn fn) { g_currency = fn; }

} // extern "C"

namespace {

enum { ECHO_DLCALL_SYSCALL = 4096 };

struct WorkerJoin;
struct ThreadCtx {
    bool in_hook = false;
    bool pending = false;                 /* a host-origin create is in flight */
    pthread_t *pending_thread = nullptr;
    const pthread_attr_t *pending_attr = nullptr;
    struct WorkerJoin *pending_join = nullptr;
};
__thread ThreadCtx t_ctx;

/* 1 = this OS thread is running host code on the guest's behalf. */
bool host_currency()
{
    if (g_observer_present)
        return tls_syscall_number == ECHO_DLCALL_SYSCALL;
    return g_currency && g_currency();
}

/* Opt-in trace: ECHO_DEBUG_THREADHOOK=1. Every emulator here runs the guest in one
 * process, so the ordinary host environment reaches this. */
bool trace()
{
    static int on = -1;
    if (on < 0) on = getenv("ECHO_DEBUG_THREADHOOK") != nullptr;
    return on != 0;
}

using PFN_create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
using PFN_exit = void (*)(void *);

PFN_create real_create()
{
    static PFN_create fn = (PFN_create) dlsym(RTLD_NEXT, "pthread_create");
    if (!fn) std::abort();
    return fn;
}
PFN_exit real_exit()
{
    static PFN_exit fn = (PFN_exit) dlsym(RTLD_NEXT, "pthread_exit");
    if (!fn) std::abort();
    return fn;
}

/* The worker's join handle.
 *
 * Two things have to be true at once, and getting either one alone is a bug we shipped
 * for an afternoon:
 *
 *   IDENTITY -- the pthread_t the host receives must be the thread its work actually
 *   runs on. Qt stores it in QThreadData::threadId and later compares it against
 *   pthread_self() to decide thread affinity; hand it anything else and
 *   BlockingQueuedConnection takes the wrong branch and qFatal's
 *   (tst_QMetaObject::invokeBlockingQueuedMetaMember caught this).
 *
 *   LIFETIME -- but that thread belongs to the EMULATOR, and at least FEX reclaims it
 *   the moment it ends. The host's std::thread still holds the value, so its join()
 *   gets EINVAL and ~thread() calls std::terminate; worse, the emulator reuses the
 *   pthread_t, so a later join can land on an unrelated live thread. Observed:
 *
 *     [thook] adopt ... tid=0x7ffff6b6dca0     <- 15th create
 *     [thook] adopt ... tid=0x7ffff6b3dca0     <- 16th
 *     [thook] adopt ... tid=0x7ffff6b6dca0     <- 17th, same value as the 15th
 *     [thook] join  tid=0x7ffff6b3dca0 rc=22   <- EINVAL
 *
 * qemu and box64 happen not to recycle that fast, but the hazard is the same there.
 *
 * So: the host gets the real tid (identity), and WE answer its join (lifetime). We
 * learn when the worker ends by wrapping the host's start_routine -- one indirect call
 * per worker, nothing needed from the runtime -- and pthread_join on a tid we handed
 * out waits on that instead of asking the emulator about a thread it may already have
 * thrown away. A reused tid is unambiguous because reuse means the previous worker had
 * already finished. */
struct WorkerJoin {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    bool done = false;
    bool detached = false;
    void *ret = nullptr;
    void *(*fn)(void *) = nullptr;   /* the host's real start_routine */
    void *arg = nullptr;
    pthread_t tid {};
};

/* Small fixed table: one live entry per un-joined worker. Workloads here peak in the
 * dozens; overflow falls back to the emulator's own join, which is what we did before. */
enum { ECHO_MAX_WORKERS = 512 };
pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
WorkerJoin *g_map[ECHO_MAX_WORKERS];

bool map_put(WorkerJoin *w)
{
    bool ok = false;
    pthread_mutex_lock(&g_map_mu);
    for (int i = 0; i < ECHO_MAX_WORKERS; ++i)
        if (!g_map[i]) { g_map[i] = w; ok = true; break; }
    pthread_mutex_unlock(&g_map_mu);
    return ok;
}

/* Newest first: a reused pthread_t names the most recent worker, and any older entry
 * with that value has already finished (that is what made the value free again). */
WorkerJoin *map_take(pthread_t tid)
{
    WorkerJoin *found = nullptr;
    pthread_mutex_lock(&g_map_mu);
    for (int i = ECHO_MAX_WORKERS - 1; i >= 0; --i)
        if (g_map[i] && pthread_equal(g_map[i]->tid, tid)) {
            found = g_map[i];
            g_map[i] = nullptr;
            break;
        }
    pthread_mutex_unlock(&g_map_mu);
    return found;
}

void map_drop(WorkerJoin *w)
{
    pthread_mutex_lock(&g_map_mu);
    for (int i = 0; i < ECHO_MAX_WORKERS; ++i)
        if (g_map[i] == w) { g_map[i] = nullptr; break; }
    pthread_mutex_unlock(&g_map_mu);
}

/* The worker this OS thread is running, so pthread_exit can release a waiting join
 * too: a start_routine that exits instead of returning would otherwise leave the
 * joiner parked for ever, turning a crash into a hang -- strictly worse. */
__thread WorkerJoin *t_worker;

void finish_worker(void *r)
{
    WorkerJoin *w = t_worker;
    if (!w)
        return;
    t_worker = nullptr;
    bool drop = w->detached;
    pthread_mutex_lock(&w->mu);
    w->ret = r;
    w->done = true;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    if (drop) {           /* nobody will join it */
        map_drop(w);
        delete w;
    }
}

/* Runs on the emulator's guest-context thread, in place of the host start_routine. */
void *worker_wrapper(void *p)
{
    WorkerJoin *w = (WorkerJoin *) p;
    t_worker = w;
    void *r = w->fn(w->arg);
    finish_worker(r);
    return r;
}

/* The emulator is creating the thread we asked the guest to spawn. Report its real
 * pthread_t to the host (identity), and remember it so we can answer the join. */
int adopt_guest_thread(PFN_create real, pthread_t *thread, const pthread_attr_t *attr,
                       void *(*start_routine)(void *), void *arg)
{
    int ret = real(thread, attr, start_routine, arg);
    if (ret != 0)
        return ret;
    if (t_ctx.pending_join) {
        t_ctx.pending_join->tid = *thread;
        if (!map_put(t_ctx.pending_join))
            t_ctx.pending_join = nullptr;   /* table full: fall back to the real join */
    }
    if (t_ctx.pending_thread)
        *t_ctx.pending_thread = *thread;
    return ret;
}

} // namespace

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                              void *(*start_routine)(void *), void *arg)
{
    PFN_create real = real_create();

    /* Our own request coming back around: we parked the host caller and the guest,
     * on this same OS thread, is now spawning the thread for it. Everything else
     * created from guest currency is an ordinary guest thread -- pass it through
     * untouched, it already gets a guest context from the emulator. */
    if (!host_currency()) {
        if (t_ctx.pending) {
            int rc = adopt_guest_thread(real, thread, attr, start_routine, arg);
            if (trace()) {
                int st = -1, want = -1;
                if (attr) pthread_attr_getdetachstate(attr, &st);
                if (t_ctx.pending_attr) pthread_attr_getdetachstate(t_ctx.pending_attr, &want);
                fprintf(stderr,
                        "[thook] adopt start=%p arg=%p attr=%p emu_detach=%d host_detach=%d rc=%d tid=%p\n",
                        (void *) start_routine, arg, (void *) attr, st, want, rc,
                        rc == 0 ? (void *) *thread : nullptr);
            }
            return rc;
        }
        if (trace())
            fprintf(stderr, "[thook] guest-thru start=%p\n", (void *) start_routine);
        return real(thread, attr, start_routine, arg);
    }

    if (t_ctx.in_hook || !g_reenter_create)
        return real(thread, attr, start_routine, arg);

    /* Host currency: invert. Ask the guest to create the thread (on the emulator's
     * clone path, so it gets a guest context); the host start_routine runs on it. */
    t_ctx.in_hook = true;
    t_ctx.pending = true;
    t_ctx.pending_thread = thread;
    t_ctx.pending_attr = attr;
    if (trace())
        fprintf(stderr, "[thook] invert start=%p arg=%p\n", (void *) start_routine, arg);

    WorkerJoin *w = new WorkerJoin();
    w->fn = start_routine;
    w->arg = arg;
    {
        int st = PTHREAD_CREATE_JOINABLE;
        if (attr)
            pthread_attr_getdetachstate(attr, &st);
        w->detached = (st == PTHREAD_CREATE_DETACHED);
    }
    t_ctx.pending_join = w;
    /* The guest spawns the worker running worker_wrapper, not the host routine. */
    int ret = g_reenter_create(thread, attr, worker_wrapper, w);
    if (ret != 0) {
        map_drop(w);
        delete w;
    }
    t_ctx.pending_join = nullptr;
    if (trace())
        fprintf(stderr, "[thook] invert done rc=%d tid=%p\n", ret,
                ret == 0 ? (void *) *thread : nullptr);
    t_ctx.pending = false;
    t_ctx.in_hook = false;
    t_ctx.pending_thread = nullptr;
    t_ctx.pending_attr = nullptr;
    return ret;
}

/* Diagnostic only (ECHO_DEBUG_THREADHOOK): pair every join against the tid we handed
 * back, so "a still-joinable std::thread was destroyed" can be told apart from "join
 * was called and failed". Costs one indirect call per join when the trace is off. */
extern "C" int pthread_join(pthread_t thread, void **retval)
{
    static int (*real)(pthread_t, void **);
    if (!real)
        real = (int (*)(pthread_t, void **)) dlsym(RTLD_NEXT, "pthread_join");

    /* A worker we handed out: wait on our own record. Asking the emulator would be
     * asking about a thread it may already have reclaimed. */
    if (WorkerJoin *w = map_take(thread)) {
        pthread_mutex_lock(&w->mu);
        while (!w->done)
            pthread_cond_wait(&w->cv, &w->mu);
        void *r = w->ret;
        pthread_mutex_unlock(&w->mu);
        if (retval)
            *retval = r;
        delete w;
        if (trace())
            fprintf(stderr, "[thook] join tid=%p (ours) rc=0\n", (void *) thread);
        return 0;
    }

    int rc = real(thread, retval);
    if (trace())
        fprintf(stderr, "[thook] join tid=%p rc=%d\n", (void *) thread, rc);
    return rc;
}

/* The exit barrier, part 1: refuse the registrations instead of racing them.
 *
 * Once the guest is gone, the ported libraries' native halves must not run their
 * exit-time code -- they are the other half of a library whose guest half is already
 * destroyed. qemu-user enforces this for free (it turns the guest's exit_group into a
 * host _exit, so NO host exit handler ever runs, and every Iuno measurement to date
 * was taken that way); box64 and FEX exit normally, so it all runs.
 *
 * Arming our own handler is not enough: handlers run last-registered-first, and a
 * function-local static registers its destructor the first time it is USED, at run
 * time -- Qt is full of them, and QQuickPixmapCache::instance()::self is the one that
 * kept winning the race. So drop the registration instead.
 *
 * Everything registered BEFORE the Echo host runtime loaded still runs: that is the
 * emulator's own cleanup (box64's endBox64, which is where it runs the guest's fini),
 * and it should run. Only what the ported libraries register once we are live is
 * dropped, which is exactly the set qemu-user never runs either.
 *
 * ECHO_NO_EXIT_BARRIER=1 turns it off for debugging. */
extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    static int (*real)(void (*)(void *), void *, void *);
    if (!real)
        real = (int (*)(void (*)(void *), void *, void *)) dlsym(RTLD_NEXT, "__cxa_atexit");
    static int on = -1;
    if (on < 0)
        on = getenv("ECHO_NO_EXIT_BARRIER") == nullptr;
    if (on && g_currency)
        return 0; ///< accepted and discarded, like qemu-user's _exit would
    return real(fn, arg, dso);
}

/* The exit barrier, part 2: the host's exit() itself.
 *
 * Redundant with part 1 where it works, and it does not always work -- when the
 * emulator's main() simply returns, glibc calls its own internal alias and an
 * interposed exit() is never consulted (box64 goes that way; FEX calls it through the
 * PLT and is caught here). Kept because it is the cheaper path when it does apply.
 *
 * Once the guest is gone, the ported libraries' native halves must not run their
 * exit-time code -- they are the other half of a library whose guest half is already
 * destroyed. qemu-user enforces this for free (it turns the guest's exit_group into a
 * host _exit, so none of it ever runs, and every Iuno measurement to date was taken
 * that way); box64 and FEX exit normally, so it all runs.
 *
 * Arming an on_exit handler is NOT enough, and this is the part that took a while:
 * exit handlers run last-registered-first, and a function-local static (Qt is full of
 * them -- QQuickPixmapCache::instance()::self) registers its destructor the first time
 * it is used, at run time, long after any handler we could have armed at load or
 * publish. Interposing exit() sidesteps the ordering question entirely.
 *
 * The guest's own stdio was already flushed on its side before it asked to exit;
 * fflush(NULL) covers the host's. ECHO_NO_EXIT_BARRIER=1 turns this off for debugging.
 *
 * Both parts are active only when the Echo host runtime is actually loaded (it
 * registers with us at load), so preloading this library into a pure-emulation lane
 * changes nothing. */
extern "C" void exit(int status)
{
    static void (*real)(int) __attribute__((noreturn));
    if (!real)
        real = (void (*)(int)) dlsym(RTLD_NEXT, "exit");
    static int on = -1;
    if (on < 0)
        on = getenv("ECHO_NO_EXIT_BARRIER") == nullptr;
    if (on && g_currency) {
        fflush(nullptr);
        _exit(status);
    }
    real(status);
    __builtin_unreachable();
}

extern "C" void pthread_exit(void *retval)
{
    PFN_exit real = real_exit();
    finish_worker(retval); ///< no-op unless this thread is carrying a worker
    if (host_currency() && g_reenter_exit && !t_ctx.in_hook) {
        t_ctx.in_hook = true;
        g_reenter_exit(retval);
        t_ctx.in_hook = false;
    }
    real(retval);
    __builtin_unreachable();
}
