# Build and Run

The current developer workflow requires an Echo-enabled LLVM/Clang build and a
QEMU 11.1 or later `qemu-x86_64` with `libdlcall.so`.

## Configuration

Create a local configuration file:

```bash
cp config.example.sh .cache/config.sh
```

Set:

- `X64ECHO_CLANG`: the Echo-enabled Clang C++ driver;
- `X64ECHO_HOST_CC` and `X64ECHO_HOST_CXX`: native host compilers;
- `X64ECHO_GUEST_CC` and `X64ECHO_GUEST_CXX`: x86-64 cross compilers;
- `X64ECHO_QEMU`: a user-mode QEMU for x86-64 guests;
- `X64ECHO_PLUGIN`: the matching `libdlcall.so`;
- `X64ECHO_X86_SYSROOT`: an x86-64 guest sysroot when the host is not x86-64.

`.cache/` is ignored by Git.

## Runtime

```bash
./scripts/build-runtime.sh
```

The script builds the guest and host runtimes and compiles the heap bridge.

## Hello C++ Example

```bash
./examples/hello-cpp/run.sh
```

Expected output:

```text
virtual call: 21
guest virtual callback: 12, guest sum: 6
guest function callback: 11
cross-boundary exception: value must be positive
hello-cpp: OK
```

## Quick Performance Check

```bash
./benchmarks/quick/run.sh
```

The driver compares native execution, QEMU full emulation and QEMU with the
X64Echo face/native pair. It prints the checksum and elapsed time for each lane
and computes the full-emulation/X64Echo speedup. This is a small smoke
benchmark, not a replacement for the full workload suite.

## Qt Quick Tests

The Qt harness expects an existing Qt 6.8.4 Echo build and its matching host
headers. See [../tests/qt/README.md](../tests/qt/README.md).

## Current Restriction

The first source snapshot builds the host and face halves in a same-architecture
configuration. The prebuilt devkit path for AArch64 and RISC-V64 will replace
the local compiler configuration in a later phase.
