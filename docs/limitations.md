# Current Limitations

X64Echo is a research system under active cleanup. Do not treat this snapshot
as a production compatibility layer.

Known limitations include:

- Concurrent callbacks and some coroutine-switch scenarios remain incomplete.
- Exit-time teardown can cross the boundary in unsupported ways.
- Some non-standard exception types lose fidelity when no cross-boundary type
  descriptor is available.
- The public quick-test path currently covers a same-architecture developer
  configuration. Cross-architecture devkit packaging is still being built.
- Qt support is version-specific. The current validated version is Qt 6.8.4.
- Static libraries linked into the guest application are still emulated.
- Performance depends on work per call. Chatty interfaces may be slower than
  full emulation.

The repository must not contain hostnames, private addresses, credentials or
machine-specific absolute paths. Local values belong in the ignored
`.cache/config.sh` file.
