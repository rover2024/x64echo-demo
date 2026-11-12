//===- GuestHooks/GuestEnv.c - guest environment module -------------------===//
//
// The GUEST-side counterpart of the host environment bridge. Same shape as the
// stdio module: a pluggable hook in libEchoGuestHooks that reports one guest
// pointer to the host through the generic __echo_call_host primitive.
//
// The guest and the host each have their own `environ`. A guest that calls setenv
// at run time changes only its own, so a native library asking getenv for that
// name sees nothing. OpenSSL's conf_include_test does exactly that: it setenv's
// FNORD, then asks NCONF_get_number with a null config, whose whole job is to fall
// back to the environment. Native code answered "no such variable" and the test
// failed with a message about configuration files.
//
// What is registered is `&environ`, not `environ`: libc reallocates the vector on
// setenv, so the host has to re-read the pointer on every lookup.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <echo/GuestRuntime/RuntimeAPI.h>

extern char **environ;

/// Bootstrap hook: hand the host the ADDRESS of the guest's environ pointer. A native
/// library without the host environment module never exports the target symbol, so
/// the crossing is a silent no-op.
static void echoGuestEnvRegister(void *handle)
{
    __echo_call_host(handle, "__echo_env_register_guest", &environ, 0);
}

__attribute__((section("echo_boot"), used))
static EchoBootHook echoGuestEnvHook = &echoGuestEnvRegister;
