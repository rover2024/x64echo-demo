#!/usr/bin/env bash

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$HERE/../../scripts/env.sh"

if [[ "$X64ECHO_BUILD_RUNTIME" = 1 ]]; then
    "$ROOT/scripts/build-runtime.sh"
fi

BUILD=${X64ECHO_BENCH_BUILD:-"$ROOT/build/quick-bench"}
CALLS=${X64ECHO_BENCH_CALLS:-100000}
mkdir -p "$BUILD/native" "$BUILD/x86" "$BUILD/echo"

CFLAGS=(-O2 -g -fPIC -Wall -Wextra -I"$HERE")
read -r -a native_flags <<<"${X64ECHO_NATIVE_FLAGS:-}"
read -r -a face_flags <<<"${X64ECHO_FACE_FLAGS:-}"

# Native host lane.
"$X64ECHO_HOST_CXX" "${CFLAGS[@]}" -shared \
    -o "$BUILD/native/libwork.so" "$HERE/work.cpp"
"$X64ECHO_HOST_CXX" "${CFLAGS[@]}" \
    -o "$BUILD/native/bench" "$HERE/driver.cpp" \
    -L"$BUILD/native" -lwork -Wl,-rpath,'$ORIGIN'

# Plain x86-64 library for QEMU full emulation.
"$X64ECHO_GUEST_CXX" "${CFLAGS[@]}" -shared \
    -o "$BUILD/x86/libwork.so" "$HERE/work.cpp"
"$X64ECHO_GUEST_CXX" "${CFLAGS[@]}" \
    -o "$BUILD/x86/bench" "$HERE/driver.cpp" \
    -L"$BUILD/x86" -lwork -Wl,-rpath,'$ORIGIN'

# Echo face and native pair for the pass-through lane.
"$X64ECHO_CLANG" --target="$X64ECHO_TARGET" "${native_flags[@]}" "${CFLAGS[@]}" \
    -c "$HERE/work.cpp" -o "$BUILD/echo/work_native.o"
"$X64ECHO_HOST_CXX" -shared -o "$BUILD/echo/libwork_host.so" \
    "$BUILD/echo/work_native.o" \
    -L"$X64ECHO_HOST_RUNTIME_LIB" -Wl,--no-as-needed \
    -lEchoHostRuntime -Wl,--as-needed -lEchoVariadic \
    -Wl,-rpath,"$X64ECHO_HOST_RUNTIME_LIB" -ldl

"$X64ECHO_CLANG" --target="$X64ECHO_FACE_TARGET" "${face_flags[@]}" \
    -mllvm -echo-emit-face "${CFLAGS[@]}" \
    -c "$HERE/work.cpp" -o "$BUILD/echo/work_face.o"
"$X64ECHO_GUEST_CXX" -fPIC -shared -o "$BUILD/echo/libwork.so" \
    "$BUILD/echo/work_face.o" \
    -Wl,--whole-archive \
    "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestInitObj.a" \
    "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestHooks.a" \
    -Wl,--no-whole-archive \
    -L"$X64ECHO_GUEST_RUNTIME_LIB" -Wl,--no-as-needed \
    -lEchoGuestRuntime -Wl,--as-needed -lEchoVariadic \
    -Wl,-rpath,"$X64ECHO_GUEST_RUNTIME_LIB" \
    -Wl,-T,"$ROOT/runtime/echo_stubs.ld"
"$X64ECHO_GUEST_CXX" "${CFLAGS[@]}" \
    -o "$BUILD/echo/bench" "$HERE/driver.cpp" \
    -L"$BUILD/echo" -lwork -Wl,-rpath,'$ORIGIN'

qemu_args=()
if [[ -n "${X64ECHO_X86_SYSROOT:-}" ]]; then
    qemu_args+=(-L "$X64ECHO_X86_SYSROOT")
fi

best_native=
best_emulated=
best_echoed=

for _ in 1 2 3; do
    candidate=$("$BUILD/native/bench" "$CALLS")
    if [[ -z "$best_native" ]] ||
       awk -v a="$(sed -n 's/.*seconds=//p' <<<"$candidate")" \
           -v b="$(sed -n 's/.*seconds=//p' <<<"$best_native")" \
           'BEGIN { exit !(a < b) }'; then
        best_native=$candidate
    fi

    candidate=$(
        LD_LIBRARY_PATH="$BUILD/x86" \
            "$X64ECHO_QEMU" "${qemu_args[@]}" "$BUILD/x86/bench" "$CALLS"
    )
    if [[ -z "$best_emulated" ]] ||
       awk -v a="$(sed -n 's/.*seconds=//p' <<<"$candidate")" \
           -v b="$(sed -n 's/.*seconds=//p' <<<"$best_emulated")" \
           'BEGIN { exit !(a < b) }'; then
        best_emulated=$candidate
    fi

    candidate=$(
        LD_LIBRARY_PATH="$BUILD/echo:$X64ECHO_HOST_RUNTIME_LIB" \
            "$X64ECHO_QEMU" "${qemu_args[@]}" \
            -plugin "$X64ECHO_PLUGIN" \
            -E LD_LIBRARY_PATH="$BUILD/echo:$X64ECHO_GUEST_RUNTIME_LIB" \
            -E LD_PRELOAD="$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestThreadHook.so:$X64ECHO_GUEST_RUNTIME_LIB/libechoheap_shim.so" \
            "$BUILD/echo/bench" "$CALLS"
    )
    if [[ -z "$best_echoed" ]] ||
       awk -v a="$(sed -n 's/.*seconds=//p' <<<"$candidate")" \
           -v b="$(sed -n 's/.*seconds=//p' <<<"$best_echoed")" \
           'BEGIN { exit !(a < b) }'; then
        best_echoed=$candidate
    fi
done

printf 'native:   %s\n' "$best_native"
printf 'qemu:     %s\n' "$best_emulated"
printf 'x64echo:  %s\n' "$best_echoed"

emulated_s=$(sed -n 's/.*seconds=//p' <<<"$best_emulated")
echoed_s=$(sed -n 's/.*seconds=//p' <<<"$best_echoed")
awk -v e="$emulated_s" -v x="$echoed_s" \
    'BEGIN { printf "speedup:  %.2fx\n", e / x }'
