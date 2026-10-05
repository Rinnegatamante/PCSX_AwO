# System 12 gameplay run on Vita3K

`tools/vita-harness/check_system12_gameplay_vita3k.py` runs the locally prepared
Soul Calibur COH-700 profile in an ARMv7/ari64 NEON or VitaGL VPK. It first validates the
profile against its pinned MAME manifest and region hashes, then installs a
console smoke build and the game into a fresh copied Vita3K app with a separate
portable filesystem. Local Vita firmware is copied into that temporary
filesystem; no PlayStation BIOS or game media is downloaded.

Example:

```sh
python3 tools/vita-harness/check_system12_gameplay_vita3k.py \
  --emulator build/vita-validation-neon-capture/emulator/Vita3K-fixed.app \
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
Native screenshots and any source report/PPM are copied out even after a guest
failure or interruption. `capture-evidence-<frames>.json` records whether the
guest completed and retains diagnostics separately from successful results.

Add `--gameplay-replay` to run 12,000 frames with the same 177 CSV input events
as the MAME/Mac comparison. This mode defaults to a 1,200-second per-process
deadline and schedules native screenshots through 600 seconds. The Vita
frontend reads `--inputs` only for a finite System 12 game run, checks bounded
ordered `frame,control,pressed` rows before initialization, and applies them at
the libretro input poll. It rejects embedded NUL, overlong rows, unknown
controls and out-of-range events. Hardware still supplies the exit chord.
Completion requires all 177 events consumed, the final joypad state, and
JAMMA `ff/bf/ff/ff` in the board report; those input checks alone do not establish
a playable match. The runner saves the CSV, input JSON, exact VPK/host hashes
and `smoke-evidence.json`. `--reuse-console-smoke PATH` accepts a prior PASS
only when both hashes match, otherwise the console probe runs again.

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
final frame count. The replay below now demonstrates match progress. Compare
game state with the separate MAME/native replay evidence before making claims
about gameplay. The runner leaves ROM regions and Vita firmware out of the repository.

The successful local run is recorded in
`/tmp/pcsx-vita-system12-neon-capture-20261005/results.json`. It uses the
VADDHN and CPU-framebuffer screenshot patches documented in
[vita-harness.md](vita-harness.md), with the built host in
`build/vita-validation-neon-capture`. The console probe and all three runs
completed with zero host instruction/crash diagnostics and zero H8 illegal,
unsupported, or unmapped accesses. H8 cycles reached 509,466,008, C352 produced
2,653,468 sample frames, and DMA5 transferred 1,585,008 bytes. Twelve runner
tests pass. These are execution and capture checks, rather than physical Vita
or full gameplay validation.

## Observed 12,000-frame replay

The local replay in `/tmp/pcsx-vita-system12-replay-20261005/results.json`
completed with ari64 and NEON, consumed all 177 events, produced the expected
terminal joypad `0002/0000` and JAMMA `ff/bf/ff/ff`, then reached
`Stopping -> Idle`. The runner exited zero and left no Vita3K process running.
Its VPK SHA-256 is
`55258ebe8a94aa7b9a3c3d73e1340379d6103ecb06d5c66ef3f91ca3e1e8aa81`;
the host SHA-256 is
`64146926b9a820ff89217bd31d165601dc9d87de67ebfa7511934693a46ec8a9`.

Visual inspection of the 23 native PNGs shows character selection, a Stage 1
Voldo/Astaroth fight with movement and damage, and progression to Stage 2
Voldo/Sophitia. The final RGB565 source capture is 512×240 and shows Stage 2
with the fight timer at 10. H8 executed 598,527,313 instructions over
3,396,681,168 cycles, C352 produced 17,691,047 frames, and DMA5 transferred
6,309,696 bytes without truncation. H8 illegal/unsupported instructions,
unmapped accesses, and host ARM fallback/crash diagnostics were all zero.

This is a verified local replay with observed match progression, not a complete
System 12 compatibility result. The same CSV selected Ivy in the native Mac
DRC run and Voldo in this ARMv7 run, so timing/game-state equivalence remains
unverified. `board_execution_verified` remains false in the machine-readable
result, and this run does not establish physical Vita performance or VitaGL
support.

## VitaGL title capture and shutdown validation

Use `--renderer vitagl --shader-compiler PATH` with an existing local
`libshacccg.suprx`. VitaGL presents through GXM rather than the libretro video
callback, so this mode requires native Vita3K PNGs and does not request a PPM.
The runner additionally requires `vitaGL shutdown PASS (0)` and `Stopping ->
Idle`. `--frames 1800` selects only that standard milestone. The additional
`--frames 3600` diagnostic captures beyond the title with the longer screenshot
schedule, without injecting replay controls; neither selector can be
combined with `--gameplay-replay`. Cumulative results include `status`,
completed milestones, and the first failed milestone with its diagnostics.

The production type-2 package `25a10718420c436022721ed3699dccdac233b0703558e258e77188605b11df48`
completed the 400- and 800-frame runs in
`/tmp/pcsx-vita-system12-vitagl-capture-20261005`. The native 800-frame captures
show FIRST POWER ON, and the 1800-frame captures show the Soul Calibur title.
Its H8/C352/DMA report completed without unsupported instructions or unmapped
board accesses, but the host then failed during shutdown with one unresolved
forwarded access. The aggregate has been corrected from the original runner's
partial success record to an explicit failure; the original capture evidence
and log are unchanged. This is title rendering evidence, not a passing run.

The bounded follow-up in
`/tmp/pcsx-vita-system12-vitagl-teardown-20261005` uses diagnostic VPK
`f4513a495455a29ec1a5c70694d2a2eb8038c515f72cc584f8136003cd88fef9`.
The plugin waits for GPU work before deleting its resources. A separate local
VitaGL finish patch ends any open scene and waits unconditionally, including
work already submitted by a swap. All renderer deletion stages complete; the
same unresolved host access occurs after entering `vglShutdown()`. Nine native
PNGs and the report are retained, and `results.json` records the failure.
The native title PNG has the same SHA-256 as the first run:
`0f5c0e4fc8d77d2caebfdbf5e652cf9222d0cef063d8d8e19e199ef35e7c4951`.
The diagnostic source delta, library flags/patches, and hashes are retained in
`build/vita-system12-vitagl-teardown/evidence.json`.

The shutdown trace identified a borrowed scratch-pool slice passed to
`mspace_free`: `vglBufferData` changes a VBO's pointer, while `glDeleteBuffers`
treats that pointer as owned storage with the required build flags. The plugin
now detaches the borrowed batch pointers before deleting the VBOs; the static
quad VBO retains its normal allocation ownership.

The isolated 1800-frame run in
`/tmp/pcsx-vita-system12-vitagl-detach-only-20261005` passes with diagnostic VPK
`9cf45006079c0cab2fda2b8327aed28ab0a699a2f80bd468edcea9005922de2e`.
Its nine native PNGs include the same title image as above, with a narrow
striped artifact at the lower edge. H8 unsupported instructions, unmapped board
accesses and DMA truncations are zero. All 815 forwarded faults have matching
recoveries; every library shutdown phase completes, `vitaGL shutdown PASS (0)`
and `Stopping -> Idle` are present. Exact source/library hashes and the frozen
VPK are recorded by `build/vita-system12-vitagl-teardown/detach-only-evidence.json`.
This validates title rendering and resource teardown, not a 3D match or physical
Vita performance.

The original initial VBO allocation remains unchanged in this run. Removing
it in a separate experiment exposed a critical host protection diagnostic
during gameplay before shutdown (`detach-evidence.json`); that experiment is
recorded as failed and is not part of this fix. Its cause remains under review.

The subsequent production replay (`VITA_CTR_GPU_TRACE=0`, default four library
patches) uses VPK `d6ef31efb5f4a5d3193c0f3eb77045ae6f24e9b876623098346e6612e83d300e`.
Its short console probe passes, but gameplay stops before the 12000-frame report
on the same valid-address critical host protection diagnostic at `60800050`.
The original VBO initialization is present, so the earlier null-initialization
experiment alone does not explain this error. The failed replay is retained in
`/tmp/pcsx-vita-system12-vitagl-production-replay-20261005` and
`build/vita-system12-vitagl-teardown/production-evidence.json`; the successful
1800-frame diagnostic run is not a production gameplay validation.

A repeat with the optional protection-miss trace host
`3bee5d88d3c1fa5c43b348fb980f0d12125a47b7f6abbd58c3e707235bc460d7`
completes all 12000 frames and 177 input rows, with 9642 paired fault recoveries,
zero critical protection misses, successful shutdown and Idle. Evidence is in
`/tmp/pcsx-vita-system12-vitagl-miss-trace-20261005` (23 native PNGs). This is a
passing execution check, but **graphics review fails**: character selection
and the Voldo/Astaroth Stage 1 fight contain broad horizontal corrupted bands.
Later GAME OVER and Sophitia intro captures show substantially cleaner textures.
The runner's `status=passed` checks completion/report/capture availability, not
pixel correctness; `graphics-review.json` records the manual failure separately.
The passing replay does not invalidate the earlier intermittent host critical.
The frozen production VPK/source delta and both host results are bound in
`build/vita-system12-vitagl-teardown/production-trace-evidence.json`.
