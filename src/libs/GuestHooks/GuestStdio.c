//===- GuestHooks/GuestStdio.c - guest stdio module (docs/07 stream family) --===//
//
// The GUEST-side counterpart of the host stdio bridge (libEchoHostHooks). It is a
// pluggable module in libEchoGuestHooks, NOT core: the core runtime knows nothing
// about stdio. It hooks into bootstrap through the generic `echo_boot` section and
// pairs with its host counterpart through the generic __echo_call_host primitive
// (RuntimeAPI.h).
//
// Its one job (for now): hand the host the guest's three standard-stream pointers,
// so a guest FILE* that crosses into host code (e.g. QTextStream(stdout)) is remapped
// to the host's own stream instead of crashing the host libc on a foreign FILE*.
// A native lib without the host stdio module never exports the target symbol, so the
// crossing is a silent no-op.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stdio.h>

#include <echo/GuestRuntime/RuntimeAPI.h>

/// Bootstrap hook: report {stdin, stdout, stderr} to the host stdio bridge via the
/// generic cross-to-host primitive. Runs from Bootstrap's echo_boot loop with the
/// native handle.
static void echoGuestStdioRegister(void *handle)
{
    FILE *streams[3] = { stdin, stdout, stderr };
    __echo_call_host(handle, "__echo_stdio_register_guest", streams, 0);
}

/// Register the hook in the linker-collected `echo_boot` section (used so the linker
/// keeps it even though nothing references it directly).
__attribute__((section("echo_boot"), used))
static EchoBootHook echoGuestStdioHook = &echoGuestStdioRegister;
