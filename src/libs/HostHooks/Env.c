//===- HostHooks/Env.c - libc environment boundary bridge -----------------===//
//
// A guest and a host have separate `environ`. Everything the launcher exports
// reaches both, so the split only shows up for a variable the guest sets at RUN
// TIME: the native library asks getenv and sees nothing.
//
// OpenSSL's conf_include_test is the case that found it. It setenv's FNORD and then
// calls NCONF_get_number with a null config, whose documented behaviour is to fall
// back to the environment. Native code answered "no such variable" and the failure
// was reported as a configuration-file error, which is nowhere near the truth.
//
// Mechanism (same as the stdio bridge): the native library is linked with
// --wrap=getenv (list in echo_callback.wrap's sibling, echo_env.wrap), so ITS calls
// become __wrap_getenv, defined here. The guest reports &environ once at bootstrap
// (GuestHooks/GuestEnv.c). A lookup checks the guest's vector first and falls back to
// the host's real getenv, so a variable only the host has still resolves.
//
// The guest's vector wins on a conflict. It is the one the program itself has been
// editing, and the launcher gives both worlds the same starting set, so a difference
// means the guest changed it on purpose.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// RTLD_NEXT, secure_getenv
#endif

#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/// Address of the guest's `environ`, reported once at bootstrap. Reread through on
/// every lookup because libc reallocates the vector on setenv. Null until registered,
/// which makes every lookup a plain pass-through.
static char ***g_guest_environ;

/// Host entry the guest INVOKE_PROCs at bootstrap (mirrors __echo_stdio_register_guest).
/// The second argument is the ignored INVOKE_PROC filler.
void __echo_env_register_guest(void *envp, void *unused)
{
    (void) unused;
    g_guest_environ = (char ***) envp;
}

static char *echoGuestGetenv(const char *name)
{
    if (!g_guest_environ || !name)
        return NULL;
    char **env = *g_guest_environ;
    if (!env)
        return NULL;
    size_t n = strlen(name);
    for (; *env; env++) {
        if (strncmp(*env, name, n) == 0 && (*env)[n] == '=')
            return *env + n + 1;
    }
    return NULL;
}

char *__wrap_getenv(const char *name)
{
    static char *(*real)(const char *);
    char *v = echoGuestGetenv(name);
    if (v)
        return v;
    if (!real)
        real = (char *(*) (const char *)) dlsym(RTLD_NEXT, "getenv");
    return real(name);
}

/// secure_getenv is getenv plus a refusal to answer in a setuid process. The guest's
/// vector is this process's own, so the same rule applies to it: defer to the real
/// entry point to decide, and only consult the guest when the host would have answered.
char *__wrap_secure_getenv(const char *name)
{
    static char *(*real)(const char *);
    if (!real)
        real = (char *(*) (const char *)) dlsym(RTLD_NEXT, "secure_getenv");
    if (!real)
        return __wrap_getenv(name);
    char *hv = real(name);
    if (hv)
        return hv;
    /// The host said no. That is either "not set" or "refused because setuid"; the
    /// second case is why this does not simply check the guest first. Distinguish them
    /// by asking for a name that is certainly set: if the host answers nothing at all
    /// for PATH, it is refusing, and the guest must not be consulted either.
    if (!real("PATH"))
        return NULL;
    return echoGuestGetenv(name);
}
