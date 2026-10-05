# System12 performance and clock diagnostics

`SYSTEM12_PROFILE=1` enables diagnostic scopes and emulated-cycle counters. It
does not change CPU multipliers, device clocks, instruction execution, or board
scheduling. The profiler supports the synchronous System12 core; the libretro
makefile rejects configurations with async GPU or SPU enabled.

For the Vita frontend:

```sh
make -f Makefile.vita VITA_GPU=neon ARCADE_SYSTEM12=1 SYSTEM12_PROFILE=1
```

The default VPK, core archive and standalone object directory gain a profile
suffix. Keep a separate source checkout/copy for performance experiments: core
objects in the generic makefile still share paths, and `Makefile.vita` forces a
complete core rebuild. Preserve validated VPKs and their evidence before building.

Run the guarded gameplay checker with that VPK and the same private Vita3K host,
firmware, profile and replay length used by the control. Do not run another guest,
native harness or compiler during either timed replay. The checker bounds guest
lifetime, preserves native screenshots and waits for shutdown.

## Measurements

The core emits cumulative `PERF` snapshots every 600 `retro_run()` calls. The
reported frequencies for the source-timing Soul Calibur profile are:

| Domain | Frequency / unit |
| --- | --- |
| Main CPU cycle counter | 50,000,000 cycles/s; source CPU input clock 100 MHz |
| H8/3002 | 16,934,400 cycles/s |
| C352 | 88,200 samples/s; source 25.4016 MHz / 288 |

Compare each device's counter delta divided by its frequency with main CPU cycle
deltas divided by `main_cycle_hz`. Main cycles accumulate with unsigned 32-bit
subtraction so normal rollover is included. Board resets discard the interval
that crosses the reset and start a new `epoch` with cleared totals. This checks
frequency consistency, not exact MAME instruction costs or bus-event ordering.
GPU `modes` counts the endpoint status bits 19 and 20 of each frame; it is not a
trace of mode changes within a frame.

| Scope | What its elapsed interval includes |
| --- | --- |
| `cpu_inclusive` | `psxCpu->Execute`, including board devices and GPU work called within it |
| `h8` | Each H8 execution slice |
| `c352` | C352 sample generation |
| `board_audio` | Delivery to the board audio callback, including its queue wait |
| `gpu_list` | GPU command list parsing and any rasterization/flushes it invokes; sampled every 31 calls |
| `gpu_flush` | Explicit `renderer_flush_queues`; internal parser flushes are included in `gpu_list` instead |
| `video` | External frontend video callback |
| `present_copy` | Capture copy, scale map update and RGB565-to-display conversion inside `video` |
| `present_wait` | Frontend display vblank wait inside `video` |
| `audio_wait` | Audio queue backpressure inside the audio callback |

These intervals are nested. Do not sum the table or label subtraction of sampled
GPU time from CPU time an exact CPU residual. `estimated_us` extrapolates the
sample mean to the section's total call count; periodic GPU sampling can be
biased. Other sections use stride 1. Calls made before the first profiled frame
are excluded.

On Vita, timestamps use `sceKernelGetProcessTimeWide`. The tested Vita3K host
implements this using its host `high_resolution_clock` RTC in microseconds; the
values measure elapsed time, including blocking, rather than host CPU usage.
The checker also writes `<log>.perf.jsonl` with independent host monotonic
timestamps when it receives frame summaries, CPU-driver start and completed
replay messages. Reception is slightly later than guest emission. Compare the
differences between milestones to calibrate the guest clock. Logging and scope
timers have overhead, so use a same-source profiler-off control for throughput.

## Frontend architecture

The broader board/frontend coverage and existing optional workers are recorded
in [pcsx-arcade-architecture.md](pcsx-arcade-architecture.md).

