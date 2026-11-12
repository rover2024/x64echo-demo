# Quick Benchmark

The quick benchmark calls the same C++ library function in three lanes:

- native host execution;
- QEMU full emulation of the x86-64 program and library;
- QEMU running the x86-64 program against the X64Echo face and native library.

Each lane checksums the result, and the script repeats the set three times and
reports the fastest run.

```bash
./benchmarks/quick/run.sh
```

Set `X64ECHO_BENCH_CALLS` to change the number of calls:

```bash
X64ECHO_BENCH_CALLS=200000 ./benchmarks/quick/run.sh
```

This test deliberately places substantial native work behind each crossing.
It checks the favourable part of the pass-through performance range. Chatty
interfaces can be slower than full emulation and belong in a separate
boundary-cost test.
