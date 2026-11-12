//===- echo/ABI/VarArgs.h - Architecture-neutral variadic argument box ----===//
//
// The variadic tail of a call crosses the guest/host boundary as a typed,
// sentinel-terminated CVargEntry[] — architecture-neutral, no x86_64 va_list
// layout. Two roles, one data format:
//   CAPTURE (face / guest side) : va_list / ...  ->  CVargEntry[]
//   REPLAY  (native / host side): CVargEntry[]    ->  platform-native call
//
// The CVargEntry box + its type machinery are shared with the vendored printf
// parser in src/3rdparty/printf (which boxes each consumed argument into one),
// so the type/enum names are kept as the parser's contract. The Echo adaptor
// (echo_varg_extract / echo_varg_call / echo_varg_vcall) ports lorelei's
// VariadicAdaptor logic to plain C: extract walks a printf/scanf format string
// to box a va_list; call/vcall replay the boxes through ffcall/avcall so the
// host issues a correctly-ABI'd native call. arm64/riscv64 native therefore
// never touches the x86_64 va_list layout.
//
// For printf/scanf-style functions echo_varg_extract handles capture directly.
// For other variadic functions the library author writes a capture that reads
// va_arg manually, and registers a capture/replay pair with ECHO_VAOPS.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_ABI_VARARGS_H
#define ECHO_ABI_VARARGS_H

#include <stdarg.h>

#ifdef __cplusplus
#  include <type_traits>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ─── 1. Boxed argument (the shared parser/adaptor contract) ───────────────── */

/// CVargType - Type tag for the value carried by one marshalled variadic argument.
/// C default-promotion has already collapsed the set (char/short -> int,
/// float -> double) by the time a value reaches va_arg, but the finer tags are
/// kept so a manual capture can box a value at its declared width.
enum CVargType {
    CVargType_Void,
    CVargType_Char,
    CVargType_UChar,
    CVargType_Short,
    CVargType_UShort,
    CVargType_Int,
    CVargType_UInt,
    CVargType_Long,
    CVargType_ULong,
    CVargType_LongLong,
    CVargType_ULongLong,
    CVargType_Float,
    CVargType_Double,
    CVargType_Pointer,
};

/// CVargEntry - One variadic argument: a CVargType tag plus the value union. An
/// entry whose type is 0 (CVargType_Void) terminates an array, as
/// CVargEntryLength relies on.
struct CVargEntry {
    int type;
    union {
        char c;
        unsigned char uc;
        short s;
        unsigned short us;
        int i;
        unsigned int u;
        long l;
        unsigned long ul;
        long long ll;
        unsigned long long ull;
        float f;
        double d;
        void *p;
    };
};

/// Count entries up to (not including) the CVargType_Void sentinel.
static inline int CVargEntryLength(struct CVargEntry *argv) {
    int argc = 0;
    for (struct CVargEntry *p = argv; p && p->type; ++p) {
        argc++;
    }
    return argc;
}

/* ─── 2. Echo variadic adaptor (ports lorelei VariadicAdaptor to C) ─────────── */

/// How echo_varg_extract interprets a format string: printf semantics (values)
/// vs scanf semantics (pointers written through).
enum EchoVargFormat {
    ECHO_VARG_PRINTF = 0,
    ECHO_VARG_SCANF = 1,
};

/// CAPTURE — box the arguments described by @fmt out of the va_list @ap into the
/// @out CVargEntry array (interpreted per @style, an EchoVargFormat). Writes a
/// trailing sentinel and returns the number of entries. @capacity includes the
/// sentinel slot; returns -1 if the format consumes too many arguments. No output
/// is produced: the format string is walked only for type info.
int echo_varg_extract(int style, const char *fmt, va_list ap,
                      struct CVargEntry *out, int capacity);

/// Compiler-generated wrappers call this after a capture reports overflow.
/// Emits a fixed diagnostic and terminates before any cross-world call starts.
void echo_varg_overflow(void) __attribute__((noreturn));

/// Called by the compiler-generated stub when a variadic function that needs an
/// ECHO_VAOPS adaptor is crossed without one. Names the function and terminates.
void echo_varg_no_adaptor(const char *name) __attribute__((noreturn));

/// REPLAY (`...` target) — call @func with @argv1 as its fixed arguments and
/// spread @argv2 as the real variadic tail; result -> @ret. Pass argc < 0 to
/// count by sentinel. @ret->type selects the return width.
void echo_varg_call(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                    struct CVargEntry *argv2, struct CVargEntry *ret);

