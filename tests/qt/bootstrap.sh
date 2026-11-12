#!/usr/bin/env bash

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$HERE/../../scripts/env.sh"

: "${X64ECHO_DEVKIT:?Run scripts/bootstrap.sh before building Qt}"

QT_VERSION=6.8.4
QT_TAG=v6.8.4-lts-lgpl
QT_COMMIT=ed77a3ca9ef1bf5e33b6f32ea41110734fb14e88
CACHE=${X64ECHO_QT_CACHE:-"$ROOT/.cache/qt/$QT_VERSION"}
SRC=$CACHE/qtbase
HOST_BUILD=$CACHE/host-build
HOST_PREFIX=$CACHE/host
TARGET_BUILD=$CACHE/target-build
JOBS=${X64ECHO_JOBS:-$(nproc)}

mkdir -p "$CACHE"
if [[ ! -d "$SRC/.git" ]]; then
    git clone --depth 1 --branch "$QT_TAG" \
        https://github.com/qt/qtbase.git "$SRC"
fi
actual_commit=$(git -C "$SRC" rev-parse HEAD)
[[ "$actual_commit" = "$QT_COMMIT" ]] || {
    echo "Qt source commit mismatch: expected $QT_COMMIT, got $actual_commit" >&2
    exit 2
}

if [[ ! -f "$HOST_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    cmake -S "$SRC" -B "$HOST_BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$HOST_PREFIX" \
        -DQT_BUILD_TESTS=OFF \
        -DQT_BUILD_EXAMPLES=OFF \
        -DFEATURE_gui=OFF \
        -DFEATURE_widgets=OFF \
        -DFEATURE_dbus=OFF \
        -DFEATURE_icu=OFF \
        -DFEATURE_glib=OFF \
        -DFEATURE_zstd=OFF \
        -DFEATURE_system_pcre2=OFF \
        -DFEATURE_system_zlib=OFF \
        -DFEATURE_system_doubleconversion=OFF
    cmake --build "$HOST_BUILD" --parallel "$JOBS" --target install
fi

host_moc=$HOST_PREFIX/libexec/moc
x64echo_require_file "$host_moc"

wrap_flags=$(
    grep -vE '^\s*#|^\s*$' "$ROOT/src/libs/HostHooks/echo_stdio.wrap" |
        sed 's/^/-Wl,--wrap=/' | tr '\n' ' '
)

export ECHO_REAL_CC="$X64ECHO_DEVKIT/bin/clang"
export ECHO_REAL_CXX="$X64ECHO_DEVKIT/bin/clang++"
export ECHO_RT="$X64ECHO_GUEST_RUNTIME_LIB"
export ECHO_NATIVE_RT="$X64ECHO_HOST_RUNTIME_LIB"
export ECHO_STUBS="$ROOT/runtime/echo_stubs.ld"
export ECHO_NOREDIRECT_FILE="$ROOT/echo_noredirect.list"
export ECHO_NATIVE_TARGET=aarch64-unknown-linux-echo
export ECHO_SYSROOT="$X64ECHO_DEVKIT/aarch64/sysroot"
export ECHO_NATIVE_FLAGS="--sysroot=$ECHO_SYSROOT -B$ECHO_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/13 -L$ECHO_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/13 -L$ECHO_SYSROOT/usr/lib/aarch64-linux-gnu -isystem $ECHO_SYSROOT/usr/include/c++/13 -isystem $ECHO_SYSROOT/usr/include/aarch64-linux-gnu/c++/13 -fuse-ld=lld"
export ECHO_FACE_FLAGS="--sysroot=$X64ECHO_DEVKIT/x86_64/sysroot -B$X64ECHO_DEVKIT/x86_64/sysroot/usr/lib/gcc/x86_64-linux-gnu/13 -L$X64ECHO_DEVKIT/x86_64/sysroot/usr/lib -L$X64ECHO_DEVKIT/x86_64/sysroot/usr/lib/x86_64-linux-gnu -isystem $X64ECHO_DEVKIT/x86_64/sysroot/usr/include/c++/13 -isystem $X64ECHO_DEVKIT/x86_64/sysroot/usr/include/c++/13/x86_64-linux-gnu -fuse-ld=lld"
export ECHO_NATIVE_LINK="-L$ECHO_NATIVE_RT -Wl,--no-as-needed -lEchoHostRuntime -Wl,--as-needed -lEchoVariadic -Wl,--no-as-needed -lEchoHostHooks -Wl,--as-needed $wrap_flags -Wl,-rpath,$ECHO_NATIVE_RT -ldl"
export ECHO_OBJCOPY="$X64ECHO_DEVKIT/bin/llvm-objcopy"
export ECHO_READELF="$X64ECHO_DEVKIT/bin/llvm-readelf"
export ECHO_SYNC_MUTABLE=1
export PATH="$X64ECHO_DEVKIT/bin:$PATH"

if [[ ! -f "$TARGET_BUILD/build.ninja" ]]; then
    cmake -S "$SRC" -B "$TARGET_BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/qt/echo-toolchain-wrap.cmake" \
        -DQT_HOST_PATH="$HOST_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DQT_BUILD_TESTS=OFF \
        -DQT_BUILD_EXAMPLES=OFF \
        -DFEATURE_gui=OFF \
        -DFEATURE_widgets=OFF \
        -DFEATURE_dbus=OFF \
        -DFEATURE_icu=OFF \
        -DFEATURE_glib=OFF \
        -DFEATURE_zstd=OFF \
        -DFEATURE_system_pcre2=OFF \
        -DFEATURE_system_zlib=OFF \
        -DFEATURE_system_doubleconversion=OFF
fi

cmake --build "$TARGET_BUILD" --parallel "$JOBS" --target Core Test

cat > "$CACHE/config.sh" <<EOF
export X64ECHO_QT_SOURCE=$SRC
export X64ECHO_QT_FACE_DIR=$TARGET_BUILD/lib/echo-face
export X64ECHO_QT_NATIVE_DIR=$TARGET_BUILD/lib
export X64ECHO_QT_INCLUDE=$HOST_PREFIX/include
export X64ECHO_QT_MOC=$host_moc
EOF

echo "Qt Echo testkit ready: $CACHE"
echo "configuration: $CACHE/config.sh"
