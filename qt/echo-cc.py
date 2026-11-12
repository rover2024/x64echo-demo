#!/usr/bin/env python3
# echo-cc — single-invocation dual compiler (docs/06 §7). One call produces BOTH
# halves; the build only ever handles/executes the NATIVE output.
#
# Naming (per spec): the native/host library keeps its REAL name (e.g.
# libQt6Core.so.6.8.4). The face is written into an "echo-face/" shadow dir beside
# the native, under the SAME real name + soname symlinks, so downstream faces link
# it with a normal `-L<dir>/echo-face -lX`. The face is also embedded into the
# native as a non-load ".echo_face" section for shipping; the shadow copy is a
# build-tree-only intermediate (not installed), so the installed host tree is a
# normal library tree.
#
#   compile  -c ... -o foo.o           -> foo.o          (native)
#                                         foo.o.echoface (face object)
#   link -shared ... -o libX.so.A.B.C   -> libX.so.A.B.C  (native, host RT, with an
#                                          embedded .echo_face section)
#                                         echo-face/libX.so.A.B.C (+ soname/link
#                                          symlinks) (face, guest RT + stubs.ld;
#                                          deps resolved to their echo-face copies)
#   link exe                            -> native only (runnable; face never needed)
#
# Set as CMAKE_C/CXX_COMPILER. Same-arch (x64) here; a cross build grows the native
# target and could compress the embedded face.
#
# SPDX-License-Identifier: MIT
import os
import shlex
import subprocess
import sys

# vtable-redirect exclusion list (data, not compiler policy): default it to the
# project's echo_noredirect.list so every wrapper build (face x86 + native arm64,
# both spawned below) keeps host-internal polymorphic types (std::locale facets &c.)
# on their native vtable. Owned by the project, editable without rebuilding clang.
os.environ.setdefault(
    "ECHO_NOREDIRECT_FILE",
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                 "echo_noredirect.list"))

# One wrapper for both CMAKE_C_COMPILER and CMAKE_CXX_COMPILER: pick the real
# driver by the source file's extension (g++/clang++ compile .c as C++, so a C file
# must go to clang). No source (link / --version) defaults to the C++ driver so C++
# libraries link libstdc++. ECHO_REAL_CC / ECHO_REAL_CXX override.
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REAL_CXX = os.environ.get("ECHO_REAL_CXX", "clang++")
_REAL_CC = os.environ.get("ECHO_REAL_CC",
                          os.path.join(os.path.dirname(_REAL_CXX), "clang"))
# Per-half target + flags, so one build can be cross-arch (native != face). Native
# defaults to the face target (same-arch). For an x86_64 guest -> aarch64 host set
# ECHO_NATIVE_TARGET=aarch64-...-echo, ECHO_NATIVE_FLAGS="--sysroot=... -isystem ...
# -fuse-ld=lld -B<gcc-dir>", ECHO_NATIVE_RT=<arm64 runtime dir>, ECHO_NATIVE_LINK=
# <its runtime link fragment>; the face stays x86_64.
FACE_TARGET = os.environ.get("ECHO_FACE_TARGET",
                             os.environ.get("ECHO_TARGET",
                                            "x86_64-unknown-linux-echo"))
NATIVE_TARGET = os.environ.get("ECHO_NATIVE_TARGET", FACE_TARGET)
NATIVE_FLAGS = shlex.split(os.environ.get("ECHO_NATIVE_FLAGS", ""))
FACE_FLAGS = shlex.split(os.environ.get("ECHO_FACE_FLAGS", ""))
# Cross-arch when the two halves target different machines: the native's system libraries
# are then a foreign ELF class and must never reach the face link.
CROSS_ARCH = NATIVE_TARGET.split("-")[0] != FACE_TARGET.split("-")[0]


def pick_real(args):
    for a in args:
        if a.endswith(".c"):
            return _REAL_CC
        if a.endswith((".cpp", ".cc", ".cxx", ".c++", ".C")):
            return _REAL_CXX
    return _REAL_CXX


