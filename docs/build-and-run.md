# Build and Run

The current developer workflow requires an Echo-enabled LLVM/Clang build and a
QEMU 11.1 or later `qemu-x86_64` with `libdlcall.so`.

## AArch64 Devkit

The bootstrap script downloads `x64echo-devkit-aarch64-0.1.0-dev`, verifies
its checksum, and configures the checkout:

```bash
export X64ECHO_QEMU=/path/to/qemu-x86_64
export X64ECHO_PLUGIN=/path/to/libdlcall.so
./scripts/bootstrap.sh aarch64
```

The host also needs `x86_64-linux-gnu-gcc` and `x86_64-linux-gnu-g++` for
plain x86-64 guest lanes. The devkit contains an AArch64-hosted Echo Clang,
the AArch64 host runtime, the x86-64 guest runtime, and both sysroots. Its
LLVM build includes only the X86 and AArch64 backends.

## Manual Configuration

Create a local configuration file:

```bash
cp config.example.sh .cache/config.sh
```

Set:

- `X64ECHO_CLANG`: the Echo-enabled Clang C++ driver;
- `X64ECHO_TARGET`: the native library target;
- `X64ECHO_FACE_TARGET`: the guest-facing library target, normally
  `x86_64-unknown-linux-echo`;
- `X64ECHO_HOST_CC` and `X64ECHO_HOST_CXX`: native host compilers;
- `X64ECHO_GUEST_CC` and `X64ECHO_GUEST_CXX`: x86-64 cross compilers;
- `X64ECHO_QEMU`: a user-mode QEMU for x86-64 guests;
- `X64ECHO_PLUGIN`: the matching `libdlcall.so`;
- `X64ECHO_X86_SYSROOT`: an x86-64 guest sysroot when the host is not x86-64.
- `X64ECHO_BUILD_RUNTIME=0` when using runtimes shipped by a devkit.
- `X64ECHO_NATIVE_FLAGS` and `X64ECHO_FACE_FLAGS`: additional compiler search
  paths for the native and face halves.

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

Build the pinned Qt 6.8.4 testkit and run the suite:

```bash
./tests/qt/bootstrap.sh
./tests/qt/run.sh
```

The build is local to `.cache/qt/6.8.4` and does not require a prebuilt Qt
bundle. See [../tests/qt/README.md](../tests/qt/README.md).
