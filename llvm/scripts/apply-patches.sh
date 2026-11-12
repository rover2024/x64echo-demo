#!/usr/bin/env bash

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SRC=${LLVM_SRC_DIR:-"$ROOT/build/llvm-project"}
PATCH_DIR="$ROOT/llvm/patches"

[[ -d "$SRC/.git" ]] || {
    echo "missing LLVM checkout: $SRC" >&2
    echo "run llvm/scripts/fetch-llvm.sh first" >&2
    exit 2
}

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
[[ ${#patches[@]} -gt 0 ]] || {
    echo "no patches found in $PATCH_DIR" >&2
    exit 2
}

for patch in "${patches[@]}"; do
    echo "applying $(basename "$patch")"
    git -C "$SRC" apply --index "$patch"
done

echo "LLVM Echo patches applied"
