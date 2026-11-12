//===- Variadic/Extract.c - capture: va_list -> CVargEntry[] --------------===//
//
// The capture half of the variadic adaptor (ports lorelei VariadicAdaptor::
// extract to C). It walks a printf/scanf format string with the vendored parser
// (src/3rdparty/printf) purely for type info: mp_vsnprintf is driven with a
// null output sink so nothing is formatted, and each consumed argument is boxed
// into the caller's CVargEntry array. Used by the face variadic stub to snapshot
// a guest va_list before it crosses the boundary.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <printf/printf.h>
#include <printf/printf_scanf.h>

#include <echo/ABI/VarArgs.h>

void echo_varg_overflow(void)
{
    fputs("x64echo: variadic argument limit exceeded (63)\n", stderr);
    abort();
}

/// A variadic function that Echo cannot marshal on its own was crossed without an
/// ECHO_VAOPS adaptor. This used to be a bare abort() emitted inline by the compiler
/// pass, which surfaced as "uncaught target signal 6" and nothing else -- no name, no
/// file, no hint that variadics were involved at all. OpenSSL's ERR_add_error_data
/// cost an afternoon that way. Name the function instead: the fix is always to write
/// the adaptor, and the only thing missing was knowing which one.
void echo_varg_no_adaptor(const char *name)
{
    fputs("x64echo: no ECHO_VAOPS adaptor for variadic '", stderr);
    fputs(name ? name : "?", stderr);
    fputs("'; it cannot cross the boundary. Register a capture/replay pair for it.\n",
          stderr);
    abort();
}

int echo_varg_extract(int style, const char *fmt, va_list ap,
                      struct CVargEntry *out, int capacity)
{
    /// _out_null discards every formatted byte, so the (size_t)-1 "unbounded"
    /// length and the 1-byte dummy buffer are inert; `out` is the real sink the
    /// parser boxes each consumed argument into (trailing CVargType_Void).
    char buffer[1];
    int rc;
    /// A null format reaches here from real code: libxml2's own API test calls every
    /// entry point with all-null arguments, so xmlStrPrintf(NULL, 0, NULL, 0) is a call
    /// the library is expected to survive. Passing null to a printf-family function is
    /// undefined, but no real implementation faults on it, and the capture must not
    /// either -- the parser dereferences the format before it can decide anything, so
    /// the guard belongs at the entry. No format means no variadic argument was consumed.
    if (!fmt) {
        if (capacity > 0)
            out[0].type = CVargType_Void;
        return 0;
    }
    if (style == ECHO_VARG_SCANF)
        rc = mp_vsnprintf_scanf(_out_null, buffer, (size_t) -1, fmt, ap, out,
                                capacity > 0 ? (size_t) capacity : 0);
    else
        rc = mp_vsnprintf(_out_null, buffer, (size_t) -1, fmt, ap, out,
                          capacity > 0 ? (size_t) capacity : 0);
    if (rc < 0)
        return -1; ///< entry-array overflow only (the parser clamps the char count)

    struct CVargEntry *p = out;
    while (p < out + capacity && p->type)
        p++;
    return (int) (p - out);
}
