#!/usr/bin/env bash

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DEST=${LLVM_SRC_DIR:-"$ROOT/build/llvm-project"}
REF=${LLVM_BASE_REF:-llvmorg-21.1.6}

if [[ -d "$DEST/.git" ]]; then
    current=$(git -C "$DEST" rev-parse --short HEAD)
    echo "LLVM checkout already exists at $DEST ($current)"
    exit 0
fi

mkdir -p "$(dirname "$DEST")"
git clone --depth 1 --branch "$REF" \
    https://github.com/llvm/llvm-project.git "$DEST"

echo "LLVM ready: $DEST ($REF)"
