#!/usr/bin/env bash

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export ROOT

if [[ -f "$ROOT/.cache/config.sh" ]]; then
    # shellcheck disable=SC1091
    . "$ROOT/.cache/config.sh"
fi

: "${X64ECHO_CLANG:?Set X64ECHO_CLANG in .cache/config.sh}"
: "${X64ECHO_QEMU:?Set X64ECHO_QEMU in .cache/config.sh}"
: "${X64ECHO_PLUGIN:?Set X64ECHO_PLUGIN in .cache/config.sh}"

export X64ECHO_TARGET=${X64ECHO_TARGET:-x86_64-unknown-linux-echo}
export X64ECHO_HOST_CC=${X64ECHO_HOST_CC:-cc}
export X64ECHO_HOST_CXX=${X64ECHO_HOST_CXX:-c++}
export X64ECHO_GUEST_CC=${X64ECHO_GUEST_CC:-x86_64-linux-gnu-gcc}
export X64ECHO_GUEST_CXX=${X64ECHO_GUEST_CXX:-x86_64-linux-gnu-g++}
export X64ECHO_RUNTIME_BUILD=${X64ECHO_RUNTIME_BUILD:-"$ROOT/build/runtime"}
export X64ECHO_RUNTIME_LIB=${X64ECHO_RUNTIME_LIB:-"$X64ECHO_RUNTIME_BUILD/lib"}
export X64ECHO_HOST_RUNTIME_LIB=${X64ECHO_HOST_RUNTIME_LIB:-"$X64ECHO_RUNTIME_LIB"}
export X64ECHO_GUEST_RUNTIME_LIB=${X64ECHO_GUEST_RUNTIME_LIB:-"$X64ECHO_RUNTIME_LIB"}
export ECHO_NOREDIRECT_FILE=${ECHO_NOREDIRECT_FILE:-"$ROOT/echo_noredirect.list"}

x64echo_require_file() {
    [[ -e "$1" ]] || {
        printf 'x64echo: required file is missing: %s\n' "$1" >&2
        exit 2
    }
}

x64echo_require_command() {
    command -v "$1" >/dev/null 2>&1 || {
        printf 'x64echo: required command is missing: %s\n' "$1" >&2
        exit 2
    }
}
