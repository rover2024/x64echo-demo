//===- echo/GuestRuntime/DLCall.h - guest-side dlcall client ---------------------===//
//
// Thin wrappers over qemu's dlcall magic syscall, used by the face's bootstrap to
// load and bind the native Echo library.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_GUESTRUNTIME_DLCALL_H
#define ECHO_GUESTRUNTIME_DLCALL_H

#ifdef __cplusplus
extern "C" {
#endif

/// The dlcall magic syscall number (qemu contrib/plugins/dlcall.c default).
enum {
    ECHO_DLCALL_SYSCALL = 4096
};

/// dlcall call IDs (must match the plugin's enum order).
enum {
    ECHO_DLCALL_GET_HOST_ATTRIBUTE = 0,
    ECHO_DLCALL_LOAD_LIBRARY = 1,
    ECHO_DLCALL_GET_PROC_ADDRESS = 2,
    ECHO_DLCALL_FREE_LIBRARY = 3,
    ECHO_DLCALL_GET_LIBRARY_ERROR = 4,
    ECHO_DLCALL_INVOKE_PROC = 5,
};

const char *EchoGetHostAttribute(const char *key);
void *EchoLoadLibrary(const char *path, int flags);
void *EchoGetProcAddress(void *handle, const char *name);
const char *EchoGetLibraryError(void);
/// Call a host function of the shape void(*)(void*, void*) through the dlcall boundary.
/// The bootstrap needs this before the stub table is usable, so it cannot go through the
/// normal face-stub path.
void EchoInvokeProc(void *proc, void *arg1, void *arg2);

#ifdef __cplusplus
}
#endif

#endif // ECHO_GUESTRUNTIME_DLCALL_H
