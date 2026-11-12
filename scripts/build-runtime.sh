#!/usr/bin/env bash

set -eu

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$SCRIPT_DIR/env.sh"

x64echo_require_file "$X64ECHO_CLANG"
x64echo_require_command "$X64ECHO_HOST_CC"
x64echo_require_command "$X64ECHO_HOST_CXX"
x64echo_require_command "$X64ECHO_GUEST_CC"

cmake -S "$ROOT" -B "$X64ECHO_RUNTIME_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$X64ECHO_HOST_CC" \
    -DCMAKE_CXX_COMPILER="$X64ECHO_HOST_CXX" \
    -DECHO_CHECK_DLADDR=ON \
    -DECHO_BUILD_QEMU_MODULES=OFF

cmake --build "$X64ECHO_RUNTIME_BUILD" --parallel

"$X64ECHO_HOST_CC" -O2 -fPIC -shared \
    -o "$X64ECHO_HOST_RUNTIME_LIB/libechoheap_host.so" \
    "$ROOT/runtime/echoheap_host.c"

"$X64ECHO_GUEST_CC" -O2 -fPIC -shared -nostdlib \
    -fno-stack-protector -ffreestanding \
    -o "$X64ECHO_GUEST_RUNTIME_LIB/libechoheap_shim.so" \
    "$ROOT/runtime/echoheap_shim.c"

echo "runtime ready: $X64ECHO_RUNTIME_LIB"
