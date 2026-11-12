# LLVM Integration

X64Echo extends LLVM and Clang with the Echo ABI. The public repository keeps
only the patch set and build helpers; the full upstream source tree is not
vendored.

The current patch baseline is:

```text
llvmorg-21.1.6
```

## Fetch

```bash
./llvm/scripts/fetch-llvm.sh
```

This creates a shallow checkout under `build/llvm-project`.

## Apply

```bash
./llvm/scripts/apply-patches.sh
```

The script applies every patch in `llvm/patches/` in filename order and stops
on the first failure.

## Build

Use the upstream LLVM build instructions with Clang and LLD enabled. The
developer preview targets one guest/host pair, so build only the two required
backends:

```cmake
-DLLVM_ENABLE_PROJECTS="clang;lld"
-DLLVM_TARGETS_TO_BUILD="X86;AArch64"
```

For the AArch64 devkit, also use an AArch64 GNU sysroot and:

```cmake
-DLLVM_DEFAULT_TARGET_TRIPLE="aarch64-linux-gnu"
-DLLVM_HOST_TRIPLE="aarch64-linux-gnu"
```

The public AArch64 devkit pins the tested Echo Clang build. To use a custom
compiler instead:

```bash
export X64ECHO_CLANG=/path/to/echo-clang++
```

as described in [../docs/build-and-run.md](../docs/build-and-run.md).
