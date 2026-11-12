//===- HostHooks/Stdio.c - libc stdio boundary bridge (docs/07 stream family) -===//
//
// A HOST-side, libc-specific module that keeps the core EchoHostRuntime free of any
// stdio knowledge. It exists because a guest FILE* handed to a host library (e.g.
// QTextStream(stdout)) is opaque to the host's own libc -- a different libc, a
// foreign structure -- and crashes it ("invalid stdio handle").
//
// Mechanism (docs/07 §5, stdio bridge): the native library is linked with
// `--wrap=<fn>` for the stdio family, so ITS calls to those functions become
// __wrap_<fn>, defined here. The wrapper MAPS a guest standard stream
// (stdin/stdout/stderr, whose pointers the guest reports at bootstrap via
// __echo_stdio_register_guest) to the HOST's corresponding standard stream, then
// forwards to the real libc entry (dlsym(RTLD_NEXT)). Because guest and host share
// one process / one fd table, the host's stdout writes to the same terminal, and
// (with the guest-side shim routing the guest's own stdio to the host too) there is
// a single set of host FILE*s -- no double buffering, no reentry.
//
// A non-standard FILE* (a host fopen'd file, or -- once the guest shim lands -- a
// host FILE* the guest holds) is passed through unchanged. The wrapped-name list
// lives on the native link line (echo_stdio.wrap), owned by this module; the
// compiler is uninvolved.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>

// The guest's three standard-stream pointers, reported once at bootstrap. Compared
// by identity against an incoming FILE* to decide whether to remap it to the host's
// own stream. Zero until registered (then no mapping happens -- safe pass-through).
static FILE *g_guest_in;
static FILE *g_guest_out;
static FILE *g_guest_err;

// Host entry the guest INVOKE_PROCs at bootstrap (mirrors __echo_arena_root): arg is
// a guest pointer to FILE*[3] = {stdin, stdout, stderr}. Read through the shared
// address space and stash. The second arg is the ignored INVOKE_PROC filler.
void __echo_stdio_register_guest(void *streams, void *unused)
{
    (void)unused;
    FILE **s = (FILE **)streams;
    if (!s)
        return;
    g_guest_in = s[0];
    g_guest_out = s[1];
    g_guest_err = s[2];

    // 一旦 guest 的标准流被映射到宿主的流,guest 的 stdout 就变成**宿主侧**的缓冲区,
    // 而 qemu-user 把 guest 的 exit_group 变成宿主 _exit() —— 宿主的 stdio 缓冲区
    // 一次都不会刷。结果是:stderr 照常出来(它本来就无缓冲),stdout **整个丢掉**。
    // 表现极其难认:一致性套件的进度行和最后那句 "Total N tests, M errors" 全没了,
    // 看着像程序中途挂了;给 pty 也没用,因为缓冲在宿主侧不在 guest 侧。
    // 2026-09-09 用 qemu 的 -strace 才看清:guest 明明 write(1, ...) 过。
    // 修法是把宿主的 stdout 设成无缓冲。用 dlsym 拿真的 setvbuf:setvbuf 自己在
    // --wrap 名单里,直接调会进 __wrap_setvbuf。
}

// 把**宿主**的 stdout 设成无缓冲,并且是在库一装载就做,不等 guest 来注册。
//
// 为什么必须做:guest 的标准流一旦映射到宿主的流,guest 的 stdout 就变成宿主侧的
// 缓冲区,而 qemu-user 把 guest 的 exit_group 变成宿主 _exit() —— 宿主 stdio 的
// 缓冲区一次都不会刷。于是 stderr 照常出来(本来就无缓冲),stdout **整个丢掉**。
// 表现极难认:一致性套件的进度行和最后那句 "Total N tests, M errors" 全没了,
// 看着像程序中途崩了;给 pty 也没用,因为缓冲在宿主侧不在 guest 侧。
// 2026-09-09 是用 qemu 的 -strace 看到 guest 明明 write(1, ...) 过才定位的。
//
// 用 dlsym 拿真的 setvbuf:setvbuf 自己在 --wrap 名单里,直接调会进 __wrap_setvbuf。
__attribute__((constructor)) static void echo_unbuffer_host_stdout(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    int (*real_setvbuf)(FILE *, char *, int, size_t) =
        (int (*)(FILE *, char *, int, size_t))dlsym(RTLD_NEXT, "setvbuf");
    if (real_setvbuf)
        real_setvbuf(stdout, NULL, _IONBF, 0);
}

// Map a guest standard stream to the host's; leave anything else (host FILE*) alone.
static inline FILE *echo_map(FILE *f)
{
    if (f == g_guest_out && g_guest_out)
        return stdout;
    if (f == g_guest_err && g_guest_err)
        return stderr;
    if (f == g_guest_in && g_guest_in)
        return stdin;
    return f;
}

// Resolve the real libc entry once, cached. RTLD_NEXT = the next definition after
// this module in the load order = libc's own.
#define REAL(name, ret, ...)                                                       \
    static ret (*real)(__VA_ARGS__);                                               \
    if (!real)                                                                     \
        *(void **)&real = dlsym(RTLD_NEXT, name);

