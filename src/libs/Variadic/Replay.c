//===- Variadic/Replay.c - replay: CVargEntry[] -> native call -----------===//
//
// The replay half of the variadic adaptor. Given a boxed argument array it
// re-issues the call with the platform's native calling convention via libffi,
// so the host makes a correctly-ABI'd call without knowing the static prototype.
// Used by the native entry thunk to call the real variadic body after the boxes
// cross the boundary.
//
// Why libffi (and not ffcall/avcall): libffi separates "describe the call" from
// "make the call" -- it marshals every argument into an in-memory register-file
// image and a single fixed asm stub loads it, so a by-value AGGREGATE return
// (sret, >16 bytes) is handled by simply putting the caller's buffer in the sret
// register (x8 on aarch64, rdi on x86_64). avcall's macro/per-arity design had a
// broken aarch64 memory-struct-return path; libffi has none of that, so the same
// code returns big aggregates on every arch with no hand asm.
//
//   echo_varg_call       : @func is a `...` function; @argv1 are its named
//                          parameters and @argv2 its variadic tail. Replayed as a
//                          genuine variadic call (ffi_prep_cif_var).
//   echo_varg_vcall      : @func is a va_list-shaped function (takes a trailing
//                          va_list). libffi cannot synthesize a va_list, so a
//                          variadic C trampoline va_start()s a real one from the
//                          boxed tail and forwards it.
//   echo_varg_call_sret  : like echo_varg_call, but @func returns a by-value
//   echo_varg_vcall_sret : aggregate via a hidden pointer ("RVO"-style). The
//                          caller's return buffer is boxed at argv1[0]; libffi
//                          routes it through the sret register.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <stdarg.h>
#include <stddef.h>

#include <ffi.h>

#include <echo/ABI/VarArgs.h>

/// Map a CVargType tag to its libffi type descriptor.
static ffi_type *cvType(int t)
{
    switch (t) {
    case CVargType_Char:      return &ffi_type_schar;
    case CVargType_UChar:     return &ffi_type_uchar;
    case CVargType_Short:     return &ffi_type_sshort;
    case CVargType_UShort:    return &ffi_type_ushort;
    case CVargType_Int:       return &ffi_type_sint;
    case CVargType_UInt:      return &ffi_type_uint;
    case CVargType_Long:      return &ffi_type_slong;
    case CVargType_ULong:     return &ffi_type_ulong;
    case CVargType_LongLong:  return &ffi_type_sint64;
    case CVargType_ULongLong: return &ffi_type_uint64;
    case CVargType_Float:     return &ffi_type_float;
    case CVargType_Double:    return &ffi_type_double;
    case CVargType_Pointer:   return &ffi_type_pointer;
    default:                  return &ffi_type_void;
    }
}

/// Pointer to a box's value. The value union is anonymous, so every member shares
/// the union's address; libffi reads it at the width selected by cvType(type).
#define CVARG_VAL(e) ((void *) &(e)->i)

/// Append @n boxes to the libffi arg vectors starting at slot @off; return the new
/// slot count. Boxes carry their value inline, so avals points straight at them.
static int fillArgs(ffi_type **atypes, void **avals, struct CVargEntry *v, int n,
                    int off)
{
    for (int i = 0; i < n; ++i) {
        atypes[off + i] = cvType(v[i].type);
        avals[off + i] = CVARG_VAL(&v[i]);
    }
    return off + n;
}

/// The scalar return slot (a CVargEntry value union, 8 bytes) is wide enough for
/// any integer/pointer/FP return libffi widens to ffi_arg; NULL for a void return.
static inline void *retSlot(struct CVargEntry *ret)
{
    return (ret && ret->type != CVargType_Void) ? CVARG_VAL(ret) : NULL;
}
static inline ffi_type *retType(struct CVargEntry *ret)
{
    return ret ? cvType(ret->type) : &ffi_type_void;
}

/// Upper bound on total argument slots (named + variadic), sentinel-safe.
#define REPLAY_MAX (ECHO_VARG_MAX * 2 + 8)

//===----------------------------------------------------------------------===//
// `...`-shaped targets: a genuine variadic call. @argv1 are the named parameters
// (the cif's fixed count), @argv2 the variadic tail. ffi_prep_cif_var makes libffi
// place the tail per the ABI's variadic rules (e.g. x86_64 sets AL; riscv64 routes
// FP through GPRs) -- so the arch-specific handling the old avcall path carried by
// hand is now the library's job.
//===----------------------------------------------------------------------===//

void echo_varg_call(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                    struct CVargEntry *argv2, struct CVargEntry *ret)
{
    if (argc1 < 0)
        argc1 = CVargEntryLength(argv1);
    if (argc2 < 0)
        argc2 = CVargEntryLength(argv2);

    ffi_type *atypes[REPLAY_MAX];
    void *avals[REPLAY_MAX];
    int n = fillArgs(atypes, avals, argv1, argc1, 0);
    n = fillArgs(atypes, avals, argv2, argc2, n);

    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, argc1, n, retType(ret), atypes)
        != FFI_OK)
        return;
    ffi_call(&cif, FFI_FN(func), retSlot(ret), avals);
}

/// Build an ffi_type for an opaque @size-byte aggregate: N uint64 members. It is
/// >16 bytes (else the ABI would not use sret), so libffi classifies it memory and
/// routes the return buffer through the sret register. @els must outlive the call.
static ffi_type makeAggType(ffi_type **els, size_t nel)
{
    for (size_t k = 0; k < nel; ++k)
        els[k] = &ffi_type_uint64;
    els[nel] = NULL;
    ffi_type t = { 0, 0, FFI_TYPE_STRUCT, els };
    return t;
}