REAL = _REAL_CXX  # set per-invocation in main()
RT = os.environ.get("ECHO_RT", os.path.join(_ROOT, "build", "runtime", "lib"))
NATIVE_RT = os.environ.get("ECHO_NATIVE_RT", RT)
STUBS = os.environ.get("ECHO_STUBS", os.path.join(_ROOT, "runtime", "echo_stubs.ld"))
OBJCOPY = os.environ.get(  # multi-arch objcopy (needed for cross-arch embed)
    "ECHO_OBJCOPY", os.path.join(os.path.dirname(_REAL_CXX), "llvm-objcopy"))
OBJ_SUFFIX = ".echoface"   # face object sidecar
SHADOW = "echo-face"       # face shared-lib shadow subdir

HOST_RT = shlex.split(os.environ.get(
    "ECHO_NATIVE_LINK",
    "-L%s -Wl,--no-as-needed -lEchoHostRuntime -Wl,--as-needed -lEchoVariadic "
    "-Wl,-rpath,%s -ldl" % (NATIVE_RT, NATIVE_RT)))
GUEST_RT = ["-Wl,--whole-archive", RT + "/libEchoGuestInitObj.a",
            RT + "/libEchoGuestHooks.a", "-Wl,--no-whole-archive",
            "-L" + RT, "-Wl,--no-as-needed",
            "-lEchoGuestRuntime", "-Wl,--as-needed", "-lEchoVariadic",
            "-Wl,-rpath," + RT, "-Wl,-T," + STUBS]

# Force the Echo ABI symbols into the dynamic export table on the native half. The
# bootstrap handshake dlsym()s the native's __echo_publish_<srchash> and the face's
# __echo_set_facevts / __echo_facevt$ / __echo_redisp$ points; a library that ships
# its own restrictive --version-script (OpenSSL's libcrypto.ld lists only its public
# API and makes everything else local) would otherwise hide these and break the
# handshake. We ADD echo_export.map (a named `ECHO_ABI { global: __echo_*; }` node) to
# the native link -- keeping the library's own version script so its version-node
# DEFINITIONS survive (e.g. Qt's qt_version_tag@Qt_6.x; stripping the script left those
# .symver references "undefined version" and broke the QtGui/QtCore native link).
# --export-dynamic-symbol is kept as belt-and-suspenders for the export table.
ECHO_EXPORT_MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "echo_export.map")
EXPORT_ECHO = ["-Wl,--version-script=" + ECHO_EXPORT_MAP,
               "-Wl,--export-dynamic-symbol=__echo_*"]


def expand(argv):
    out = []
    for a in argv:
        if a.startswith("@") and os.path.exists(a[1:]):
            with open(a[1:]) as f:
                out += expand(shlex.split(f.read()))
        else:
            out.append(a)
    return out


def strip_target(args):
    """Drop any incoming --target/-target: the wrapper owns the per-half target,
    and a CMAKE_CXX_COMPILER_TARGET (--target=<native>) injected by CMake would
    otherwise override the FACE pass's x86 target in a cross-arch build."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a.startswith("--target="):
            continue
        if a in ("-target", "--target"):
            skip = True
            continue
        out.append(a)
    return out


def run(args, target, extra=()):
    cmd = [REAL, "--target=" + target] + list(extra) + strip_target(args)
    if os.environ.get("ECHO_DEBUG"):
        sys.stderr.write("echo-cc: " + " ".join(cmd) + "\n")
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)


def strip_sysroot(args):
    """Drop --sysroot/-isysroot from FACE args. The face is always built for the
    x86 host with the default host toolchain; in a cross-arch build CMAKE_SYSROOT
    injects the NATIVE (arm64) sysroot onto every command, which must not reach the
    x86 face (it would pull arm64 crt/headers and break the link)."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a.startswith("--sysroot="):
            continue
        if a in ("--sysroot", "-isysroot"):
            skip = True
            continue
        out.append(a)
    return out


