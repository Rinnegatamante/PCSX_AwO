# System 12 gameplay run on Vita3K

`tools/vita-harness/check_system12_gameplay_vita3k.py` runs the locally prepared
Soul Calibur COH-700 profile in the ARMv7/ari64 NEON VPK. It first validates the
profile against its pinned MAME manifest and region hashes, then installs a
console smoke build and the game into a fresh copied Vita3K app with a separate
portable filesystem. Local Vita firmware is copied into that temporary
filesystem; no PlayStation BIOS or game media is downloaded.

Example:

```sh
python3 tools/vita-harness/check_system12_gameplay_vita3k.py \
  --emulator build/vita-validation-neon-fix/emulator/Vita3K-fixed.app \
  --vpk build/vita-system12/src/pcsx-rearmed-vita-neon.vpk \
  --firmware-fs build/vita-validation-repro/emulator/portable/fs \
  --output-dir /tmp/system12-vita-soulcalibur-run
```

The output directory is created by the runner and must not already exist. It
contains the Vita3K log, the source profile manifest, report/PPM pairs, and
native Vita3K PNG screenshots at 400, 800, and 1,800 frames. Screenshots are
scheduled at several elapsed wall-clock times and copied out of the temporary
portable filesystem before cleanup. The runner fails if the host cannot emit a
native PNG for a milestone; its libretro PPM is recorded separately and never
substitutes for that capture. Each report records the active board identity,
main-CPU PC/opcode, H8 counters and fault state, C352 sample progress, DMA5,
shared-memory traffic, and unmapped accesses. Each PPM is captured from the
actual RGB565 libretro video callback before Vita display scaling. The runner
requires the report to identify the expected COH-700 profile, show increasing
H8 cycles and active C352 time, and report zero H8 unsupported instructions
and zero unmapped accesses.

The copied Vita3K process runs in its own process group. A separate watchdog
process terminates that group at the per-run timeout even if the Python runner
stops making progress. Logs are streamed with bounded retained output, and ARM
instruction fallback/unpredictable-instruction diagnostics invalidate a run.

The first step is a console ari64 GPU smoke test, which checks the existing
triangle, background, and guest-memory signature before the System 12 runs.
`results.json` records these checks and labels board execution as unverified.
Frame count, successful reports, active devices, and screenshots establish
observable execution progress; they do not certify full game compatibility or
accurate gameplay. In the current capture series, the 400-frame PPM was black,
the 800-frame run's native image showed the first-power-on screen, and the
1,800-frame run's timed native captures reached the Soul Calibur title. Its
final source PPM shows Sophitia in the attract sequence with the earlier
VADDHN-related texture stripes gone. These wall-clock screenshots precede the
final frame count. No playable match has been verified. Inspect the captures and compare
game state with the separate MAME/native replay evidence before making claims
about gameplay. The runner leaves ROM regions and Vita firmware out of the repository.

The successful local run is recorded in
`/tmp/pcsx-vita-system12-neon-capture-20261005/results.json`. It uses the
VADDHN and CPU-framebuffer screenshot patches documented in
[vita-harness.md](vita-harness.md), with the built host in
`build/vita-validation-neon-capture`. The console probe and all three runs
completed with zero host instruction/crash diagnostics and zero H8 illegal,
unsupported, or unmapped accesses. H8 cycles reached 509,466,008, C352 produced
2,653,468 sample frames, and DMA5 transferred 1,585,008 bytes. Seven runner
tests pass. These are execution and capture checks, rather than physical Vita
or full gameplay validation.
