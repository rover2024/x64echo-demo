#!/usr/bin/env bash

set -eu

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

: "${X64ECHO_CROSS_CLANG:?Set X64ECHO_CROSS_CLANG to a host C cross compiler}"
: "${X64ECHO_CROSS_CXX:?Set X64ECHO_CROSS_CXX to a host C++ cross compiler}"
: "${X64ECHO_AARCH64_SYSROOT:?Set X64ECHO_AARCH64_SYSROOT to an AArch64 sysroot}"

OUT=${X64ECHO_AARCH64_RUNTIME_LIB:-"$ROOT/build/runtime-aarch64/lib"}
GCC="$X64ECHO_AARCH64_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/13"
MULTIARCH="$X64ECHO_AARCH64_SYSROOT/usr/lib/aarch64-linux-gnu"
CXX_INCLUDE=(
    -isystem "$X64ECHO_AARCH64_SYSROOT/usr/include/c++/13"
    -isystem "$X64ECHO_AARCH64_SYSROOT/usr/include/aarch64-linux-gnu/c++/13"
)
CROSS=(
    --target=aarch64-linux-gnu
    --sysroot="$X64ECHO_AARCH64_SYSROOT"
    -B"$GCC" -L"$GCC" -L"$MULTIARCH"
    -fuse-ld=lld
)
INCLUDES=(-I"$ROOT/include" -I"$ROOT/include/echo/HostRuntime")
OBJ=$(mktemp -d "$ROOT/build/.runtime-aarch64.XXXXXX")
trap 'rm -rf "$OBJ"' EXIT
mkdir -p "$OUT"

"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/HostRuntime/Entry.c" \
    -o "$OBJ/Entry.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/HostRuntime/CheckCall.c" \
    -o "$OBJ/CheckCall.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/HostRuntime/Publish.c" \
    -o "$OBJ/Publish.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/HostRuntime/ArenaHost.c" \
    -o "$OBJ/ArenaHost.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/Arena/Arena.c" \
    -o "$OBJ/Arena.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/HostRuntime/aarch64/Coroutine.S" \
    -o "$OBJ/Coroutine.o"

for source in Catch.cpp ReentryRethrow.cpp DynCast.cpp; do
    "$X64ECHO_CROSS_CXX" "${CROSS[@]}" "${CXX_INCLUDE[@]}" \
        -O2 -fPIC -fexceptions "${INCLUDES[@]}" \
        -c "$ROOT/src/libs/HostRuntime/$source" \
        -o "$OBJ/${source%.cpp}.o"
done

"$X64ECHO_CROSS_CXX" "${CROSS[@]}" "${CXX_INCLUDE[@]}" \
    -O2 -fPIC "${INCLUDES[@]}" \
    -c "$ROOT/src/libs/Arena/Operators.cpp" \
    -o "$OBJ/Operators.o"
"$X64ECHO_CROSS_CXX" "${CROSS[@]}" \
    -shared -o "$OUT/libEchoHostRuntime.so" "$OBJ"/*.o -ldl

"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC -shared -I"$ROOT/include" \
    -o "$OUT/libEchoHostHooks.so" \
    "$ROOT/src/libs/HostHooks/Stdio.c" \
    "$ROOT/src/libs/HostHooks/Env.c" \
    "$ROOT/src/libs/HostHooks/Locale.c" \
    -ldl

"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC -shared \
    -o "$OUT/libechoheap_host.so" "$ROOT/runtime/echoheap_host.c"

"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC \
    -I"$ROOT/include" -I"$ROOT/src/3rdparty" \
    -I"$X64ECHO_AARCH64_SYSROOT/usr/include/aarch64-linux-gnu" \
    -c "$ROOT/src/libs/Variadic/Extract.c" -o "$OBJ/VariadicExtract.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC \
    -I"$ROOT/include" -I"$ROOT/src/3rdparty" \
    -I"$X64ECHO_AARCH64_SYSROOT/usr/include/aarch64-linux-gnu" \
    -c "$ROOT/src/libs/Variadic/Replay.c" -o "$OBJ/VariadicReplay.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC \
    -I"$ROOT/include" -I"$ROOT/src/3rdparty" \
    -c "$ROOT/src/3rdparty/printf/printf.c" -o "$OBJ/Printf.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -O2 -fPIC \
    -I"$ROOT/include" -I"$ROOT/src/3rdparty" \
    -c "$ROOT/src/3rdparty/printf/printf_scanf.c" -o "$OBJ/PrintfScanf.o"
"$X64ECHO_CROSS_CLANG" "${CROSS[@]}" -shared -o "$OUT/libEchoVariadic.so" \
    "$OBJ/VariadicExtract.o" "$OBJ/VariadicReplay.o" \
    "$OBJ/Printf.o" "$OBJ/PrintfScanf.o" \
    "$MULTIARCH/libffi_pic.a"

echo "AArch64 host runtime ready: $OUT"
