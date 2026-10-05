# Vita3K host-page protection regression

`patches/vita3k-host-page-protection.patch` aligns `protect_tree` segments to the effective 16 KiB host protection range when `MemState` uses the non-page-table memory layout. Callback blocks retain their 4 KiB guest-page keys, so distinct watchers that share one host page remain distinct. Segment merging uses the effective range, rather than the unrounded request length. Page-table and external-mapping behavior is left unchanged.

The C++ regression runs Vita3K's real `init(mem, false)`, fixed allocation, `add_protect`, and native protected-memory accesses. An un-watched 4 KiB neighbor must conservatively fire both watchers sharing its 16 KiB host page, after which each watcher must fire again when rearmed. A second case checks watchers on opposite sides of a host-page boundary. Run it with:

```sh
python3 tools/vita-harness/test_host_page_protection.py \
  --source ../vita3k --build ../vita3k/build/macos --sanitize
```

This only addresses protection bookkeeping for the non-page-table allocation. It does not claim to fix guest aliases, shared backing mappings, or page-table/external-mapping protection behavior. The harness applies the patch to a private temporary source copy and replaces `mem.cpp.o` in a private archive; it does not modify the Vita3K checkout or its build.

Add `--fix-host-page-protection` to `rebuild_vita3k_fixed.py` to apply the same patch to an isolated host. The recipe recompiles `mem.cpp`, reconstructs a private `libmem.a`, redirects every occurrence of that archive in the original link command, and records the patch and executable hashes. It preserves the original checkout and compiled objects.

The 2026-10-05 run with `memory-mapping: disabled` used host SHA-256 `7bba1fb6cdb3b93832257cc2bed6cbd2eef532f9aa688ec4efb048959c42e5ca` and the final3 VitaGL VPK `2102ea666b6fcd13fc771503b287671619cbda1737a14ff3b1f09b7c9732dfd8`. GPU/ari64 initialization, VitaGL shutdown and `Stopping -> Idle` were observed, with no critical protection errors. Eleven Dynarmic `Unhandled EXC_BAD_ACCESS` diagnostics remain and the strict lifecycle runner still reports FAIL. The separate bounded pixel diagnostic also reports FAIL: triangle `2082`, background `2082`, sentinel `534d4f4b`, 180 frames. No graphics validation is claimed.

Logs: `/tmp/pcsx-vitagl-host-pages-lifecycle-20261005.log` and `/tmp/pcsx-vitagl-host-pages-pixels-20261005.log`. Both owned host processes were closed by their watchdogs. The regression passes normally and with ASan/UBSan; `--baseline` verifies the expected failure without the patch.

For a native recovery-trace check, pass `--trace-recovery`. This additionally applies `patches/vita3k-macos-recovery-trace.patch` to the private `mem.cpp` copy; successful Apple ARM64 SIGSEGV/SIGBUS recovery writes a stack-formatted line using only `write(2)`:

```text
Vita3K recovered protected access pc=0x... addr=0x... write=0|1
```

The independent Dynarmic message patch is `patches/dynarmic-forwarded-exceptions.patch`; it labels only PCs absent from Dynarmic's JIT registry and leaves the `KERN_FAILURE` forwarding behavior unchanged. A runner can pair those events with recovery records by PC and fail on any unmatched forwarding event. `KERN_FAILURE` alone is not proof of recovery.

`rebuild_vita3k_fixed.py --trace-protection-recovery --dynarmic-archive PATH`
rebuilds both diagnostic TUs privately. The tested Dynarmic input is copied,
its one macOS exception-handler member is replaced, and its linked hash is
recorded separately from the input archive hash. Combine this with
`--fix-host-page-protection`; the recovery trace does not fix protection bugs.

The strict lifecycle run then passed with host SHA-256
`d6aaf20c4c7ee916bea5152c087b22e66546de92fac407c5d196316100dbf3fb`:
11 forwarded events matched 11 recovery records, zero unresolved events,
zero critical diagnostics, `cpu_driver=ari64`, shutdown PASS and Idle.
The runner keeps every forwarding/recovery record and counts repeated PCs
individually. A missing or mismatched recovery, critical fallback, abnormal
host exit or timeout still fails. The 4/16 MiB System12 ARMv7 DRC fixtures also
passed in this host. Logs: `/tmp/pcsx-vitagl-recovery-lifecycle-20261005.log`
and `/tmp/pcsx-system12-drc-recovery-trace-20261005.log`.

Native screenshots from the preceding bounded 180-frame diagnostic are black,
including `build/vita-validation-host-pages2/emulator/portable/screenshots/PCSX-ReARMed Vita vitagl/PCSX-ReARMed Vita vitagl_2026-10-05-023222.png`.
The successful lifecycle test establishes resource startup/shutdown; graphics
remain unvalidated.

For valid-address faults whose protection interval is missing, the optional
`rebuild_vita3k_fixed.py --trace-protection-miss` patch prints at most eight
`Vita3K protection miss` lines under the existing protection mutex. Each line
includes the fault address, memory base, guest address, access type, mapping
mode, host-page size, allocation validity, interval count, and neighboring
interval bounds/sizes in descending map order. It changes no recovery behavior
and retains the existing critical error. The gameplay runner keeps these lines
in its bounded log. The patch applies with or without the host-page and
recovery-trace patches; the isolated macOS host compilation passes.
