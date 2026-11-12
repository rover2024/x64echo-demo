//===- HostHooks/Callback.c - libc callback boundary bridge ---------------===//
//
// A guest function pointer handed to an UNINSTRUMENTED host libc entry point.
//
// Echo classifies indirect calls where they are made: the compiler rewrites every
// indirect call in native code into __echo_check_call plus a per-signature
// dispatcher, so a native caller that invokes a guest callback reenters the guest
// instead of jumping into x86 bytes. That covers the callback a native library
// calls itself. It does not cover the callback a native library HANDS to host libc,
// because libc was never compiled by Echo and its indirect call has no
// classification point.
//
// OpenSSL's stack is exactly that: a guest test registers int_compare with
// OPENSSL_sk_new, native OPENSSL_sk_sort passes it straight to qsort, and glibc
// calls the guest address raw. The failure surfaces as a segfault inside qsort.c
// with a nonsense instruction pointer, and qemu reports it as its own internal
// SIGSEGV -- nothing in the message mentions guests, callbacks, or OpenSSL.
//
// The fix is to give libc a callback it CAN call. Each wrapper below parks the real
// comparator and hands libc a trampoline defined here. Because this file is
// compiled by echo-cc as part of the ported library, the call through that parked
// pointer is an ordinary native indirect call, and the compiler gives it the same
// classification and reentry every other one gets. A native comparator takes the
// direct path; a guest one reenters the guest. Neither case is special-cased here.
//
// USE: compile this file with echo-cc into the library being ported and add the
// names in echo_callback.wrap to the native link line as -Wl,--wrap=<name>. That
// redirects the library's own calls only. The real entry points are reached through
// dlsym(RTLD_NEXT) rather than __real_<name>, so the FACE half of this same
// translation unit -- which is linked without --wrap and never called -- still
// links.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// RTLD_NEXT, qsort_r
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

typedef int (*EchoCmp2)(const void *, const void *);
typedef int (*EchoCmp3)(const void *, const void *, void *);

/// Parked comparators, one entry per nesting level. A comparator is allowed to sort
/// again (OpenSSL does not, but nothing forbids it), so this is a stack rather than
/// a single slot, and it is per-thread because two threads can sort at once.
#define ECHO_CB_DEPTH 8
static __thread EchoCmp2 s_cmp2[ECHO_CB_DEPTH];
static __thread EchoCmp3 s_cmp3[ECHO_CB_DEPTH];
static __thread void *s_arg3[ECHO_CB_DEPTH];
static __thread int s_depth2;
static __thread int s_depth3;

/// The indirect call this file exists for. Compiled by echo-cc, so the compiler
/// classifies the target and reenters the guest when it belongs to the guest.
///
/// THE `echo_` PREFIX IS LOAD-BEARING. Echo canonicalises a function's address
/// wherever it escapes as a value, so that both worlds agree on one identity for it:
/// handing `&f` to anyone yields f's FACE stub, a guest address. That is right for
/// library code and exactly wrong here, because the whole point is to give host libc
/// something host libc can call. The canonicaliser skips names beginning `thunk_`,
/// `__echo` or `echo_` (Echo's own glue is never homed), while the indirect-call
/// rewrite skips only `__echo` and `echo_dispatch_` -- so an `echo_`-prefixed name
/// keeps its native address AND still gets the dispatcher on the call below, which is
/// the exact pair of properties a trampoline needs. Rename it to anything else and
/// qsort receives a guest address again, with the identical crash this file fixes.
static int echo_qsort_cmp2(const void *a, const void *b)
{
    return s_cmp2[s_depth2 - 1](a, b);
}

static int echo_qsort_cmp3(const void *a, const void *b, void *arg)
{
    (void) arg; ///< the caller's arg travels in s_arg3, not through libc
    return s_cmp3[s_depth3 - 1](a, b, s_arg3[s_depth3 - 1]);
}

void __wrap_qsort(void *base, size_t nmemb, size_t size, EchoCmp2 compar)
{
    static void (*real)(void *, size_t, size_t, EchoCmp2);
    if (!real)
        real = (void (*)(void *, size_t, size_t, EchoCmp2)) dlsym(RTLD_NEXT, "qsort");
    /// No comparator, or deeper nesting than the stack holds: hand libc what it was
    /// given. A native comparator still works; a guest one would have crashed here
    /// anyway, and crashing is better than silently sorting with the wrong function.
    if (!compar || s_depth2 >= ECHO_CB_DEPTH) {
        real(base, nmemb, size, compar);
        return;
    }
    s_cmp2[s_depth2++] = compar;
    real(base, nmemb, size, echo_qsort_cmp2);
    s_depth2--;
}

