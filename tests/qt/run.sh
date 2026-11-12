#!/usr/bin/env bash

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$HERE/../../scripts/env.sh"

: "${X64ECHO_QT_SOURCE:?Set X64ECHO_QT_SOURCE to the Qt 6.8.4 source tree}"
: "${X64ECHO_QT_FACE_DIR:?Set X64ECHO_QT_FACE_DIR to the Echo face libraries}"
: "${X64ECHO_QT_NATIVE_DIR:?Set X64ECHO_QT_NATIVE_DIR to the Echo native libraries}"
: "${X64ECHO_QT_INCLUDE:?Set X64ECHO_QT_INCLUDE to the matching host Qt headers}"
: "${X64ECHO_QT_MOC:?Set X64ECHO_QT_MOC to the matching moc executable}"

x64echo_require_file "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestRuntime.so"
x64echo_require_file "$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestThreadHook.so"
x64echo_require_file "$X64ECHO_GUEST_RUNTIME_LIB/libechoheap_shim.so"

BUILD=${X64ECHO_QT_TEST_BUILD:-"$ROOT/build/qt-quick"}
mkdir -p "$BUILD/face" "$BUILD/native" "$BUILD/tests"

for module in Core Test; do
    face_lib=$(find "$X64ECHO_QT_FACE_DIR" -maxdepth 1 \
        -name "libQt6${module}.so.6.8.4" -print -quit)
    native_lib=$(find "$X64ECHO_QT_NATIVE_DIR" -maxdepth 1 \
        -name "libQt6${module}.so.6.8.4" -print -quit)
    x64echo_require_file "$face_lib"
    x64echo_require_file "$native_lib"

    ln -sfn "$face_lib" "$BUILD/face/libQt6${module}.so.6.8.4"
    ln -sfn "libQt6${module}.so.6.8.4" "$BUILD/face/libQt6${module}.so.6"
    ln -sfn "libQt6${module}.so.6.8.4" "$BUILD/face/libQt6${module}.so"

    ln -sfn "$native_lib" "$BUILD/native/libQt6${module}.so.6.8.4"
    ln -sfn "libQt6${module}.so.6.8.4" "$BUILD/native/libQt6${module}.so.6"
    ln -sfn "libQt6${module}.so.6.8.4" "$BUILD/native/libQt6${module}_host.so.6"
    ln -sfn "libQt6${module}.so.6.8.4" "$BUILD/native/libQt6${module}_host.so.6.8.4"
done

QT_INC=$X64ECHO_QT_INCLUDE
PRIVATE_INCLUDES=(
    -I"$QT_INC/QtCore/6.8.4"
    -I"$QT_INC/QtCore/6.8.4/QtCore"
    -I"$QT_INC/QtTest/6.8.4"
    -I"$QT_INC/QtTest/6.8.4/QtTest"
    -I"$X64ECHO_QT_SOURCE/mkspecs/linux-g++"
)
COMMON_INCLUDES=(
    -I"$QT_INC"
    -I"$QT_INC/QtCore"
    -I"$QT_INC/QtTest"
)

tests=(
    "text/qbytearray/tst_qbytearray"
    "text/qstringlist/tst_qstringlist"
    "tools/qbitarray/tst_qbitarray"
    "time/qdatetime/tst_qdatetime"
)

qemu_args=()
if [[ -n "${X64ECHO_X86_SYSROOT:-}" ]]; then
    qemu_args+=(-L "$X64ECHO_X86_SYSROOT")
fi

failed=0
for relative in "${tests[@]}"; do
    source="$X64ECHO_QT_SOURCE/tests/auto/corelib/${relative}.cpp"
    name=$(basename "$relative")
    output_dir="$BUILD/tests/$name"
    mkdir -p "$output_dir"

    x64echo_require_file "$source"
    includes=(
        "${COMMON_INCLUDES[@]}"
        "${PRIVATE_INCLUDES[@]}"
        -I"$output_dir"
        -I"$(dirname "$source")"
    )

    if grep -q "${name}.moc" "$source"; then
        "$X64ECHO_QT_MOC" "${includes[@]}" "$source" \
            -o "$output_dir/${name}.moc"
    fi

    "$X64ECHO_GUEST_CXX" -O2 -g -std=c++17 -fPIC -fexceptions \
        -DQTEST_THROW_ON_FAIL -DQT_CORE_LIB -DQT_TESTLIB_LIB \
        -w "${includes[@]}" \
        -o "$output_dir/$name" "$source" \
        -L"$BUILD/face" -lQt6Test -lQt6Core \
        -Wl,-rpath-link,"$X64ECHO_GUEST_RUNTIME_LIB" \
        -Wl,-rpath,"$BUILD/face" -pthread -ldl

    result=$(
        LD_LIBRARY_PATH="$BUILD/native:$X64ECHO_HOST_RUNTIME_LIB" \
            "$X64ECHO_QEMU" "${qemu_args[@]}" \
            -plugin "$X64ECHO_PLUGIN" \
            -E LD_LIBRARY_PATH="$BUILD/face:$X64ECHO_GUEST_RUNTIME_LIB" \
            -E LD_PRELOAD="$X64ECHO_GUEST_RUNTIME_LIB/libEchoGuestThreadHook.so:$X64ECHO_GUEST_RUNTIME_LIB/libechoheap_shim.so" \
            -E QTEST_NO_WATCHDOG=1 \
            "$output_dir/$name" 2>&1
    )
    printf '%s\n' "$result"

    totals=$(grep -aoE 'Totals: [0-9]+ passed, [0-9]+ failed' <<<"$result" |
        tail -1)
    if [[ -z "$totals" ]] || [[ "$totals" != *", 0 failed" ]]; then
        echo "Qt test failed: $name" >&2
        failed=1
    fi
done

[[ $failed -eq 0 ]] || exit 1
echo "Qt quick tests: OK"