The validated NEON VPK uses ARI64 for PSX MIPS-to-ARMv7 translation. Vita3K uses
Dynarmic outside it to execute the Vita ARMv7 application on the Mac. H8 remains
a separate interpreter. The GPU uses the C SIMD implementation with actual ARM
NEON instructions (`GPU_NEON_USE_C=1`); this setting does not select a scalar
renderer.

GPU, SPU, CD-ROM and DRC compiler workers are disabled in this VPK. The core runs
sequentially in the standalone loop. SDL audio playback has a queue and can block
the producer, but its effective backend thread count and core affinity have not
been measured. PCSX contains optional async infrastructure; there is no validated
Vita worker/core allocation here yet.

The Mac headless harness additionally calls `system12_sync` after every PSX
`ExecuteBlock`. Deltas below 256 main cycles are carried until a later call. The
libretro/Vita path calls it at both vblank edges, about twice per frame. The two
paths therefore have different H8/shared-RAM event order and C352 chunk sizes.
Matching aggregate clocks does not establish frontend equivalence. Neither bus
handler currently forces H8 synchronization on shared-RAM reads/writes. The new
timestamped C352 boundary catches up before H8 sound-register reads/writes;
key-on/off and BUSY-read fixtures now pass independently of fine/coarse MIPS
blocks. Complete MAME scheduler and frontend equivalence remain unverified.

Parallelizing H8 freely with PSX would introduce observable RAM and IRQ races.
Keep board time and IRQ delivery on an ordered emulation schedule. GPU async can
be evaluated through existing infrastructure once VRAM readbacks and barriers
are covered. CPU/GPU threading alone cannot produce 60 fps if H8 still consumes
more than 16.7 ms of work per emulated frame. Optimize measured H8 work first,
then reassess the benefit of distributing isolated work across Vita cores.

## First controlled diagnostic, 2026-10-05

The private profiler copy was based on `build/vita-system12/src`, with instrumentation
only in the four core/frontend paths above plus the standalone callbacks. The
original frozen VPK (`55258ebe…e1e8aa81`) and original core archive were preserved.
The matching original standalone object had been overwritten, so no bit-for-bit
frontend rebuild claim is made. The same-source profiler-off control rebuilds
all instrumented translation units without the macro and reuses unchanged core
objects from the full profiler build.

Profiler VPK SHA-256:
`490d3bdf3dce8b4d66402bd6ce70235515fd4f3f017d355134f31097731c944b`.
Host SHA-256:
`1f8f370352c0d344f0c9c0ada9551d0739c92ae693d87fc149ea3ded34d4120d`.
Build manifests are in `build/vita-system12-profile-20261005/build-evidence.json`
and `build/vita-system12-profile-off-20261005/build-evidence.json`.

The guarded 6,000-frame replay completed with 75/75 inputs, report, nonblack
framebuffer capture, no recorded host execution issues, and normal process exit.
Evidence: `/tmp/pcsx-neon-profile-6000-20261005/`. This remains a partial System12
validation, with `board_execution_verified=false`.

| Counter | Observed delta | Emulated duration |
| --- | ---: | ---: |
| Main | 5,014,405,004 cycles | 100.28810008 s |
| H8 | 1,698,318,802 cycles | 100.2881000803 s |
| C352 | 8,845,410 samples | 100.2880952381 s |

H8 differs from the main-derived expectation by about 0.005 cycles and C352 by
less than half a sample. Guest and independent host elapsed deltas over each
600-frame interval differed by at most 144 microseconds.

| Timed scope | Cumulative elapsed | Share of total diagnostic elapsed |
| --- | ---: | ---: |
| Total diagnostic elapsed | 361.365 s | 100% |
| CPU inclusive | 310.372 s | 85.9% |
| H8 (inside CPU) | 273.680 s | 75.7% |
| C352 (inside CPU) | 6.557 s | 1.8% |
| GPU lists, sampled (inside CPU) | estimated 17.430 s | estimated 4.8% |
| External video callback | 50.765 s | 14.0% |
| Display wait (inside video) | 43.695 s | 12.1% |
| Copy/scale (inside video) | 7.045 s | 1.9% |
| Audio queue wait (inside board audio) | 0.035 s | 0.01% |

