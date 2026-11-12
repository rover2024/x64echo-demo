#!/usr/bin/env bash

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$HERE/../../scripts/env.sh"

if [[ "$X64ECHO_BUILD_RUNTIME" = 1 ]]; then
    "$ROOT/scripts/build-runtime.sh"
fi

BUILD=${X64ECHO_BUILD_DIR:-"$ROOT/build/hello-cpp"}
mkdir -p "$BUILD"

x64echo_require_file "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestRuntime.so"
x64echo_require_file "$X64ECHO_HOST_RUNTIME_LIB/libEchoHostRuntime.so"

CFLAGS=(-O2 -g -fPIC -Wall -Wextra -I"$HERE")
read -r -a native_flags <<<"${X64ECHO_NATIVE_FLAGS:-}"
read -r -a face_flags <<<"${X64ECHO_FACE_FLAGS:-}"

"$X64ECHO_CLANG" --target="$X64ECHO_TARGET" "${native_flags[@]}" "${CFLAGS[@]}" \
    -c "$HERE/hello.cpp" -o "$BUILD/hello_native.o"

"$X64ECHO_HOST_CXX" -shared -o "$BUILD/libhello_host.so" \
    "$BUILD/hello_native.o" \
    -L"$X64ECHO_HOST_RUNTIME_LIB" -Wl,--no-as-needed \
    -lEchoHostRuntime -Wl,--as-needed -lEchoVariadic \
    -Wl,-rpath,"$X64ECHO_HOST_RUNTIME_LIB" -ldl

"$X64ECHO_CLANG" --target="$X64ECHO_FACE_TARGET" "${face_flags[@]}" \
    -mllvm -echo-emit-face "${CFLAGS[@]}" \
    -c "$HERE/hello.cpp" -o "$BUILD/hello_face.o"

"$X64ECHO_GUEST_CXX" -fPIC -shared -o "$BUILD/libhello.so" \
    "$BUILD/hello_face.o" \
    -Wl,--whole-archive \
    "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestInitObj.a" \
    "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestHooks.a" \
    -Wl,--no-whole-archive \
    -L"$X64ECHO_GUEST_RUNTIME_LIB" -Wl,--no-as-needed \
    -lEchoGuestRuntime -Wl,--as-needed -lEchoVariadic \
    -Wl,-rpath,"$X64ECHO_GUEST_RUNTIME_LIB" \
    -Wl,-T,"$ROOT/runtime/echo_stubs.ld"

"$X64ECHO_GUEST_CXX" -O2 -g -I"$HERE" \
    -o "$BUILD/hello_guest" "$HERE/main.cpp" \
    -L"$BUILD" -lhello -Wl,-rpath,'$ORIGIN'

qemu_args=()
if [[ -n "${X64ECHO_X86_SYSROOT:-}" ]]; then
    qemu_args+=(-L "$X64ECHO_X86_SYSROOT")
fi

output=$(
    LD_LIBRARY_PATH="$BUILD:$X64ECHO_HOST_RUNTIME_LIB" \
        "$X64ECHO_QEMU" "${qemu_args[@]}" \
        -plugin "$X64ECHO_PLUGIN" \
        -E LD_LIBRARY_PATH="$BUILD:$X64ECHO_GUEST_RUNTIME_LIB" \
        -E LD_PRELOAD="$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestThreadHook.so:$X64ECHO_GUEST_RUNTIME_LIB/libechoheap_shim.so" \
        "$BUILD/hello_guest" 2>&1
)

printf '%s\n' "$output"

for expected in \
    'virtual call: 21' \
    'guest virtual callback: 12, guest sum: 6' \
    'guest function callback: 11' \
    'cross-boundary exception: value must be positive'; do
    if ! grep -Fq "$expected" <<<"$output"; then
        echo "missing expected output: $expected" >&2
        exit 1
    fi
done

echo "hello-cpp: OK"
