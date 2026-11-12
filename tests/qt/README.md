# Qt Quick Tests

This harness compiles four unmodified Qt 6.8.4 Core tests against the
X64Echo face libraries and runs them through QEMU:

- `tst_qbytearray`
- `tst_qstringlist`
- `tst_qbitarray`
- `tst_qdatetime`

The test binaries, both Qt halves, the x86-64 sysroot and the Echo compiler
all use the same pinned Qt source and ABI configuration.

## Build

After `scripts/bootstrap.sh` has configured X64Echo:

```bash
./tests/qt/bootstrap.sh
```

This fetches Qt `v6.8.4-lts-lgpl` at commit
`ed77a3ca9ef1bf5e33b6f32ea41110734fb14e88`, builds a native host toolset,
then builds the AArch64 native and x86-64 face `QtCore`/`QtTest` libraries.
The resulting configuration is written under `.cache/qt/6.8.4/`.

The build requires CMake, Ninja and a native C++ compiler. Allow several
minutes and roughly 4 GiB of free disk space.

## Run

```bash
./tests/qt/run.sh
```

The runner loads the generated Qt configuration automatically. The selected
suite is intentionally small enough for a presentation demo; it is not a
replacement for the full Qt test matrix.