def strip_pch(args):
    """Drop -include-pch (clang '-Xclang -include-pch -Xclang <file>' or bare
    '-include-pch <file>') from FACE args. A PCH is target-specific; in a cross
    build the native (aarch64) PCH cannot be loaded by the x86 face, so the face
    compiles without it — any surviving '-include <header>' just re-parses the
    header textually, which is correct."""
    out, i, n = [], 0, len(args)
    while i < n:
        a = args[i]
        if a == "-Xclang" and i + 3 < n and args[i + 1] == "-include-pch" \
                and args[i + 2] == "-Xclang":
            i += 4
            continue
        if a == "-include-pch" and i + 1 < n:
            i += 2
            continue
        out.append(a)
        i += 1
    return out


def run_native(args):
    run(args, NATIVE_TARGET, NATIVE_FLAGS)


def run_face(args):
    run(strip_pch(strip_sysroot(args)), FACE_TARGET, FACE_FLAGS)


def whole_archive(args, to_face):
    """Wrap each ECHO-BUILT static archive in --whole-archive, for both halves.

    A linker pulls an archive member only when something already in the link refers to
    it. On the face side nothing does: every exported body has been replaced by a stub,
    so the intra-library references that pulled the member in the native link are gone.
    The member is dropped, and with it the stub for a function the guest is entitled to
    call. OpenSSL keeps ssl3_cbc_remove_padding_and_mac in providers/libcommon.a, and
    the face libcrypto simply did not have it: a test that calls it dies at "undefined
    symbol", with nothing to say an archive was involved.

    It has to be BOTH halves, not just the face. Doing it on the face alone gives the
    face translation units the native side does not have, and the publish handshake
    pairs them by source hash, so the first crossing aborts with "native lib missing
    __echo_publish_<hash>". Whole-archiving both keeps the two sets equal, which is the
    property the handshake actually depends on.

    Only an archive with a face shadow is touched. That is exactly the set Echo built,
    so a system .a keeps ordinary archive semantics and no dead system code is dragged
    in.
    """
    out = []
    for a in args:
        if a.endswith(".a"):
            shadow = os.path.join(os.path.dirname(a) or ".", SHADOW,
                                  os.path.basename(a))
            if os.path.exists(shadow):
                out += ["-Wl,--whole-archive", shadow if to_face else a,
                        "-Wl,--no-whole-archive"]
                continue
        out.append(a)
    return out


def find_out(args):
    for i, a in enumerate(args):
        if a == "-o" and i + 1 < len(args):
            return args[i + 1]
    return None


def link_name(soname_file):
    """libX.so.6.8.4 -> libX.so (the name a bare -lX resolves to)."""
    i = soname_file.find(".so")
    return soname_file[:i + 3] if i >= 0 else soname_file


def shadow_face_exists(name, libdirs):
    """Is there an echo-face copy of -l<name> in any -L dir?"""
    for d in libdirs:
        if os.path.exists(os.path.join(d, SHADOW, "lib" + name + ".so")):
            return True
    return False


def symlink(target, path):
    if os.path.lexists(path):
        os.remove(path)
    os.symlink(target, path)


SRC_EXTS = (".c", ".cpp", ".cc", ".cxx", ".c++", ".C", ".m", ".mm", ".S", ".s")


