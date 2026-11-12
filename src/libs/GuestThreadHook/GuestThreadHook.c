//===- GuestThreadHook.c - drain host TSD before a guest pthread exits -----===//
// SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include <echo/GuestRuntime/RuntimeAPI.h>

typedef int (*EchoPthreadCreate)(pthread_t *, const pthread_attr_t *,
                                 void *(*)(void *), void *);

typedef struct {
    void *(*start)(void *);
    void *arg;
} EchoGuestThreadStart;

static void *echoGuestThreadWrapper(void *opaque)
{
    EchoGuestThreadStart start = *(EchoGuestThreadStart *) opaque;
    free(opaque);
    void *ret = start.start(start.arg);
    __echo_thread_drain();
    return ret;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    static EchoPthreadCreate real_create;
    if (!real_create)
        real_create = (EchoPthreadCreate) dlvsym(RTLD_NEXT, "pthread_create",
                                                 "GLIBC_2.34");
    if (!real_create)
        real_create = (EchoPthreadCreate) dlsym(RTLD_NEXT, "pthread_create");
    if (!real_create)
        return EAGAIN;

    EchoGuestThreadStart *start = malloc(sizeof *start);
    if (!start)
        return ENOMEM;
    start->start = start_routine;
    start->arg = arg;
    int rc = real_create(thread, attr, echoGuestThreadWrapper, start);
    if (rc != 0)
        free(start);
    return rc;
}