size_t __wrap_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    REAL("fwrite", size_t, const void *, size_t, size_t, FILE *)
    return real(p, sz, n, echo_map(f));
}
size_t __wrap_fread(void *p, size_t sz, size_t n, FILE *f)
{
    REAL("fread", size_t, void *, size_t, size_t, FILE *)
    return real(p, sz, n, echo_map(f));
}
int __wrap_fputs(const char *s, FILE *f)
{
    REAL("fputs", int, const char *, FILE *)
    return real(s, echo_map(f));
}
char *__wrap_fgets(char *s, int n, FILE *f)
{
    REAL("fgets", char *, char *, int, FILE *)
    return real(s, n, echo_map(f));
}
int __wrap_fputc(int c, FILE *f)
{
    REAL("fputc", int, int, FILE *)
    return real(c, echo_map(f));
}
int __wrap_fgetc(FILE *f)
{
    REAL("fgetc", int, FILE *)
    return real(echo_map(f));
}
int __wrap_putc(int c, FILE *f)
{
    REAL("putc", int, int, FILE *)
    return real(c, echo_map(f));
}
int __wrap_getc(FILE *f)
{
    REAL("getc", int, FILE *)
    return real(echo_map(f));
}
int __wrap_fflush(FILE *f)
{
    REAL("fflush", int, FILE *)
    return real(echo_map(f)); // fflush(NULL) flushes all -- map() leaves NULL as-is
}
int __wrap_fclose(FILE *f)
{
    REAL("fclose", int, FILE *)
    // Never close a mapped host standard stream on the guest's behalf; only a real
    // (host fopen'd) FILE* reaches the close. A guest std stream maps to the host's
    // own stdout/stderr/stdin, which must stay open -- swallow the close.
    FILE *m = echo_map(f);
    if (m != f)
        return 0;
    return real(f);
}
int __wrap_fseek(FILE *f, long off, int whence)
{
    REAL("fseek", int, FILE *, long, int)
    return real(echo_map(f), off, whence);
}
long __wrap_ftell(FILE *f)
{
    REAL("ftell", long, FILE *)
    return real(echo_map(f));
}
// Large-file-support variants (_FILE_OFFSET_BITS=64, as Qt is built): fseeko/ftell
// resolve to the *64 names, which is what the native library actually references.
int __wrap_fseeko(FILE *f, long long off, int whence)
{
    REAL("fseeko", int, FILE *, long long, int)
    return real(echo_map(f), off, whence);
}
long long __wrap_ftello(FILE *f)
{
    REAL("ftello", long long, FILE *)
    return real(echo_map(f));
}
int __wrap_fseeko64(FILE *f, long long off, int whence)
{
    REAL("fseeko64", int, FILE *, long long, int)
    return real(echo_map(f), off, whence);
}
long long __wrap_ftello64(FILE *f)
{
    REAL("ftello64", long long, FILE *)
    return real(echo_map(f));
}
int __wrap_fgetpos(FILE *f, void *pos)
{
    REAL("fgetpos", int, FILE *, void *)
    return real(echo_map(f), pos);
}
int __wrap_fsetpos(FILE *f, const void *pos)
{
    REAL("fsetpos", int, FILE *, const void *)
    return real(echo_map(f), pos);
}
int __wrap_fgetpos64(FILE *f, void *pos)
{
    REAL("fgetpos64", int, FILE *, void *)
    return real(echo_map(f), pos);
}
int __wrap_fsetpos64(FILE *f, const void *pos)
{
    REAL("fsetpos64", int, FILE *, const void *)
    return real(echo_map(f), pos);
}
int __wrap_feof(FILE *f)
{
    REAL("feof", int, FILE *)
    return real(echo_map(f));
}
int __wrap_ferror(FILE *f)
{
    REAL("ferror", int, FILE *)
    return real(echo_map(f));
}
void __wrap_clearerr(FILE *f)
{
    REAL("clearerr", void, FILE *)
    real(echo_map(f));
}
int __wrap_fileno(FILE *f)
{
    REAL("fileno", int, FILE *)
    return real(echo_map(f));
}
int __wrap_setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    REAL("setvbuf", int, FILE *, char *, int, size_t)
    return real(echo_map(f), buf, mode, size);
}

// Variadic: forward the va_list to the real v-form on the MAPPED (host) stream --
// the host FILE* makes the real libc formatter work directly, no manual capture.
int __wrap_fprintf(FILE *f, const char *fmt, ...)
{
    REAL("vfprintf", int, FILE *, const char *, va_list)
    va_list ap;
    va_start(ap, fmt);
    int r = real(echo_map(f), fmt, ap);
    va_end(ap);
    return r;
}
int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    REAL("vfprintf", int, FILE *, const char *, va_list)
    return real(echo_map(f), fmt, ap);
}
int __wrap_fscanf(FILE *f, const char *fmt, ...)
{
    REAL("vfscanf", int, FILE *, const char *, va_list)
    va_list ap;
    va_start(ap, fmt);
    int r = real(echo_map(f), fmt, ap);
    va_end(ap);
    return r;
}
int __wrap_vfscanf(FILE *f, const char *fmt, va_list ap)
{
    REAL("vfscanf", int, FILE *, const char *, va_list)
    return real(echo_map(f), fmt, ap);
}