void echo_varg_call_sret(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                         struct CVargEntry *argv2, unsigned long retsize)
{
    if (argc1 < 0)
        argc1 = CVargEntryLength(argv1);
    if (argc2 < 0)
        argc2 = CVargEntryLength(argv2);

    /// argv1[0] is the caller's return buffer (F's hidden sret pointer, boxed);
    /// the real named parameters are argv1[1..].
    void *buf = argv1[0].p;

    ffi_type *atypes[REPLAY_MAX];
    void *avals[REPLAY_MAX];
    int n = fillArgs(atypes, avals, argv1 + 1, argc1 - 1, 0);
    n = fillArgs(atypes, avals, argv2, argc2, n);

    size_t nel = (retsize + 7) / 8;
    ffi_type *els[nel + 1];
    ffi_type rt = makeAggType(els, nel);

    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, argc1 - 1, n, &rt, atypes) != FFI_OK)
        return;
    ffi_call(&cif, FFI_FN(func), buf, avals);
}

//===----------------------------------------------------------------------===//
// va_list-shaped targets. libffi has no primitive to synthesize a va_list, so a
// variadic C trampoline (vtramp) is minted by an OUTER libffi variadic call that
// spreads the boxed tail into vtramp's `...`; vtramp va_start()s to obtain a real
// native va_list and forwards it to the target through an INNER libffi call. On
// aarch64 a va_list is a 32-byte struct passed by invisible reference, so it is
// handed to libffi as an equally-sized aggregate; on x86_64/riscv64 it is
// pointer-like and passed as a pointer.
//===----------------------------------------------------------------------===//

#if !(defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64))
/// ffi_type mirroring the platform va_list's size (aarch64: a by-value struct).
static ffi_type *valistType(void)
{
    static ffi_type ty;
    static ffi_type *els[(sizeof(va_list) + 7) / 8 + 1];
    if (!ty.elements) {
        size_t nel = (sizeof(va_list) + 7) / 8;
        for (size_t k = 0; k < nel; ++k)
            els[k] = &ffi_type_uint64;
        els[nel] = NULL;
        ty.type = FFI_TYPE_STRUCT;
        ty.elements = els;
    }
    return &ty;
}
#endif

/// Inner call: @func(@fixed[0..nfixed), ap) with return of type @rtype into
/// @rvalue. @func is a fixed-prototype function whose last parameter is a va_list.
static void innerVaCall(void *func, struct CVargEntry *fixed, int nfixed,
                        va_list ap, void *rvalue, ffi_type *rtype)
{
    ffi_type *atypes[ECHO_VARG_MAX + 2];
    void *avals[ECHO_VARG_MAX + 2];
    int n = fillArgs(atypes, avals, fixed, nfixed, 0);

#if defined(__x86_64__) || (defined(__riscv) && __riscv_xlen == 64)
    void *apptr = ap; /* array-decayed, pointer-like va_list */
    atypes[n] = &ffi_type_pointer;
    avals[n] = &apptr;
#else
    atypes[n] = valistType();
    avals[n] = &ap;
#endif
    ++n;

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, n, rtype, atypes) != FFI_OK)
        return;
    ffi_call(&cif, FFI_FN(func), rvalue, avals);
}

/// Variadic trampoline: the outer libffi call spreads the boxed tail into `...`,
/// va_start captures it as a real native va_list, then it is forwarded to @func.
static void vtramp(void *func, struct CVargEntry *fixed, int nfixed, void *rvalue,
                   ffi_type *rtype, ...)
{
    va_list ap;
    va_start(ap, rtype);
    innerVaCall(func, fixed, nfixed, ap, rvalue, rtype);
    va_end(ap);
}

/// Outer driver: libffi variadic-call vtramp, spreading @argv2 as its tail.
static void driveVcall(void *func, struct CVargEntry *fixed, int nfixed, int argc2,
                       struct CVargEntry *argv2, void *rvalue, ffi_type *rtype)
{
    ffi_type *atypes[REPLAY_MAX];
    void *avals[REPLAY_MAX];
    int n = 0;
    atypes[n] = &ffi_type_pointer; avals[n] = &func;    ++n;
    atypes[n] = &ffi_type_pointer; avals[n] = &fixed;   ++n;
    atypes[n] = &ffi_type_sint;    avals[n] = &nfixed;  ++n;
    atypes[n] = &ffi_type_pointer; avals[n] = &rvalue;  ++n;
    atypes[n] = &ffi_type_pointer; avals[n] = &rtype;   ++n;
    int fixedN = n; /* vtramp's named parameters */
    n = fillArgs(atypes, avals, argv2, argc2, n);

    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, fixedN, n, &ffi_type_void, atypes)
        != FFI_OK)
        return;
    ffi_call(&cif, FFI_FN(vtramp), NULL, avals);
}

void echo_varg_vcall(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                     struct CVargEntry *argv2, struct CVargEntry *ret)
{
    if (argc1 < 0)
        argc1 = CVargEntryLength(argv1);
    if (argc2 < 0)
        argc2 = CVargEntryLength(argv2);

    driveVcall(func, argv1, argc1, argc2, argv2, retSlot(ret), retType(ret));
}

void echo_varg_vcall_sret(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                          struct CVargEntry *argv2, unsigned long retsize)
{
    if (argc1 < 0)
        argc1 = CVargEntryLength(argv1);
    if (argc2 < 0)
        argc2 = CVargEntryLength(argv2);

    /// argv1[0] is the caller's return buffer; the named parameters are argv1[1..].
    void *buf = argv1[0].p;

    size_t nel = (retsize + 7) / 8;
    ffi_type *els[nel + 1];
    ffi_type rt = makeAggType(els, nel);

    /// rt (and els) live on this frame through the whole synchronous nested call.
    driveVcall(func, argv1 + 1, argc1 - 1, argc2, argv2, buf, &rt);
}
