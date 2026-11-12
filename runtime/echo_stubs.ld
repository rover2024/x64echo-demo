/* x64echo D-2: page-isolate the self-identifying stub section, so the bootstrap's
 * mprotect(R+X) on it does NOT clobber adjacent writable data (.got.plt / .data / .bss
 * share its page otherwise). See 02-implementation (地基 D §3).
 *
 * The alignment is on the OUTPUT SECTION -- ALIGN() on the header aligns the START, the
 * internal `. = ALIGN()` pads the END -- so echo_stubs occupies whole pages and the
 * following .bss starts on a fresh one. This form survives `-z relro` / `--version-script`
 * (Qt's link flags); the earlier bare `. = ALIGN()` statements around a plain
 * `echo_stubs : {}` were silently dropped there, leaving the section unaligned so its last
 * page overlapped .bss -- e.g. a TU's `completed.0` guard became read-only and the exit-time
 * write in __do_global_dtors_aux SIGSEGV'd at process teardown.
 *
 * Used via `ld -T this` — because it contains INSERT, ld appends it to the default linker
 * script rather than replacing it. */
SECTIONS
{
  echo_stubs : ALIGN(CONSTANT(MAXPAGESIZE)) {
    KEEP(*(echo_stubs))
    . = ALIGN(CONSTANT(MAXPAGESIZE));
  }
} INSERT AFTER .data;