H8 retired 297,843,065 instructions and ran in 11,999 slices. This is the measured
dominant section in this diagnostic, including gameplay.

The same-source profiler-off control also passed 6,000 frames and normal shutdown:
`/tmp/pcsx-neon-profile-off-6000-20261005/`, VPK SHA-256
`d0c751368c32e5c566a55ef9b64749c4d55743a55338a8a591cabbfac1d3d326`.
Host monotonic reception timestamps from CPU-driver start to replay completion
span 361.434 seconds, or 16.60 fps. The profiler's 361.365 seconds is within
0.02% of that interval. This single pair does not establish a statistical
instrumentation overhead bound; it does show consistent throughput for this run.
The final PPM, full board report and replay input CSV are byte-identical between
on and off. PPM SHA-256:
`21648aed86d979da130e4da29a36873f47b5f5bbe5863797ee24ac20f81356c0`.
No physical Vita performance claim follows from these Vita3K measurements.

## H8 IRQ selection optimization

`fdfe62bd` replaces the unconditional scan of vectors 7–63 with iteration over
the pending bits in ascending order. The six external interrupt-line checks,
priority/filter rules, lowest-vector tie handling, latching, and all calls from
instruction prefetch, tick and execution remain unchanged. No instructions or
device events are skipped. VitaSDK lowers the trailing-zero operation to inline
ARM RBIT/CLZ; the tested Dynarmic A32-to-ARM64 path supports both operations.

The differential test compares against the old scan over empty, one-bit, two-bit,
priority/mask and 100,000 deterministic random states. It and the synthetic H8
instruction/IRQ/timer/SCI/ADC/scheduling suite pass normal and UBSan builds.

The profiler-off optimized VPK SHA-256 is
`b9ed187b9505f1bc3b13eab9d32e7faec542dcd6aa738393f9549cd0f48bab8a`.
The guarded same-input 6,000-frame replay passed at
`/tmp/pcsx-neon-h8-irq-6000-20261005/`:

| Variant, profiler off | Host elapsed | Mean emulated frames/s |
| --- | ---: | ---: |
| Original IRQ scan | 361.434 s | 16.60 |
| Pending-bit iteration | 184.165 s | 32.58 |

This is a 1.96× improvement for this measured replay. Final image, complete board
report and input CSV match byte-for-byte; main/H8/C352 clocks, scheduling and
counter totals are unchanged. Both guests exited normally. This is a single
paired experiment; per-scene throughput varies.

The native Mac 12,000-frame replay also retained the original frame and entire
VRAM hashes, 10,029,060,040 main cycles, 3,396,722,206 H8 cycles, 597,909,593 H8
instructions, and 17,691,261 C352 samples. Evidence:
`/tmp/pcsx-mac-h8-irq-soul-12000-20261005/`. Native emulation elapsed was 67.742 s;
the previous baseline ran concurrently with Vita3K, so it is not used for a
paired native speedup claim.

## Current cost and architecture recommendation

The post-change instrumented VPK
`024b73e4d4679fd14b28d7ef72b1a42e76aeee9565d92f677761ccde30193419`
also passed the same replay and identical output/report at
`/tmp/pcsx-neon-h8-irq-profile-6000-20261005/`. Guest elapsed was 188.922 s;
independent host elapsed was 188.928 s. This is 2.6% above the uninstrumented
optimized run; individual scope times and sampled GPU estimates should be read
with that measurement limitation.

| Post-change scope | Elapsed | Share of instrumented total |
| --- | ---: | ---: |
| CPU inclusive | 135.644 s | 71.8% |
| H8, inside CPU | 89.769 s | 47.5% |
| C352, inside CPU | 6.911 s | 3.7% |
| GPU lists, sampled inside CPU | estimated 21.705 s | estimated 11.5% |
| External video | 53.030 s | 28.1% |
| Vblank wait, inside video | 45.659 s | 24.2% |
| Copy/scale, inside video | 7.342 s | 3.9% |
| Audio queue wait, inside board audio | 7.002 s | 3.7% |

