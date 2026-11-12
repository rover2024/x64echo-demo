# Qt Quick Tests

This harness compiles four unmodified Qt 6.8.4 Core tests against the
X64Echo face libraries and runs them through QEMU:

- `tst_qbytearray`
- `tst_qstringlist`
- `tst_qbitarray`
- `tst_qdatetime`

The test binaries, both Qt halves, the x86-64 sysroot and the Echo compiler
must all use the same Qt and ABI configuration.

Required environment:

- `X64ECHO_QT_SOURCE`: Qt 6.8.4 source tree;
- `X64ECHO_QT_FACE_DIR`: face `libQt6Core/QtTest.so.6.8.4`;
- `X64ECHO_QT_NATIVE_DIR`: native `libQt6Core/QtTest.so.6.8.4`;
- `X64ECHO_QT_INCLUDE`: matching host Qt include directory;
- `X64ECHO_QT_MOC`: matching `moc`;
- the normal X64Echo variables from `config.example.sh`.

Run:

```bash
./tests/qt/run.sh
```

The selected suite is intentionally small enough for a presentation demo. The
full Qt suite and distributable Qt testkit will be added later.
