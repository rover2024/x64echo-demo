//===- GuestRT/DLCall.c - guest-side dlcall client ------------------------===//
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include "DLCall.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
#  include "x86_64/Syscall.c.inc"
#else
#  error "x64echo guest side currently x86_64 only"
#endif

const char *EchoGetHostAttribute(const char *key)
{
    const char *ret = NULL;
    echo_syscall3(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_GET_HOST_ATTRIBUTE,
                  (uint64_t) key, (uint64_t) &ret);
    return ret;
}

void *EchoLoadLibrary(const char *path, int flags)
{
    void *ret = NULL;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_LOAD_LIBRARY,
                  (uint64_t) path, (uint64_t) flags, (uint64_t) &ret);
    return ret;
}

void *EchoGetProcAddress(void *handle, const char *name)
{
    void *ret = NULL;
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_GET_PROC_ADDRESS,
                  (uint64_t) handle, (uint64_t) name, (uint64_t) &ret);
    return ret;
}

const char *EchoGetLibraryError(void)
{
    const char *ret = NULL;
    echo_syscall2(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_GET_LIBRARY_ERROR, (uint64_t) &ret);
    return ret;
}

void EchoInvokeProc(void *proc, void *arg1, void *arg2)
{
    echo_syscall4(ECHO_DLCALL_SYSCALL, ECHO_DLCALL_INVOKE_PROC,
                  (uint64_t) proc, (uint64_t) arg1, (uint64_t) arg2);
}