/// REPLAY (`va_list` target) — synthesize a native va_list from @argv2, then call
/// the fixed-prototype @func with @argv1 followed by that va_list. Use this for a
/// vprintf-shaped target; use echo_varg_call for a printf-shaped `...` target.
void echo_varg_vcall(void *func, int argc1, struct CVargEntry *argv1, int argc2,
                     struct CVargEntry *argv2, struct CVargEntry *ret);

/* ─── 3. User-registered adaptor (ECHO_VAOPS) ──────────────────────────────── */

/// CAPTURE op — called by the compiler-generated face variadic stub.
///   @named : pointer to a function-specific struct holding the named args
///            (cast to the right type inside the implementation);
///   @ap    : va_list positioned just after the last named argument;
///   @out   : caller buffer with room for ECHO_VARG_MAX entries.
/// Must write a sentinel at out[n]; returns n (sentinel not counted), or -1 if
/// the ECHO_VARG_MAX buffer is too small.
typedef int (*EchoVargCapture)(const void *named, va_list ap, struct CVargEntry *out);

/// REPLAY op — called by the compiler-generated native entry thunk.
///   @fn    : the real native function body (EchoCallBlock::native_target);
///   @named : same struct as above, unpacked from the argblock;
///   @args  : the CVargEntry[] written by the capture op;
///   @ret   : output box for the return value (type preset; NULL for void).
typedef void (*EchoVargReplay)(void *fn, const void *named,
                               const struct CVargEntry *args, struct CVargEntry *ret);

/// Pair of ops for one variadic function.
typedef struct {
    EchoVargCapture capture;
    EchoVargReplay  replay;
} EchoVargOps;

/// Upper bound on variadic arguments per call (sentinel inclusive); the capture
/// buffer the face stub hands to a capture op is this big.
#define ECHO_VARG_MAX 64

/// Register @cap_fn / @rep_fn as the adaptor for @fn_name. The compiler pass
/// locates the record by the symbol __echo_vaops_<fn_name>.
#define ECHO_VAOPS(fn_name, cap_fn, rep_fn)                                \
    const EchoVargOps __echo_vaops_##fn_name                               \
        __attribute__((used, visibility("hidden")))                        \
        = { (EchoVargCapture)(cap_fn), (EchoVargReplay)(rep_fn) }

#ifdef __cplusplus
}
#endif

/* ─── 4. Typed box constructors (kept from lorelei, C++ and C paths) ────────── */

#ifdef __cplusplus
/// _Enum2Underlying - Maps an enum to its underlying integer type, leaving
/// non-enums unchanged, so an enum argument is classified by its underlying type.
template <typename T, class = std::void_t<>>
struct _Enum2Underlying {
    using type = T;
};

template <typename T>
struct _Enum2Underlying<T, std::enable_if_t<std::is_enum_v<T>>> {
    using type = std::underlying_type_t<T>;
};

template <class T>
static inline constexpr CVargType _CVargTypeID() {
    using T1 = std::remove_cv_t<T>;
    using T2 = typename _Enum2Underlying<T1>::type;
    using V = T2;
    if constexpr (std::is_same_v<V, void>) {
        return CVargType_Void;
    } else if constexpr (std::is_same_v<V, char>) {
        return CVargType_Char;
    } else if constexpr (std::is_same_v<V, unsigned char>) {
        return CVargType_UChar;
    } else if constexpr (std::is_same_v<V, short>) {
        return CVargType_Short;
    } else if constexpr (std::is_same_v<V, unsigned short>) {
        return CVargType_UShort;
    } else if constexpr (std::is_same_v<V, int>) {
        return CVargType_Int;
    } else if constexpr (std::is_same_v<V, unsigned int>) {
        return CVargType_UInt;
    } else if constexpr (std::is_same_v<V, long>) {
        return CVargType_Long;
    } else if constexpr (std::is_same_v<V, unsigned long>) {
        return CVargType_ULong;
    } else if constexpr (std::is_same_v<V, long long>) {
        return CVargType_LongLong;
    } else if constexpr (std::is_same_v<V, unsigned long long>) {
        return CVargType_ULongLong;
    } else if constexpr (std::is_same_v<V, float>) {
        return CVargType_Float;
    } else if constexpr (std::is_same_v<V, double>) {
        return CVargType_Double;
    } else {
        return CVargType_Pointer;
    }
}

