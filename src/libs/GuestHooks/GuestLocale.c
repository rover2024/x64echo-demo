//===- GuestHooks/GuestLocale.c - guest locale module ---------------------===//
//
// The GUEST half of the locale bridge (host half: HostHooks/Locale.c). Interposes
// the guest's setlocale and repeats every successful change on the host, so native
// code reads the tables the program actually asked for.
//
// Why interposition rather than a registered pointer, as the environment module
// does: a locale is not a vector the host can read at a distance. It lives inside
// guest libc's private state and is only observable through its own accessors, so
// the only place to catch a change is the call itself.
//
// This defines `setlocale`, and the face is linked ahead of libc in the guest's
// global scope, so the program's own calls land here. Forwarding to the real entry
// through RTLD_NEXT keeps the guest's locale exactly as it was; the crossing is
// added, nothing is replaced.
//
// Calls made before the handshake are remembered, not dropped: a program may set
// its locale from a constructor, long before the face has a host handle. The table
// is small and keyed by category, since a later setlocale on the same category
// supersedes the earlier one.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /// RTLD_NEXT
#endif

#include <dlfcn.h>
#include <locale.h>
#include <stddef.h>
#include <string.h>

#include <echo/GuestRuntime/RuntimeAPI.h>

/// Native handle, non-null once the bootstrap has run the hook below.
static void *g_native;

/// One remembered locale per category. LC_ALL is 6 in glibc and the categories are
/// small consecutive integers, so a fixed array covers every one of them with room
/// to spare; an unknown category simply is not remembered (it still crosses live).
#define ECHO_LOCALE_NCAT 16
#define ECHO_LOCALE_MAXLEN 64
static char g_pending[ECHO_LOCALE_NCAT][ECHO_LOCALE_MAXLEN];
static int g_pending_set[ECHO_LOCALE_NCAT];

static void echoRemember(int category, const char *locale)
{
    if (category < 0 || category >= ECHO_LOCALE_NCAT)
        return;
    size_t n = strlen(locale);
    if (n >= ECHO_LOCALE_MAXLEN)
        return;
    memcpy(g_pending[category], locale, n + 1);
    g_pending_set[category] = 1;
}

char *setlocale(int category, const char *locale)
{
    static char *(*real)(int, const char *);
    if (!real)
        real = (char *(*) (int, const char *)) dlsym(RTLD_NEXT, "setlocale");
    if (!real)
        return NULL;
    char *r = real(category, locale);
    /// A null `locale` is a query, not a change; nothing to propagate.
    if (!r || !locale)
        return r;
    echoRemember(category, locale);
    if (g_native)
        __echo_call_host(g_native, "__echo_locale_set", (void *) (long) category,
                         (void *) locale);
    return r;
}

/// Bootstrap hook: keep the handle and replay whatever the program set before the
/// handshake. A native library without the host locale module never exports the
/// target symbol, so each crossing is a silent no-op.
static void echoGuestLocaleRegister(void *handle)
{
    g_native = handle;
    for (int c = 0; c < ECHO_LOCALE_NCAT; c++) {
        if (g_pending_set[c])
            __echo_call_host(handle, "__echo_locale_set", (void *) (long) c,
                             (void *) g_pending[c]);
    }
}

__attribute__((section("echo_boot"), used))
static EchoBootHook echoGuestLocaleHook = &echoGuestLocaleRegister;
