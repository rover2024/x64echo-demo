# Reproduction Notes

The repository was validated on an AArch64 NVIDIA DGX Spark host using the
published-style AArch64 devkit. Qt 6.8.4 source was fetched at the pinned
commit and both Qt halves were built locally by `tests/qt/bootstrap.sh`.

The four QtCore tests used by `tests/qt/run.sh` produced:

| Test | Result |
| --- | --- |
| `tst_qbytearray` | 414 passed, 0 failed, 1 skipped |
| `tst_qstringlist` | 54 passed, 0 failed |
| `tst_qbitarray` | 172 passed, 0 failed |
| `tst_qdatetime` | 671 passed, 0 failed, 8 skipped |

Total: 1,311 passed, 0 failed, 9 skipped.

The quick C++ correctness suite and Hello C++ example also pass. A local
100,000-call smoke benchmark measured approximately 4.93x over QEMU full
emulation; this single-run number is illustrative rather than a paper
replacement.

These commands are reproducible from the public repository checkout and do not
require prebuilt Qt binaries.