The H8 scope fell from 273.680 to 89.769 seconds. It remains the largest measured
compute section. GPU and main-CPU work still run sequentially with it; frontend
display and audio waits have become more visible. This table is not an exact
exclusive partition because of nested scopes and periodic GPU sampling.

Keep the current synchronous architecture while studying these three points:

1. Continue examining H8 interpreter/bus/peripheral costs while preserving
   retired instructions, cycle budgets and IRQ/event boundaries.
2. Reconcile Mac and libretro board scheduling before using their results as
   evidence of equivalent hardware behavior. A common causal scheduler must
   account for shared RAM, IRQ delivery and C352 register/sample timestamps.
3. Review frontend pacing and buffer ownership together: video vblank waits and
   audio queue backpressure can both block the same emulation thread. Additional
   workers do not automatically remove that blocking.

Threading is an architecture study only. No workers or core affinities were
changed. If parallel execution is later evaluated, the existing GPU worker is a
candidate with explicit VRAM/readback barriers. Keep PSX/H8 shared-memory and IRQ
ordering defined; do not run H8 independently without a causal synchronization
design. SDL already has an audio playback path, whose actual threads and
affinities still need measurement. There is no validated allocation across Vita
cores 0/1/2 yet.

Combined manifests and comparisons:
`build/vita-system12-h8-irq-20261005/comparison-evidence.json`.

## Presentation and interpreter follow-up: source review only

The final post-change log distinguishes 6,000 video callback entries from
**4,761 valid image conversions/display waits**. `video_cb` returns immediately
for null data or zero dimensions. The measured averages are:

| Scope | Calls | Mean per call | Mean per emulated frame |
| --- | ---: | ---: | ---: |
| H8 slice | 11,999 | 7.481 ms | 14.962 ms |
| Display wait | 4,761 | 9.590 ms | 7.610 ms |
| Capture/copy/scale | 4,761 | 1.542 ms | 1.224 ms |
| Audio queue wait scope | 35,999 | 0.194 ms | 1.167 ms |

Audio wait scope calls include checks that do not actually sleep. Copy time
includes the enabled final-frame capture copy; it is not solely RGB conversion.
The averaging denominator matters: neither display wait nor H8 slice occurs
exactly once per emulated frame. Evidence is the final `PERF` block in
`/tmp/pcsx-neon-h8-irq-profile-6000-20261005/vita3k.log`.

The private host source used by this replay is
`build/vita-validation-mspace-stats-fix2/host-fix-src`. Its display wait and
vblank sync implementation matches the corresponding functions in `../vita3k`;
the files as a whole differ because of the private lifetime/teardown fixes and
display logging. `SceDisplay.cpp::display_wait` chooses at least
`vblank_count + 1` for `sceDisplayWaitVblankStart`. `display.cpp` increments that
counter on a host thread with a nominal 60 Hz cadence and suspends the waiting
guest thread until the target count. Thus the frontend still incurs a next-edge
wait when emulation is already slower than real time. This explains the
blocking mechanism, not a statistical prediction of each wait's duration or
physical Vita behavior.

The frontend fills the alternate framebuffer, submits it with `NEXTFRAME`,
waits, and then swaps its front index. Removing that wait without a replacement
buffer-lifetime contract could let the producer overwrite a buffer still being
displayed. The appropriate pacing study is to distinguish buffer availability
from real-time throttling, including the host's predicted-frame/display access
and teardown lifetime. Keep the emulated vblank/IRQ timeline independent of this
presentation decision. No wait or buffer implementation has been changed.

