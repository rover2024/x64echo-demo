#define MP_VA_PUSH(entry)                                                                     \
  do {                                                                                        \
    if (entries < entries_end)                                                                \
      *entries++ = (entry);                                                                   \
    else                                                                                      \
      entries_overflow = 1;                                                                  \
  } while (0)

#if defined(_WIN32)
#  define va_int(ap)                0
#  define va_unsigned_int(ap)       0
#  define va_long(ap)               0
#  define va_unsigned_long(ap)      0
#  define va_long_long(ap)          0
#  define va_unsigned_long_long(ap) 0
#  define va_float(ap)              0
#  define va_double(ap)             0
#  define va_string(ap)             0
#  define va_pointer(ap)            0
#else
#  ifdef MP_VA_CAPTURE_POINTERS
#    define va_int(ap)                                                                            \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0;                                                                                      \
        })
#    define va_unsigned_int(ap)                                                                   \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0;                                                                                      \
        })
#    define va_long(ap)                                                                           \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0L;                                                                                     \
        })
#    define va_unsigned_long(ap)                                                                  \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0UL;                                                                                    \
        })
#    define va_long_long(ap)                                                                      \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0LL;                                                                                    \
        })
#    define va_unsigned_long_long(ap)                                                             \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0ULL;                                                                                   \
        })
#    define va_float(ap)                                                                          \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0.0f;                                                                                   \
        })
#    define va_double(ap)                                                                         \
        ({                                                                                        \
          void *_val = va_arg(ap, void *);                                                        \
          struct CVargEntry entry = {.type = CVargType_Pointer};                                  \
          entry.p = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          0.0;                                                                                    \
        })
#  else
#    define va_int(ap)                                                                            \
        ({                                                                                        \
          int _val = va_arg(ap, int);                                                             \
          struct CVargEntry entry = {.type = CVargType_Int};                                      \
          entry.i = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_unsigned_int(ap)                                                                   \
        ({                                                                                        \
          unsigned int _val = va_arg(ap, unsigned int);                                           \
          struct CVargEntry entry = {.type = CVargType_UInt};                                     \
          entry.u = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_long(ap)                                                                           \
        ({                                                                                        \
          long _val = va_arg(ap, long);                                                           \
          struct CVargEntry entry = {.type = CVargType_Long};                                     \
          entry.l = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_unsigned_long(ap)                                                                  \
        ({                                                                                        \
          unsigned long _val = va_arg(ap, unsigned long);                                         \
          struct CVargEntry entry = {.type = CVargType_ULong};                                    \
          entry.ul = _val;                                                                        \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_long_long(ap)                                                                      \
        ({                                                                                        \
          long long _val = va_arg(ap, long long);                                                 \
          struct CVargEntry entry = {.type = CVargType_LongLong};                                 \
          entry.ll = _val;                                                                        \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_unsigned_long_long(ap)                                                             \
        ({                                                                                        \
          unsigned long long _val = va_arg(ap, unsigned long long);                               \
          struct CVargEntry entry = {.type = CVargType_ULongLong};                                \
          entry.ull = _val;                                                                       \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_float(ap)                                                                          \
        ({                                                                                        \
          /* A float promotes to double in a variadic call, so %f consumes (and must be          \
           * re-passed as) a full double; tagging it Float would pass a half-width slot and       \
           * misalign every later argument on ABIs with a single varargs save area (riscv64). */  \
          double _val = va_arg(ap, double);                                                       \
          struct CVargEntry entry = {.type = CVargType_Double};                                   \
          entry.d = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#    define va_double(ap)                                                                         \
        ({                                                                                        \
          double _val = va_arg(ap, double);                                                       \
          struct CVargEntry entry = {.type = CVargType_Double};                                   \
          entry.d = _val;                                                                         \
          MP_VA_PUSH(entry);                                                                              \
          _val;                                                                                   \
        })
#  endif
#  define va_string(ap)                                                                           \
      ({                                                                                          \
        char *_val = va_arg(ap, char *);                                                          \
        struct CVargEntry entry = {.type = CVargType_Pointer};                                    \
        entry.p = _val;                                                                           \
        MP_VA_PUSH(entry);                                                                                \
        _val;                                                                                     \
      })
#  define va_pointer(ap)                                                                          \
      ({                                                                                          \
        void *_val = va_arg(ap, void *);                                                          \
        struct CVargEntry entry = {.type = CVargType_Pointer};                                    \
        entry.p = _val;                                                                           \
        MP_VA_PUSH(entry);                                                                                \
        _val;                                                                                     \
      })
#endif
