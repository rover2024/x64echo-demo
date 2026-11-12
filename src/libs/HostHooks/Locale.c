//===- HostHooks/Locale.c - libc locale boundary bridge -------------------===//
//
// A guest and a host have separate libc instances, so they have separate locales.
// The guest sets one; every native function that consults the locale -- ctype
// tables, collation, decimal point -- still sees the host's, which is "C".
//
// PCRE2's test3 is the case that found it. pcre2test (guest) does
// setlocale(LC_CTYPE, "fr_FR") and then asks pcre2_maketables() to build a
// character-class table. That function runs NATIVE and reads the HOST's ctype
// tables, so it produced the C-locale table and every one of test3's French
// character-class matches came out wrong. The failure looks like a pattern-matching
// bug in the library, with nothing pointing at the locale.
//
// Mechanism, mirroring the environment bridge next door: the guest's setlocale is
// interposed (GuestHooks/GuestLocale.c) and reports the change here, and this
// applies the same change to the host's libc. It is the WRITE direction that has to
// cross -- unlike getenv there is nothing to wrap on the native side, because
// native code does not call setlocale at all; it just reads the tables that
// setlocale installed.
//
// The category numbers are passed through as-is. Both worlds are glibc and glibc's
// LC_* have been stable ABI since forever, so LC_CTYPE is 0 on x86_64 and on
// aarch64 alike. A host libc that numbered them differently (musl does not, but it
// is the kind of thing that differs) would need a translation table here.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <locale.h>

/// Host entry the guest INVOKE_PROCs whenever its own setlocale succeeded (and once
/// per remembered category at bootstrap, for the calls that happened before the
/// handshake). `cat` is the libc category number, `loc` a guest-owned string --
/// readable here because both worlds share one address space.
void __echo_locale_set(void *cat, void *loc)
{
    int category = (int) (long) cat;
    const char *locale = (const char *) loc;
    if (!locale)
        return;
    setlocale(category, locale);
}