Audio uses an SDL stream with source 88.2 kHz and device/output 48 kHz, confirmed
by the replay log after the board AV update. The frontend's queue threshold is
8,192 bytes (2,048 stereo S16 source frames, nominally 23.22 ms at 88.2 kHz).
It delays the producer in 1 ms increments while the queue exceeds that limit.
Removing display waiting can move backpressure to this queue instead of removing
it. Source sample cadence, device resampling and queue occupancy must be assessed
together before selecting one pacing policy.

Before changing pacing, a future diagnostic should record presentation return
codes, buffer identity and vblank count before/after each wait, plus audio queue
occupancy, wait iterations and maximum blocked interval. Aggregate these into
periodic summaries rather than per-sample logs. Current gameplay presentation
return codes are ignored, and an audio queue that stops draining can keep this
callback waiting without a local timeout or further input polling. Neither
case should be diagnosed from the FPS number alone. Preserve the existing
external watchdog and verify clocks/replay signatures in any diagnostic build.

The source review identifies these H8 candidates; their individual cost has
**not** been measured within the 47.5% H8 scope. That scope also includes bus and
peripheral callbacks invoked by H8 execution, not only instruction arithmetic:

- `h8_run_cycles`, `prefetch_done` and `tick` all select pending IRQs. The recent
  optimization made each selection cheaper. Further caching would need to track
  every pending/enable/priority/CCR change and preserve the original recognition
  point, latching and acknowledgement; dropping calls is not justified.
- `tick` checks five timers, two SCI deadlines and ADC after each execution
  state, including intermediate prefix-decoder states as well as completed
  instructions and exception states.
  Active internally clocked timers iterate each timer tick. A future deadline or
  arithmetic implementation must preserve compare-before-increment semantics,
  overflow, simultaneous A/B matches, clear modes, fractional divider phase,
  status/enable transitions and interrupt observation. Timer-specific profiling
  and differential state/event checks should precede implementation.
- Instruction dispatch uses the generated decoder, and fetch/data accesses use
  bus callbacks with side effects and cycle charges. Decode caching or a new H8
  DRC would need fetch/SMC, atomic C352 word access and precise IRQ/cycle guards.
  No benefit estimate or implementation is established here.
- The board's `sub_read16` resolves two byte reads for ROM/shared accesses.
  A direct word path is another candidate if it retains big-endian assembly,
  boundary behavior and access counters. C352 atomic word side effects must
  remain on their existing distinct path.

First investigate timer and IRQ cost while retaining synchronous execution;
then choose between a faithful interpreter improvement and a larger H8 backend
change from measurements. A worker would relocate the same interpreter cost and
introduce shared-memory ordering requirements. Presentation changes, H8 backend
work and parallel execution remain proposals for review, not runtime changes.

## Mac C352 timing fidelity, 2026-10-05

The priority is a faithful Mac reference before selecting further speedups.
The first concrete mismatch was C352 register/sample ordering: the old adapter
executed the whole H8 interval, then generated all its audio using the resulting
register state. A key-on could affect a sample preceding its bus access; a read
could inspect BUSY before the samples that should clear it had been generated.

Commits `3124e90a` and `b5a670d4` expose the H8 access cycle and catch up C352
before each byte/word read or write. The timestamp includes costs charged inside
the current execution state. SCI/ADC callbacks after `tick` use committed cycles
without charging that state again. Word writes remain atomic. One monotonic
stream cursor combines register catch-up and the existing board catch-up.

The H8 phase uses `floor(1e18 / 16934400)` attoseconds per cycle, matching the
pinned MAME scheduler's accumulated normal subsecond slices. At 88200 Hz the
reduced conversion is `26041666666617 / 5000000000000000`. Bounded 64-bit chunks
preserve that phase without requiring 128-bit arithmetic in the Vita core.
Host tests compare it with an independent wide-integer reference over sample
edges, split updates, rewind rejection and 24 hours. Time zero emits nothing;
at positive time the stream includes its current sample as MAME does.

