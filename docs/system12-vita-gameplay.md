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
Private host LwCond snapshots and long-delay probes are also streamed to the
runner's standard output before the guest finishes, bounded to 8,192 lines of
1,024 characters per process. Reaching this live-output bound does not suppress
the normal capture's marker checks or independently bounded retained log.
Native screenshots and any source report/PPM are copied out even after a guest
failure or interruption. `capture-evidence-<frames>.json` records whether the
guest completed and retains diagnostics separately from successful results.

Add `--gameplay-replay` to run the same input timeline as the MAME/Mac
comparison. The default remains 12,000 frames and 177 CSV events. To shorten a
focused capture, select a supported replay endpoint:

```sh
python3 tools/vita-harness/check_system12_gameplay_vita3k.py \
  --gameplay-replay --replay-frames 3600 \
  --emulator ... --vpk ... --firmware-fs ... --output-dir ...
```

`--replay-frames` accepts 3,600, 6,000, or 12,000 and is valid only with
`--gameplay-replay`; `--frames` remains incompatible with replay. Shorter CSVs
use the same event timeline through the requested endpoint and retain a final
`p1button3` event so the terminal input check remains meaningful. The runner
derives the expected event count and completion frame from the selected
timeline. Replay mode defaults to a 1,200-second per-process deadline and keeps
the native screenshot schedule through 600 seconds. The Vita frontend reads
`--inputs` only for a finite System 12 game run, checks bounded ordered
`frame,control,pressed` rows before initialization, and applies them at the
libretro input poll. It rejects embedded NUL, overlong rows, unknown controls
and out-of-range events. Hardware still supplies the exit chord. Completion
requires all generated events consumed, the final joypad state, and JAMMA
`ff/bf/ff/ff` in the board report; those input checks alone do not establish a
playable match. The runner saves the CSV, input JSON, exact VPK/host hashes and
`smoke-evidence.json`. `--reuse-console-smoke PATH` accepts a prior PASS only
when both hashes match, otherwise the console probe runs again.

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

Disabling the P4 texture cache in a private diagnostic VPK
`bca5af21453e5703db0ddd7b0c7d94d840ad1664d2ef9eff2268a0d16fe66833`
does not remove the horizontal bands. Both cached and direct-sampling
3600-frame attract runs complete, followed by the same 12000-frame input replay
for the direct-sampling VPK. The latter is retained in
`/tmp/pcsx-vita-system12-vitagl-p4-direct-replay-20261005`: 22 native PNGs,
9642 paired recoveries, clean shutdown, and the same DMA5 bytes (5909108) and
H8 cycles (3396679944) as the cached replay. Graphics review still fails.
`p4-direct-source.patch` and `p4-direct-replay-evidence.json` beside the frozen
VPK record this experiment; the bypass is not applied to repository code.

`VITA_CTR_GPU_TRACE=1` also records up to 256 distinct begin/store geometry
states (`pcsx-vita: frame-geometry`): draw clip, E5 offset, display rect,
physical/logical target size, initial clip and final pack rect. It changes no
rendering state. The first 64-record diagnostic VPK
`94cdf2bb2f071233f8f98b4715b9de294496ab1f58399b2242d0c4a1eb5ed0df`
was run with the 3600-frame input replay on the stale-fault host. Its trace
shows scene 2 begin with a 1x1 target, then store that same target into a
640x220 draw area. Later scenes alternate draw pages at y=10/250 while the
display window points at the opposite page. The target uses draw-clip extents,
but onscreen projection/scissor currently use display extents/origin; ordinary
onscreen clip changes do not rebind the target. These are coordinate/lifecycle
inconsistencies to investigate, not a validated graphics fix.

The run retains 12 native PNGs and the board report in
`/tmp/pcsx-vita-system12-vitagl-geometry-20261005`. Graphics review still fails.
Its guest consumed 33/33 inputs, reached 3600/3600 frames, shutdown PASS and
Idle, but the runner did not recognize completion after its 128-message ring
lost early expected tokens. A native sample showed the host in Qt idle;
only the owned process group was terminated. `geometry-evidence.json` beside
the frozen VPK keeps this run failed at runner completion detection. Do not
reclassify it as a passing validation; rerun with corrected token retention.

The runner now registers the exact completion tokens before starting its output
pump and retains only their seen flags independently of the recent-message
ring. Idle, paired fault recovery and late error checks remain required. The
fake-host regression emits 256 diagnostic messages after early tokens and a
forwarded/recovered fault pair; gameplay evidence tests also reject empty or
mismatched token sets. Both suites pass. The separate console renderer's regex
smoke path is unchanged; installation registers its literal completion token.

A fresh private experiment makes both projection and onscreen scissor use the
clip-local target (`44e053b939deb668c0bc5222fe095625b5fd19e12d751808e7270b008a193cfa`).
The corrected runner completes the 3600-frame/33-input replay, pairs all 2652
forwarded faults with recoveries, observes shutdown PASS/Idle, and closes its
host automatically. Ten native PNGs are retained in
`/tmp/pcsx-vita-system12-vitagl-cliplocal-20261005`. The title's bottom strip is
removed, but Stage 1 Voldo/Astaroth still contains broad horizontal bands;
**graphics review fails**. `clip-local-source.patch`, `clip-local-evidence.json`
and the output `graphics-review.json` preserve that distinction. This change
is not applied to repository rendering code. The trace still shows target/load
geometry changing independently of the final store rect; fixing only projection
and scissor is insufficient. Main-target segmentation and independent display
scanout remain open.