def main():
    global REAL
    args = expand(sys.argv[1:])
    REAL = pick_real(args)
    is_c = "-c" in args
    is_shared = "-shared" in args
    # A relocatable (partial) link is not a program: it produces another object file, and
    # lld refuses -l entirely in -r mode. glib turns a compiled .gresource into an object
    # this way (`-r -Wl,-b,binary -nostdlib`), and adding the host runtime to it fails with
    # "unable to find library -lEchoHostRuntime", which reads like a path problem and is
    # not one.
    is_partial = ("-r" in args or "-Wl,-r" in args or "--relocatable" in args
                  or "-nostdlib" in args)
    out = find_out(args)

    # ---- native pass (primary output) ----
    if is_shared:
        run_native(whole_archive(args, to_face=False) + HOST_RT + EXPORT_ECHO)
    elif not is_c and not is_partial and (
            (out and any(a.endswith((".o", ".a")) or a.startswith("-l") for a in args)) or
            any(a.endswith(SRC_EXTS) and not a.startswith("-") for a in args)):
        # Executable link (a feature probe) -> a runnable native binary.
        #
        # No -o required in the source-file case: autoconf's very first check compiles
        # conftest.c with no output flag at all and expects a.out, and ICU's configure
        # fails with "the C compiler works... no" when that link is missing HOST_RT.
        #
        # The second test matters: a build system often compiles ONE source straight to an
        # executable, with no .o, no .a and no -l anywhere. meson's compiler sanity check
        # does exactly that, and so does autoconf's AC_TRY_LINK. Without HOST_RT those
        # links fail on __echo_bind_match, which the emitted __echo_publish_<hash> needs,
        # and the build system concludes the compiler "cannot compile programs".
        run_native(args + HOST_RT)
        return
    else:
        run_native(args)      # compile / preprocess / --version / other

    # 没有 -o 的 `-c` 编译:编译器把 foo.c 写成当前目录的 foo.o。以前这里直接 return,
    # face 那一遍整个不跑,于是没有 foo.o.echoface,后续链接报
    # "no such file or directory: foo.o.echoface"。autoconf/zlib 的 configure 探测就是
    # 这么写的(`$CC -c -O3 -fPIC conftest.c`),结果被判成"编译器不支持建共享库",
    # 悄悄退回只建静态库——库建出来了,车道却什么都测不到。
    if not out and is_c:
        for a in args:
            if a.startswith("-"):
                continue
            base, dot, ext = a.rpartition(".")
            if dot and ext.lower() in ("c", "cc", "cpp", "cxx", "c++", "m", "mm", "s"):
                out = os.path.basename(base) + ".o"
                break
    if not out:
        return

    # ---- face pass ----
    if is_c:
        if "-emit-pch" in args:
            return   # PCH is target-specific; the x86 face compiles without one
        face_args, skip = [], False
        for a in args:
            if skip:
                face_args.append(out + OBJ_SUFFIX)
                skip = False
                continue
            if a == "-o":
                face_args.append("-o")
                skip = True
                continue
            face_args.append(a)
        # 原命令行没有 -o 时,上面的循环不会改写输出名,face 会和 native 抢同一个 foo.o。
        # 显式补一个。
        if "-o" not in args:
            face_args += ["-o", out + OBJ_SUFFIX]
        run_face(["-mllvm", "-echo-emit-face"] + face_args)
        return

    if is_shared:
        libdirs = [a[2:] for a in args if a.startswith("-L")]
        nat_dir, nat_base = os.path.dirname(out) or ".", os.path.basename(out)
        shadow_dir = os.path.join(nat_dir, SHADOW)
        os.makedirs(shadow_dir, exist_ok=True)
        face_out = os.path.join(shadow_dir, nat_base)

        face_args, skip = [], False
        for a in args:
            if skip:
                skip = False
                continue
            if a == "-o":
                skip = True
                continue
            if a == out:
                continue
            # .lo 是 libtool/zlib 给 PIC 对象用的后缀。只认 ".o" 的话,face 链接会把
            # native(跨架构时是 aarch64)的 .lo 原样喂给 x86 的 face 链接,报一串
            # "inflate.lo is incompatible with elf64-x86-64"。
            if a.endswith(".o") or a.endswith(".lo"):
                face_args.append(a + OBJ_SUFFIX)                 # object -> face object
            elif a.endswith(".a"):
                # -> face archive, whole (see whole_archive)
                face_args.extend(whole_archive([a], to_face=True))
            elif not a.startswith("-") and (a.endswith(".so")
                                            or ".so." in os.path.basename(a)):
                # A PATH, not an option: `-Wl,-soname,libQt6Widgets.so.6` also contains
                # ".so." and must not be mistaken for a library to rewrite or drop.
                # a full-path Echo .so dep (e.g. Test links lib/libQt6Core.so.6.8.4):
                # the face link must use the dep's FACE copy, not the native (which is
                # the wrong arch cross-arch, and the wrong world same-arch). Mirrors
                # the .a rewrite above.
                fa = os.path.join(os.path.dirname(a) or ".", SHADOW,
                                  os.path.basename(a))
                if os.path.exists(fa):
                    face_args.append(fa)                 # -> face .so
                elif not CROSS_ARCH:
                    face_args.append(a)                  # same-arch system lib: fine
                # else: a full-path SYSTEM library from the native sysroot (cmake passes
                # e.g. .../aarch64-linux-gnu/libxkbcommon.so for the xcb plugin). It has
                # no face copy and, cross-arch, is the wrong ELF class -- linking it into
                # the x86 face fails outright. Drop it: the face never calls those
                # symbols, every use of them is inside the native half. Same reasoning as
                # the -l<name> case below.
            elif CROSS_ARCH and a.startswith("-Wl,") and "-z,now" in a:
                # Same reason as the undefined symbols just below, one step further: with
                # `-z now` (Qt passes -Wl,-z,relro,-z,now) the loader must bind EVERY
                # symbol when the face is loaded, so leaving them undefined is not enough
                # -- keep relro, drop the eager binding, and the untaken references are
                # never resolved. Rebuilt token-wise because cmake sends them combined.
                parts = a[len("-Wl,"):].split(",")
                kept, i = [], 0
                while i < len(parts):
                    if parts[i] == "-z" and i + 1 < len(parts) and parts[i + 1] == "now":
                        i += 2
                        continue
                    kept.append(parts[i])
                    i += 1
                if kept:
                    face_args.append("-Wl," + ",".join(kept))
            elif CROSS_ARCH and a in ("-Wl,--no-undefined", "-Wl,-z,defs",
                                      "-Wl,--no-allow-shlib-undefined"):
                pass
                # Cross-arch the face cannot resolve the native's system libraries (they
                # were dropped above as a foreign ELF class), yet emit-face keeps the
                # bodies of NON-exported functions, and those still reference e.g.
                # xcb_shape_get_rectangles. Those paths are dead in the face -- every
                # entry point into them is a stub that crosses to the native half, where
                # the real libraries are -- so let the face keep undefined symbols rather
                # than link a foreign library just to satisfy the linker. Same-arch keeps
                # --no-undefined: there the host's own libraries do resolve them.
            elif a.startswith("-L"):
                face_args.append("-L" + os.path.join(a[2:], SHADOW))  # dep faces first
                face_args.append(a)
            elif a.startswith("-l"):
                if shadow_face_exists(a[2:], libdirs):
                    face_args.append(a)                          # echo dep -> its face
                # else: a system lib the face doesn't reference -> drop
            else:
                face_args.append(a)                             # -Wl,-soname, defines...
        # Use lld for the face link too (the native link already does): a version script
        # that versions the linker-defined section-boundary symbols (__stop_echo_resolvers
        # @@Qt_6 etc.) makes GNU ld fail "version node not found / failed to set dynamic
        # section sizes" -- lld handles it. Surfaced building the xcb QPA face library.
        run_face(face_args + ["-o", face_out, "-fuse-ld=lld"] + GUEST_RT + EXPORT_ECHO)

        # soname / link-name symlinks in the shadow so `-lX` resolves there
        ln = link_name(nat_base)
        if ln != nat_base:
            symlink(nat_base, os.path.join(shadow_dir, ln))
        for a in args:
            son = None
            if a.startswith("-Wl,-soname,"):
                son = a.split(",", 2)[2]
            elif a.startswith("-Wl,-soname="):
                son = a.split("=", 1)[1]
            if son and son != nat_base:
                symlink(nat_base, os.path.join(shadow_dir, son))

        # embed the (shadow) face into the native for shipping; keep the shadow copy
        subprocess.run([OBJCOPY, "--add-section", ".echo_face=" + face_out,
                        "--set-section-flags", ".echo_face=readonly,noload",
                        out], check=True)


if __name__ == "__main__":
    main()
