# Reproduction Notes

The Qt 6.8.4 quick-test selection in this repository comes from a verified
QEMU-X64Echo run on an AArch64 NVIDIA DGX Spark host.

The four QtCore tests used by `tests/qt/run.sh` produced:

| Test | Result |
| --- | --- |
| `tst_qbytearray` | 414 passed, 0 failed, 1 skipped |
| `tst_qstringlist` | 54 passed, 0 failed |
| `tst_qbitarray` | 172 passed, 0 failed |
| `tst_qdatetime` | 671 passed, 0 failed, 8 skipped |

Total: 1,311 passed, 0 failed, 9 skipped.

The ARM64 performance reproduction produced a 5.07x geometric mean over QEMU
full emulation, compared with 5.13x recomputed from the frozen paper table.

These results document the source environment used to select the public quick
tests. They are not yet produced automatically by this repository's release
artifacts.