`c2e1f5f7` adds a real MAME oracle. The installed executable reports **0.287
(unknown)**; its precise source commit is unavailable. Source reasoning remains
against pinned `deb2f54fe7300e7a192690318beb860159bfd52a`. The oracle transiently
replaces H8 instructions and the first wave bytes in memory after loading
`soulclbrac` from the local `soulclbr.zip` archive. It captures the C352 sound
hook before speaker resampling, uses private config/NVRAM directories, and
leaves the archive unchanged. Every run has a 45-second kill timeout.

The actual MAME capture and native adapter produce these identical front-channel
raw sample prefixes (left and right are equal):

| Key-on H8 cycle | Samples 0..4 | First nonzero | First silent after key-off |
| --- | --- | --- | --- |
| 190 | 0, 8, 16, 24, 0 | 1 | 4 |
| 192 | 0, 8, 16, 24, 0 | 1 | 4 |
| 194 | 0, 0, 8, 16, 0 | 2 | 4 |

MAME exposes normalized floats; multiplication by 32768 yields these integers
exactly. Native integration tests run the actual H8 firmware through the board
with both MIPS interpreter and DRC, fine and coarse main CPU blocks, and an
additional one-shot BUSY/key-off read fixture. All pass. The prior binary fails
the 190-cycle test by starting at sample zero, so the test detects the original
regression rather than merely accepting both implementations.

Reproduction:

```sh
make -f Makefile.mac-harness BUILD=build/mac-system12-audio-time-20261005 DRC=1 SYSTEM12_TIMING=1 -j4
make -f tests/Makefile.h8 test irq-test
make -f tests/Makefile.c352-timing test
python3 tests/check_system12_audio_timing.py --harness build/mac-system12-audio-time-20261005/pcsx-harness
python3 tests/check_system12_audio_timing.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_mame_c352_timing.py
```

Normal and UBSan C352/clock tests pass; existing synthetic board and native DRC
bank/RAM/SMC checks pass. Evidence logs are
`/tmp/pcsx-mac-audio-time-{interpreter,drc,board-test,drc-test}-20261005.log`,
`/tmp/pcsx-c352-timing-{root,ubsan}-20261005.log` and
`/tmp/pcsx-mame-c352-oracle-20261005.log`. The older failing run is retained at
`/tmp/pcsx-audio-timing-old-regression-20261005.log`.

An isolated native NEON/ARI64 Soul Calibur replay of **12000 frames** completed
with the same input script as the earlier IRQ-optimization replay. Main cycles
remain `10029060040`; H8 cycles remain `3396722206`, with `597909593`
instructions, no illegal/unsupported instruction and no unmapped board access.
PCM frames are now `17691262`, one more than the old stream, consistent with
including the current sample. The final framebuffer and synchronized VRAM are
byte-identical to the earlier replay. This is regression evidence, not a claim
of pixel equivalence to MAME or of complete game compatibility.

The harness reported 65.965 seconds (66.516 seconds including startup/teardown).
One replay does not establish a speed improvement. Audio generation from H8 bus
callbacks now falls inside the inclusive H8 profiling scope, so future scope
comparisons must account for that nesting. Frozen evidence is under
`/tmp/pcsx-mac-c352-timed-soul-12000-20261005/`, including the binary, log,
framebuffer, VRAM and `evidence.json`. Binary SHA-256:
`cd3377f6597b71e25a3586e8224fd78baf746956e0c8b39dadda444cf4123580`.

Next fidelity gates are actual scheduler/shared-mailbox and vblank traces,
H8 peripheral deadlines/partial execution, and soft-reset phase. The current
reset starts a new H8/audio epoch; MAME preserves CPU local time and absolute
stream phase. Register-edge tests do not cover that difference. No new Vita,
vitaGL, worker or H8 DRC validation is claimed for these commits. Optimization
should preserve the established bus/state/sample traces and then demonstrate a
measured reduction in cost; a worker or DRC is still an architectural candidate.
