# X64Echo

**Native-speed C++ library pass-through for QEMU user-mode.**

X64Echo lets an unmodified x86-64 guest program call native C++ shared
libraries on a different-ISA Linux host. The guest binary and both dynamic
linkers remain unchanged.

Lorelei established C library pass-through on QEMU's `dlcall` plugin and
syscall filter. X64Echo extends the same foundation to C++ interfaces:
classes, virtual dispatch, callbacks, exceptions, RTTI, object identity and a
shared heap.

> **Status:** developer preview. An AArch64/X86 developer kit and end-to-end
> correctness tests are available. APIs and packaging may still change.

## How It Works

The X64Echo compiler derives two artifacts from one C++ library source:

- a guest-facing **face** library for x86-64;
- a native library for the host ISA.

The face preserves the symbols and object representation expected by the
guest. A call from the emulated program enters the face and crosses through
QEMU's generic `dlcall` primitive to the native library. Host-to-guest
callbacks re-enter the guest with a coroutine, so a native caller can resume
after executing an overridden virtual function or a callback in guest code.

QEMU 11.1 ships the required plugin interface and `dlcall` plugin. X64Echo
does not require a modified QEMU.

See [docs/how-it-works.md](docs/how-it-works.md) for the call path and object
model.

## Current Scope

- Guest ISA: x86-64.
- Native host ISA: x86-64 and AArch64. The published devkit contains only the
  X86 and AArch64 LLVM backends needed for this target pair.
- C++ support: virtual dispatch, multiple inheritance, exceptions, RTTI,
  host-to-guest callbacks, variadic calls and shared allocation.
- Qt: the repository builds pinned Qt 6.8.4 Core/Test native and face
  libraries locally, then runs four unmodified Qt tests through X64Echo.
- Concurrent workloads, coroutine switching and some exit-time teardown
  remain incomplete. See [docs/limitations.md](docs/limitations.md).

## Quick Start

On an AArch64 Linux host, install an x86-64 GNU cross compiler, CMake and
Ninja. Point the bootstrap script at QEMU 11.1 or later and its `libdlcall`
plugin:

```bash
export X64ECHO_QEMU=/path/to/qemu-x86_64
export X64ECHO_PLUGIN=/path/to/libdlcall.so
./scripts/bootstrap.sh aarch64

./examples/hello-cpp/run.sh
./tests/quick/run.sh
./benchmarks/quick/run.sh
```

The bootstrap script downloads the pinned devkit, verifies its SHA-256, and
writes a machine-local `.cache/config.sh`.

`.cache/` is ignored by Git. It is the intended location for machine-local
toolchain paths and generated data.

The hello example runs an unmodified x86-64 guest which:

- calls a virtual method on an object created by the native library;
- passes a guest subclass back across a host-to-guest virtual callback;
- passes a guest function pointer through a native indirect call;
- catches an exception thrown by the native library.

## Repository Layout

```text
include/              Echo ABI and runtime headers
src/                  guest and host runtimes
runtime/              heap bridge and linker support
examples/hello-cpp/   end-to-end C++ example
tests/quick/          small correctness suite
tests/qt/             Qt quick-test harness
benchmarks/quick/     short performance comparison
llvm/                 pinned LLVM integration and patches
docs/                 architecture and usage documentation
scripts/              local build helpers
```

## Documentation

- [How X64Echo works](docs/how-it-works.md)
- [Build and run](docs/build-and-run.md)
- [Current limitations](docs/limitations.md)
- [Reproduction record](docs/reproduction.md)
- [QEMU dynamic linking calls](https://www.qemu.org/docs/master/about/emulation.html#dynamic-linking-call)

Build Qt locally, then run the four quick Qt tests:

```bash
./tests/qt/bootstrap.sh
./tests/qt/run.sh
```

The bootstrap fetches the pinned Qt 6.8.4 source and builds a host `moc` plus
the native/face `QtCore` and `QtTest` libraries. No Qt binaries are
distributed. See [tests/qt/README.md](tests/qt/README.md) for details.

## License

X64Echo is licensed under the MIT License. LLVM patches are distributed under
the Apache License 2.0 with LLVM Exceptions. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
