#!/usr/bin/env bash

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${1:-$(uname -m)}
VERSION=${X64ECHO_VERSION:-0.1.0-dev}
TAG=${X64ECHO_RELEASE_TAG:-v$VERSION}
NAME=x64echo-devkit-$ARCH-$VERSION
CACHE=$ROOT/.cache/devkit/$VERSION
ARCHIVE=${X64ECHO_DEVKIT_ARCHIVE:-$CACHE/$NAME.tar.xz}
CHECKSUM=${X64ECHO_DEVKIT_CHECKSUM:-$ARCHIVE.sha256}

case "$ARCH" in
    x86_64|aarch64) ;;
    *)
        echo "unsupported host architecture: $ARCH" >&2
        exit 2
        ;;
esac

mkdir -p "$CACHE"
if [[ ! -f "$ARCHIVE" ]]; then
    URL=${X64ECHO_DEVKIT_URL:-"https://github.com/rover2024/x64echo-demo/releases/download/$TAG/$NAME.tar.xz"}
    echo "downloading $URL"
    curl -fL --retry 3 -o "$ARCHIVE" "$URL"
    curl -fL --retry 3 -o "$CHECKSUM" "$URL.sha256"
fi

if [[ -f "$CHECKSUM" ]]; then
    expected=$(cut -d' ' -f1 "$CHECKSUM")
    actual=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
    [[ "$expected" = "$actual" ]] || {
        echo "devkit checksum mismatch" >&2
        exit 2
    }
fi

rm -rf "$CACHE/$NAME"
tar -xf "$ARCHIVE" -C "$CACHE"
DEVKIT=$CACHE/$NAME
AARCH64_GCC=$DEVKIT/aarch64/sysroot/usr/lib/gcc/aarch64-linux-gnu/13
AARCH64_LIB=$DEVKIT/aarch64/sysroot/usr/lib/aarch64-linux-gnu
X86_GCC=$DEVKIT/x86_64/sysroot/usr/lib/gcc/x86_64-linux-gnu/13
X86_LIB=$DEVKIT/x86_64/sysroot/usr/lib
X86_MULTIARCH_LIB=$DEVKIT/x86_64/sysroot/usr/lib/x86_64-linux-gnu
AARCH64_CXX_INCLUDE=$DEVKIT/aarch64/sysroot/usr/include/c++/13
AARCH64_CXX_TARGET_INCLUDE=$DEVKIT/aarch64/sysroot/usr/include/aarch64-linux-gnu/c++/13
X86_CXX_INCLUDE=$DEVKIT/x86_64/sysroot/usr/include/c++/13
X86_CXX_TARGET_INCLUDE=$DEVKIT/x86_64/sysroot/usr/include/c++/13/x86_64-linux-gnu

for tool in clang clang++ ld.lld llvm-objcopy llvm-readelf; do
    [[ -x "$DEVKIT/bin/$tool" ]] || {
        echo "devkit is missing $tool" >&2
        exit 2
    }
done

guest_cc=${X64ECHO_GUEST_CC:-x86_64-linux-gnu-gcc}
guest_cxx=${X64ECHO_GUEST_CXX:-x86_64-linux-gnu-g++}
command -v "$guest_cc" >/dev/null || {
    echo "missing x86-64 guest compiler: $guest_cc" >&2
    echo "install gcc-x86-64-linux-gnu and g++-x86-64-linux-gnu" >&2
    exit 2
}
command -v "$guest_cxx" >/dev/null || {
    echo "missing x86-64 guest compiler: $guest_cxx" >&2
    exit 2
}

qemu=${X64ECHO_QEMU:-qemu-x86_64}
command -v "$qemu" >/dev/null || {
    echo "missing qemu-x86_64" >&2
    exit 2
}
plugin=${X64ECHO_PLUGIN:-}
[[ -n "$plugin" && -f "$plugin" ]] || {
    echo "set X64ECHO_PLUGIN to the QEMU 11.1+ libdlcall.so" >&2
    exit 2
}

mkdir -p "$ROOT/.cache"
cat > "$ROOT/.cache/config.sh" <<EOF
export X64ECHO_DEVKIT=$DEVKIT
export X64ECHO_CLANG=$DEVKIT/bin/clang++
export X64ECHO_TARGET=aarch64-unknown-linux-echo
export X64ECHO_FACE_TARGET=x86_64-unknown-linux-echo
export X64ECHO_NATIVE_FLAGS="--sysroot=$DEVKIT/aarch64/sysroot -B$AARCH64_GCC -L$AARCH64_GCC -L$AARCH64_LIB -isystem $AARCH64_CXX_INCLUDE -isystem $AARCH64_CXX_TARGET_INCLUDE"
export X64ECHO_FACE_FLAGS="--sysroot=$DEVKIT/x86_64/sysroot -B$X86_GCC -L$X86_LIB -L$X86_MULTIARCH_LIB -isystem $X86_CXX_INCLUDE -isystem $X86_CXX_TARGET_INCLUDE"
export X64ECHO_HOST_CC=$(command -v cc)
export X64ECHO_HOST_CXX=$(command -v c++)
export X64ECHO_GUEST_CC=$guest_cc
export X64ECHO_GUEST_CXX=$guest_cxx
export X64ECHO_QEMU=$(command -v "$qemu")
export X64ECHO_PLUGIN=$(realpath "$plugin")
export X64ECHO_X86_SYSROOT=$DEVKIT/x86_64/sysroot
export X64ECHO_HOST_RUNTIME_LIB=$DEVKIT/aarch64/lib
export X64ECHO_GUEST_RUNTIME_LIB=$DEVKIT/x86_64/lib
export X64ECHO_BUILD_RUNTIME=0
EOF

echo "devkit ready: $DEVKIT"
echo "configuration: $ROOT/.cache/config.sh"