void __wrap_qsort_r(void *base, size_t nmemb, size_t size, EchoCmp3 compar, void *arg)
{
    static void (*real)(void *, size_t, size_t, EchoCmp3, void *);
    if (!real)
        real = (void (*)(void *, size_t, size_t, EchoCmp3, void *))
            dlsym(RTLD_NEXT, "qsort_r");
    if (!compar || s_depth3 >= ECHO_CB_DEPTH) {
        real(base, nmemb, size, compar, arg);
        return;
    }
    s_arg3[s_depth3] = arg;
    s_cmp3[s_depth3++] = compar;
    real(base, nmemb, size, echo_qsort_cmp3, NULL);
    s_depth3--;
}

void *__wrap_bsearch(const void *key, const void *base, size_t nmemb, size_t size,
                     EchoCmp2 compar)
{
    static void *(*real)(const void *, const void *, size_t, size_t, EchoCmp2);
    if (!real)
        real = (void *(*) (const void *, const void *, size_t, size_t, EchoCmp2))
            dlsym(RTLD_NEXT, "bsearch");
    if (!compar || s_depth2 >= ECHO_CB_DEPTH)
        return real(key, base, nmemb, size, compar);
    void *r;
    s_cmp2[s_depth2++] = compar;
    r = real(key, base, nmemb, size, echo_qsort_cmp2);
    s_depth2--;
    return r;
}

/// pthread_once has the same shape as qsort: libc runs the routine DURING the call,
/// once. OpenSSL's CRYPTO_THREAD_run_once forwards a caller-supplied init straight to
/// it, and threadstest supplies its own, so the host ran guest code from inside
/// __pthread_once_slow. The routine takes no arguments, so one parked pointer and a
/// depth stack are enough, exactly as for a comparator.
static __thread void (*s_once[ECHO_CB_DEPTH])(void);
static __thread int s_once_depth;

static void echo_once_tramp(void)
{
    s_once[s_once_depth - 1]();
}

int __wrap_pthread_once(pthread_once_t *once, void (*routine)(void))
{
    static int (*real)(pthread_once_t *, void (*)(void));
    if (!real)
        real = (int (*)(pthread_once_t *, void (*)(void)))
            dlsym(RTLD_NEXT, "pthread_once");
    if (!routine || s_once_depth >= ECHO_CB_DEPTH)
        return real(once, routine);
    s_once[s_once_depth++] = routine;
    int rc = real(once, echo_once_tramp);
    s_once_depth--;
    return rc;
}

/// dladdr is not a callback interface, but it fails the same way. An address the
/// guest holds for a library function is the FACE's stub, and the host's dladdr sees
/// only host objects, so it returns 0. OpenSSL's shlibloadtest dlsyms ERR_get_error
/// and asks DSO_dsobyaddr which DSO it came from; the answer was "none".
///
/// __echo_check_call is exactly the translation needed: a face stub becomes the
/// native body, anything else passes through. It is resolved by name because this
/// file is compiled into BOTH halves and the face does not link the host runtime.
int __wrap_dladdr(const void *addr, Dl_info *info)
{
    static int (*real)(const void *, Dl_info *);
    static void *(*xlate)(void *);
    static int inited;
    if (!inited) {
        inited = 1;
        real = (int (*)(const void *, Dl_info *)) dlsym(RTLD_NEXT, "dladdr");
        xlate = (void *(*) (void *)) dlsym(RTLD_DEFAULT, "__echo_check_call");
    }
    if (!real)
        return 0;
    int rc = real(addr, info);
    if (rc || !xlate)
        return rc;
    /// The host does not know this address. If it is a face stub, ask again about the
    /// native body it stands for. check_call reads the stub header only for an address
    /// it has already classified as the guest's, so this is no less safe than the
    /// dispatcher's own use of it.
    void *t = xlate((void *) addr);
    if (t && t != addr)
        return real(t, info);
    return 0;
}

//===----------------------------------------------------------------------===//
// Register-now-call-later: pthread_key_create
//
// The wrappers above park a comparator for the duration of one libc call. A TSD
// destructor is different: libc keeps it and calls it at thread exit, long after
// whoever registered it has returned. That was the reason to leave this alone, and
// OpenSSL shows why it cannot be left alone: threadstest calls
// CRYPTO_THREAD_init_local(&key, its own cleanup), native libcrypto passes that
// GUEST function straight to the host's pthread_key_create, and the host runs it at
// teardown as host code. It crashes with the instruction pointer in guest memory and
// a stack that says only "pthread".
//
// A key is an index, which is what makes it tractable: give libc one trampoline PER
// KEY, each knowing its own slot, and keep the real destructor beside it. The call
// through that saved pointer is an ordinary native indirect call, so a guest
// destructor reenters the guest and a native one is called directly, exactly as for
// the comparators.
//===----------------------------------------------------------------------===//

