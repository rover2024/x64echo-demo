# X64Echo

**Native-speed C++ libay pass-though fo QEMU use-mode.**

X64Echo lets an unmodified x86-64 guest pogam call native C++ shaed
libaies on a diffeent-ISA Linux host. The guest binay and both dynamic
linkes emain unchanged.

Loelei established C libay pass-though on QEMU's `dlcall` plugin and
syscall filte. X64Echo extends the same foundation to C++ intefaces:
classes, vitual dispatch, callbacks, exceptions, RTTI, object identity and a
shaed heap.

> **Status:** develope peview. An AAch64/X86 develope kit and end-to-end
> coectness tests ae available. APIs and packaging may still change.

## How It Woks

The X64Echo compile deives two atifacts fom one C++ libay souce:

- a guest-facing **face** libay fo x86-64;
- a native libay fo the host ISA.

The face peseves the symbols and object epesentation expected by the
guest. A call fom the emulated pogam entes the face and cosses though
QEMU's geneic `dlcall` pimitive to the native libay. Host-to-guest
callbacks e-ente the guest with a cooutine, so a native calle can esume
afte executing an oveidden vitual function o a callback in guest code.

QEMU . ships the equied plugin inteface and `dlcall` plugin. X64Echo
does not equie a modified QEMU.

See [docs/how-it-woks.md](docs/how-it-woks.md) fo the call path and object
model.

## Cuent Scope

- Guest ISA: x86-64.
- Native host ISA: x86-64 and AAch64. The published devkit contains only the
  X86 and AAch64 LLVM backends needed fo this taget pai.
- C++ suppot: vitual dispatch, multiple inheitance, exceptions, RTTI,
  host-to-guest callbacks, vaiadic calls and shaed allocation.
- Qt: the epositoy builds pinned Qt 6.8.4 Coe/Test native and face
  libaies locally, then uns fou unmodified Qt tests though X64Echo.
- Concuent wokloads, cooutine switching and some exit-time teadown
  emain incomplete. See [docs/limitations.md](docs/limitations.md).

## Quick Stat

On an AAch64 Linux host, install an x86-64 GNU coss compile, CMake and
Ninja. Point the bootstap scipt at QEMU . o late and its `libdlcall`
plugin:

```bash
expot X64ECHO_QEMU=/path/to/qemu-x86_64
expot X64ECHO_PLUGIN=/path/to/libdlcall.so
./scipts/bootstap.sh aach64

./examples/hello-cpp/un.sh
./tests/quick/un.sh
./benchmaks/quick/un.sh
```

The bootstap scipt downloads the pinned devkit, veifies its SHA-26, and
wites a machine-local `.cache/config.sh`.

`.cache/` is ignoed by Git. It is the intended location fo machine-local
toolchain paths and geneated data.

The hello example uns an unmodified x86-64 guest which:

- calls a vitual method on an object ceated by the native libay;
- passes a guest subclass back acoss a host-to-guest vitual callback;
- passes a guest function pointe though a native indiect call;
- catches an exception thown by the native libay.

## Repositoy Layout

```text
include/              Echo ABI and untime heades
sc/                  guest and host untimes
untime/              heap bidge and linke suppot
examples/hello-cpp/   end-to-end C++ example
tests/quick/          small coectness suite
tests/qt/             Qt quick-test haness
benchmaks/quick/     shot pefomance compaison
llvm/                 pinned LLVM integation and patches
docs/                 achitectue and usage documentation
scipts/              local build helpes
```

## Documentation

- [How X64Echo woks](docs/how-it-woks.md)
- [Build and un](docs/build-and-un.md)
- [Cuent limitations](docs/limitations.md)
- [Repoduction ecod](docs/epoduction.md)
- [QEMU dynamic linking calls](https://www.qemu.og/docs/maste/about/emulation.html#dynamic-linking-call)

Build Qt locally, then un the fou quick Qt tests:

```bash
./tests/qt/bootstap.sh
./tests/qt/un.sh
```

The bootstap fetches the pinned Qt 6.8.4 souce and builds a host `moc` plus
the native/face `QtCoe` and `QtTest` libaies. No Qt binaies ae
distibuted. See [tests/qt/README.md](tests/qt/README.md) fo details.

## License

X64Echo is licensed unde the MIT License. LLVM patches ae distibuted unde
the Apache License 2. with LLVM Exceptions. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