#  define CVargTypeID(x) _CVargTypeID<__typeof__(x)>()

template <class T>
static inline constexpr auto *_CVargValueRef(CVargEntry &v) {
    using T1 = std::remove_cv_t<T>;
    using T2 = typename _Enum2Underlying<T1>::type;
    using V = T2;
    if constexpr (std::is_same_v<V, char>) {
        return &v.c;
    } else if constexpr (std::is_same_v<V, unsigned char>) {
        return &v.uc;
    } else if constexpr (std::is_same_v<V, short>) {
        return &v.s;
    } else if constexpr (std::is_same_v<V, unsigned short>) {
        return &v.us;
    } else if constexpr (std::is_same_v<V, int>) {
        return &v.i;
    } else if constexpr (std::is_same_v<V, unsigned int>) {
        return &v.u;
    } else if constexpr (std::is_same_v<V, long>) {
        return &v.l;
    } else if constexpr (std::is_same_v<V, unsigned long>) {
        return &v.ul;
    } else if constexpr (std::is_same_v<V, long long>) {
        return &v.ll;
    } else if constexpr (std::is_same_v<V, unsigned long long>) {
        return &v.ull;
    } else if constexpr (std::is_same_v<V, float>) {
        return &v.f;
    } else if constexpr (std::is_same_v<V, double>) {
        return &v.d;
    } else {
        return &v.p;
    }
}

#  define CVargValueRef(x, V) _CVargValueRef<__typeof__(x)>(V)

#  define CVargValue(x, V) (*CVargValueRef(x, V))

template <class T>
static inline CVargEntry CVargGet(T x) {
    CVargEntry result;
    result.type = _CVargTypeID<T>();
    if constexpr (std::is_pointer_v<T>) {
        result.p = (void *) x;
    } else {
        CVargValue(x, result) = static_cast<__typeof__(CVargValue(x, result))>(x);
    }
    return result;
}
#else
#  define CVargTypeID(x)                                                                           \
      ({                                                                                           \
        __attribute__((unused)) __typeof__(x) y;                                                   \
        __builtin_choose_expr(__builtin_classify_type(y) == 5, CVargType_Pointer,                  \
                              _Generic(y,                                                          \
                                  char: CVargType_Char,                                            \
                                  unsigned char: CVargType_UChar,                                  \
                                  short: CVargType_Short,                                          \
                                  unsigned short: CVargType_UShort,                                \
                                  int: CVargType_Int,                                              \
                                  unsigned int: CVargType_UInt,                                    \
                                  long: CVargType_Long,                                            \
                                  unsigned long: CVargType_ULong,                                  \
                                  long long: CVargType_LongLong,                                   \
                                  unsigned long long: CVargType_ULongLong,                         \
                                  float: CVargType_Float,                                          \
                                  double: CVargType_Double,                                        \
                                  default: CVargType_Pointer));                                    \
      })

#  define CVargValueRef(x, V)                                                                      \
      ({                                                                                           \
        __attribute__((unused)) __typeof__(x) y;                                                   \
        __builtin_choose_expr(__builtin_classify_type(y) == 5, &(V).p,                             \
                              _Generic(y,                                                          \
                                  char: &(V).c,                                                    \
                                  unsigned char: &(V).uc,                                          \
                                  short: &(V).s,                                                   \
                                  unsigned short: &(V).us,                                         \
                                  int: &(V).i,                                                     \
                                  unsigned int: &(V).u,                                            \
                                  long: &(V).l,                                                    \
                                  unsigned long: &(V).ul,                                          \
                                  long long: &(V).ll,                                              \
                                  unsigned long long: &(V).ull,                                    \
                                  float: &(V).f,                                                   \
                                  double: &(V).d,                                                  \
                                  default: &(V).p));                                               \
      })

#  define CVargValue(x, V) *CVargValueRef(x, V)

#  define CVargGet(x)                                                                              \
      ({                                                                                           \
        struct CVargEntry result = {.type = CVargTypeID(x)};                                       \
        __auto_type p = CVargValueRef((x), result);                                                \
        *p = (__typeof__(*p)) x;                                                                   \
        result;                                                                                    \
      })
#endif

#endif /* ECHO_ABI_VARARGS_H */