#define ECHO_TSD_SLOTS 64

static void (*s_tsd_destr[ECHO_TSD_SLOTS])(void *);
static pthread_key_t s_tsd_key[ECHO_TSD_SLOTS];
static int s_tsd_used[ECHO_TSD_SLOTS];
static pthread_mutex_t s_tsd_lock = PTHREAD_MUTEX_INITIALIZER;

/// The classified call, named `echo_` so its address stays native (see echo_qsort_cmp2).
static void echo_tsd_run(int i, void *v)
{
    void (*d)(void *) = s_tsd_destr[i];
    if (d)
        d(v);
}

#define ECHO_TSD_TRAMP(n) \
    static void echo_tsd_##n(void *v) { echo_tsd_run(n, v); }
#define ECHO_TSD_TRAMP8(b)                                                    \
    ECHO_TSD_TRAMP(b##0) ECHO_TSD_TRAMP(b##1) ECHO_TSD_TRAMP(b##2)            \
    ECHO_TSD_TRAMP(b##3) ECHO_TSD_TRAMP(b##4) ECHO_TSD_TRAMP(b##5)            \
    ECHO_TSD_TRAMP(b##6) ECHO_TSD_TRAMP(b##7)
ECHO_TSD_TRAMP8(0) ECHO_TSD_TRAMP8(1) ECHO_TSD_TRAMP8(2) ECHO_TSD_TRAMP8(3)
ECHO_TSD_TRAMP8(4) ECHO_TSD_TRAMP8(5) ECHO_TSD_TRAMP8(6) ECHO_TSD_TRAMP8(7)

#define ECHO_TSD_ROW(b)                                                       \
    echo_tsd_##b##0, echo_tsd_##b##1, echo_tsd_##b##2, echo_tsd_##b##3,       \
    echo_tsd_##b##4, echo_tsd_##b##5, echo_tsd_##b##6, echo_tsd_##b##7
static void (*const echo_tsd_tramps[ECHO_TSD_SLOTS])(void *) = {
    ECHO_TSD_ROW(0), ECHO_TSD_ROW(1), ECHO_TSD_ROW(2), ECHO_TSD_ROW(3),
    ECHO_TSD_ROW(4), ECHO_TSD_ROW(5), ECHO_TSD_ROW(6), ECHO_TSD_ROW(7),
};

int __wrap_pthread_key_create(pthread_key_t *key, void (*destr)(void *))
{
    static int (*real)(pthread_key_t *, void (*)(void *));
    if (!real)
        real = (int (*)(pthread_key_t *, void (*)(void *)))
            dlsym(RTLD_NEXT, "pthread_key_create");
    if (!destr)
        return real(key, NULL);

    int slot = -1;
    pthread_mutex_lock(&s_tsd_lock);
    for (int i = 0; i < ECHO_TSD_SLOTS; i++) {
        if (!s_tsd_used[i]) {
            s_tsd_used[i] = 1;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&s_tsd_lock);
    /// Out of trampolines: register the destructor as given. A native one still works,
    /// and a guest one fails the way it did before this bridge existed, which is better
    /// than silently dropping the destructor a library is relying on.
    if (slot < 0)
        return real(key, destr);

    s_tsd_destr[slot] = destr;
    int rc = real(key, echo_tsd_tramps[slot]);
    if (rc != 0) {
        pthread_mutex_lock(&s_tsd_lock);
        s_tsd_used[slot] = 0;
        pthread_mutex_unlock(&s_tsd_lock);
        s_tsd_destr[slot] = NULL;
        return rc;
    }
    s_tsd_key[slot] = *key;
    return 0;
}

int __wrap_pthread_key_delete(pthread_key_t key)
{
    static int (*real)(pthread_key_t);
    if (!real)
        real = (int (*)(pthread_key_t)) dlsym(RTLD_NEXT, "pthread_key_delete");
    pthread_mutex_lock(&s_tsd_lock);
    for (int i = 0; i < ECHO_TSD_SLOTS; i++) {
        if (s_tsd_used[i] && s_tsd_key[i] == key) {
            s_tsd_used[i] = 0;
            s_tsd_destr[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&s_tsd_lock);
    return real(key);
}
