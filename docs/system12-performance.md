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
subtraction so normal rollover is included. The profiler starts a new `epoch`
with cleared totals at reset. The newer System12 reset implementation catches
up before clearing the CPU counter and preserves emulated absolute device time;
its exported profile counts remain relative to the new epoch. The frozen Vita
measurements below predate that change. This checks
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

At the C352-access milestone, the next fidelity gates were actual
scheduler/shared-mailbox and vblank traces, H8 peripheral deadlines/partial
execution, and soft-reset phase. The reset mismatch is addressed in the next
section; register-edge tests alone did not cover it. No new Vita,
vitaGL, worker or H8 DRC validation is claimed for these commits. Optimization
should preserve the established bus/state/sample traces and then demonstrate a
measured reduction in cost; a worker or DRC is still an architectural candidate.

## Soft-reset retention and phase, 2026-10-05

Commits `056f7832`, `eca1460d` and `2dcecc79` separate initial machine creation
from soft reset. Source review found that `namcos12_state::driver_start` sets
banks and DMA offsets, whereas `driver_reset` only repeats the optional boot
copy. CPU main RAM and shared RAM are not erased by reset. H8 reset preserves
ER/CCR, internal RAM and the mapped GPIO registers. MAME preserves CPU local
time and the sound stream cursor while resetting the C352 voices/control.

The former adapter cleared all those storage/phase domains. It now retains main
and shared RAM, the bank selector and offsets, held input levels, modeled H8
storage/GPIO, clock conversion remainders and the absolute PCM cursor. COH-716
still repeats its source boot copy of exactly 12 bytes. `system12_pre_reset`
catches up even a pending interval shorter than 256 main cycles before
`psxReset` clears the main counter.

H8 reset is explicit about instruction debt: MAME's H8 engine clears its carried
overshoot at reset, and its scheduler preserves budget-based local time rather
than advancing by the overshot instruction tail. The board accumulates requested
H8 clocks separately from its executed-state diagnostic count and supplies that
absolute budget to `h8_soft_reset`. The CPU resets execution/peripheral state,
retains ER/CCR/RAM/GPIO, and starts a new diagnostic epoch. Old SCI/ADC deadlines
are cancelled; future deadlines use the new absolute domain. The stream clock
rejects a temporary backwards access timestamp without modifying its cursor,
then resumes once the CPU reaches the last rendered access time. Audio callbacks
are synchronous and must not reset or re-enter the core.

The independent real MAME reset oracle requests reset at
`0:16715113233669674` attoseconds from `frame_done`. The reset notifier observes
the same time. The raw C352 capture contains a first burst at sample 1
(`8,16,24,0,0`) and a second at absolute sample **1476** (`8,16,0,0,0`), proving
that the output timeline does not restart at zero. The second onset and key-off
phase match the timestamp prediction exactly, without a one-sample tolerance.
Both status reads are `0006`. As before, this installed MAME reports 0.287 with
unknown build commit; pinned-source reasoning and runtime evidence are distinct.

Native integration tests execute a MIPS boot counter and initialize RAM/shared
markers and bank 2 only on the first boot. After the harness invokes `psxReset`,
the guest reads and stores the retained state rather than recreating it. Five
cases per backend cover four COH-700 reset times, including the same PCM phase
bin as the MAME capture, and a COH-716/altbank reset. They verify two boots, main
and shared RAM, banked reads, held coin/start, timed H8 audio, the 12-byte boot
copy, its untouched neighboring word and retained RAM near the top of the 16 MiB
configuration. Interpreter
and DRC produce identical observed reset times/onsets for these cases.

The previous System12 reset implementation was separately compiled with the
new harness diagnostic switch and a no-op pre-reset shim. The same test fails:
the second burst includes an extra nonzero sample at index 7 rather than going
silent there. This baseline binary/source/build log is retained under
`/tmp/pcsx-system12-pre-reset-baseline-20261005/`; its failure is logged in
`/tmp/pcsx-system12-reset-old-regression-20261005.log`.

Reproduction after building the source-timing Mac DRC harness:

```sh
make -f tests/Makefile.h8 test irq-test
python3 tests/check_system12_reset.py --harness build/mac-system12-audio-time-20261005/pcsx-harness
python3 tests/check_system12_reset.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_mame_c352_reset.py
```

H8 retention/debt tests pass at O2 and with UBSan, including 64-bit/zero epochs,
repeated reset, prefix/illegal-state disposal, and rejection during bus/SCI/ADC
callbacks. Existing H8, IRQ, C352-access, board/profile and native DRC bank/RAM/SMC
fixtures also pass. The new harness switch `--reset-at-cycle N` requests one
System12 reset at the first safe block endpoint reaching that cumulative main
cycle; it preserves replay frame progress and fails if the request is not reached.
It is a validation control, not an exact-instruction scheduling guarantee.

Evidence logs:
`/tmp/pcsx-h8-soft-reset-root-20261005.log`,
`/tmp/pcsx-h8-soft-reset-ubsan-root-20261005.log`,
`/tmp/pcsx-system12-reset-{interpreter,drc,board,existing-drc}-20261005.log`,
`/tmp/pcsx-system12-reset-existing-audio-{interpreter,drc}-20261005.log`,
`/tmp/pcsx-mame-c352-reset-oracle-20261005.log` and
`/tmp/pcsx-mame-c352-oracle-busy-20261005.log`.

A second isolated **12000-frame no-reset** Soul Calibur replay verifies creation
and normal execution after this lifetime change: final framebuffer, VRAM, main
cycles `10029060040`, H8 cycles `3396722206`, H8 instructions `597909593` and PCM
frames `17691262` match the preceding C352-timing replay. It completes without
illegal/unsupported H8 instructions or unmapped board access. This does not
itself validate a real-game reset. Frozen binary/log/evidence are under
`/tmp/pcsx-mac-reset-fidelity-soul-12000-20261005/`; binary SHA-256 is
`d39c2e233c911d419f335537f0b5e735500d1a9cdfce1a24cf145f468b0a2a33`.

An authentic **3600-frame warm-reset** replay also completes: the harness
resets at the safe block endpoint reaching main cycle `835756`, then continues
the existing input script. The captured image shows Stage 1, Ivy versus Kilik,
with timer 24 and credit 0. It reports no illegal/unsupported H8 instruction or
unmapped board access. Frozen output and the PNG preview are under
`/tmp/pcsx-mac-reset-fidelity-soul-warm-3600-20261005/`. This demonstrates resumed
game execution; no identical real-game MAME reset replay was captured, so it
does not establish gameplay-state or pixel equivalence across that reset.

MAME's immediate H8 debug PC/IR/temporary snapshots before reset prefetch also
retain old values that this standalone API reconstructs. Source reset execution
overwrites its active vector/prefetch fields before executing guest instructions;
the current gates make no claim of identical prefetch-time debug snapshots.

Full scheduler/shared-mailbox/vblank and H8 partial-state/peripheral equivalence
remain open. No new Vita or vitaGL execution is claimed, and no worker or H8 DRC
has been introduced. The scope remains all ten PSX-derived driver families;
these are System12 fidelity gates, not completion of a whole family.

## Measured Mac vblank discrepancy, 2026-10-05

`2d953d84` adds passive pin diagnostics, without changing device clocks or the
scheduler. `--system12-edge-trace FILE` records vblank delivery and H8 port-B
input observations before GPIO masking. It retains the last old and first new
poll around each transition. Rows include the 32-bit main counter, its last
synchronized value, absolute H8 access cycles and accumulated H8 budget. The
main counter is not an instruction-exact PC/time trace under every DRC path.
Reset/unload ownership and output truncation are explicit; overflow or file
errors fail the harness run.

The native and MAME fixtures use identical H8 firmware, words
`6a08 ffd6 40fa`: read port B at `0xffffd6`, then branch back. Reset prefetch
places the first read at H8 cycle 12; subsequent reads are ten cycles apart.
Both fixtures replace MIPS startup with a jump/delay-slot-NOP loop. The native
coarse case inserts 1024 NOPs before that jump to expose block-dependent event
delivery. No authentic ROM archive is modified.

The MAME oracle taps actual CPU program-space reads, normalizes the high byte
of the 16-bit big-endian bus, and retains integer attosecond timestamps. Its
reset notifier establishes epoch zero. Screen API targets bracket each last-old
and first-new read; the initial start target is reconstructed from consecutive
exact start targets, rather than treating `frame_done` as a vblank timestamp.
The first read and every reported read gap are checked against the truncated
H8 attosecond period. The installed MAME still reports **0.287 (unknown)**;
source reasoning uses the pinned tree, not a verified matching executable.

The first four transition observations are:

| Port-B bit 7 becomes | MAME first new H8 read | Mac fine-block first new read | Mac coarse-block first new read |
| --- | ---: | ---: | ---: |
| 1 | 283062 | 240702 | 241172 |
| 0 | 325402 | 283012 | 283562 |
| 1 | 566122 | 523792 | 524032 |
| 0 | 608462 | 566102 | 566422 |

Native MIPS interpreter and ARI64 produce the same table. The observer-on/off
controls have identical board reports and framebuffers in each native case.
Those controls validate passive capture, not MAME fidelity: the strict
`compare_system12_vblank.py` gate **fails** for both backends and both block
sizes, with no timing tolerance.

MAME's first scheduled start is `0:16715113233669674` attoseconds, followed by
its end exactly 2.5 ms later. The fine native case instead delivers start at
main cycle 710756 (14.21512 ms) and end at 835756 (16.71512 ms). This reveals a
phase discrepancy of about 2.5 ms, in addition to synchronization granularity.
The fine case leaves main-cycle tails `[100, 172, 16, 88, 188, 4]` pending when
delivering its six edges. Coarse block delivery removes that tail but is late
relative to the native counter deadlines; its first start is cycle 712044.

Source inspection explains the phase discrepancy. Our source-timing
`scheduleRcntBase` places the rising edge at `frameCycles - PSXCLK/400` and the
falling edge at `frameCycles`. Pinned `screen.cpp` starts its vblank timeline at
zero and schedules the next start a full period later, then the end after its
configured blank duration. `psx.cpp` configures a 2500 us blank duration, which
the source explicitly labels approximate. Fidelity to this MAME model requires
its phase as well as its average refresh rate; this capture does not establish
physical arcade timing accuracy.

The next correction must cover source screen phase/deadlines, fractional frame
periods, mode changes and reset continuity, then order H8 execution and external
edges at those deadlines. Merely forcing the sub-256 tail or shifting one
callback does not establish partial-instruction/bus ordering. Timed shared-RAM
protocol and IRQ/ADC fixtures remain separate requirements. No scheduler fix,
worker, H8 DRC, Vita or graphics validation is claimed by this measurement.

Reproduction:

```sh
python3 tests/check_mame_system12_vblank.py --output /tmp/mame-vblank.json
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --output /tmp/mac-vblank-interpreter.json
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc --output /tmp/mac-vblank-drc.json
python3 tests/compare_system12_vblank.py --mame /tmp/mame-vblank.json --native /tmp/mac-vblank-interpreter.json /tmp/mac-vblank-drc.json
```

At pre-correction revision `42e64dc2`, the capture fixtures pass and the last
command returns 1 for the measured fidelity failure. Frozen native binary/evidence are under
`/tmp/pcsx-mac-vblank-fidelity-20261005/`, binary SHA-256
`320f9386de8e9ca4d71294df0dccb5dd421bb746f10c9726e447f27bb5e70feb`.
Root MAME evidence is `/tmp/pcsx-mame-system12-vblank-root-20261005.json`
with adjacent stdout log. Comparison evidence is
`/tmp/pcsx-system12-vblank-comparison-20261005.{json,log}`.

## Mac source screen correction, 2026-10-05

Mac fidelity to MAME remains the first target. Optimization work must preserve
the compared device observations; neither a frame-rate improvement nor this
narrow gate establishes complete board fidelity.

`a1b20e0c` adds a standalone screen clock modeled on pinned MAME
`screen_device::configure/vblank_begin`, with integer-attosecond periods and
the source's approximate 2500 us blank duration. It retains fractional frame
phase, the previous start origin on refresh changes and an already armed end
timer. The cold end time exists for `screen.vblank()` queries but its timer is
unarmed until the first actual start. The component preserves phase across
GPU and machine reset rather than restarting a frame at reset time.

`bff1141c` generates both full and partial H8 instruction states using MAME's
bus checkpoints. `h8_run_until` advances the committed H8 clock to an absolute
external deadline and resumes the partial state after the caller delivers the
edge. A bus operation can cross the deadline: its callback observes the charged
cycle before the edge, and the remaining charge is carried forward without
repeating the access. This follows the post-access checkpoint contract, rather
than claiming that every callback is bounded by the external deadline.

`cc02244d` schedules source screen events independently of
MIPS block endpoints. It runs the H8 to the floor of the event's absolute time
on the truncated H8 clock, then delivers IRQ1, ADTRG and port-B bit 7 in source
order. GP1 reset/display-mode writes update the source screen configuration;
warm reset restores GPU defaults and retains absolute screen time. Compatibility
timing and other board families keep their existing counter path.

`cafcc034` also preserves catch-up for frontends that rely on core vblank
events. RTC/EEPROM/PCM advance after all overdue screen edges have been
delivered, so H8 execution cannot pass a later queued edge while catching up
to a late MIPS endpoint. The native diagnostic option
`--system12-vblank-sync-only` omits the harness's extra block synchronization.
Before this fix its silent-ROM fixture left C352 at zero generated frames;
after the fix PCM advances and the same strict H8 polling gate passes on both
CPU backends and block sizes. This simulates the cadence in Mac, without
claiming an actual libretro/Vita run.

The strict polling comparison now passes with **zero cycle tolerance**:

| Port-B bit 7 becomes | MAME old → new H8 read | Mac interpreter, short/long blocks | Mac ARI64, short/long blocks |
| --- | ---: | ---: | ---: |
| 1 | 283052 → 283062 | 283052 → 283062 | 283052 → 283062 |
| 0 | 325392 → 325402 | 325392 → 325402 | 325392 → 325402 |
| 1 | 566112 → 566122 | 566112 → 566122 | 566112 → 566122 |
| 0 | 608452 → 608462 | 608452 → 608462 | 608452 → 608462 |

Observer-on/off controls retain identical native board reports and images.
The recorded main-counter synchronization tail can still be nonzero when an
edge is traced: the H8 is now advanced separately to the scheduled edge time,
so that field describes the last ordinary bridge synchronization, not the
delivered H8 deadline. MIPS interrupt delivery remains subject to the core's
block boundary granularity; no instruction-exact MIPS interrupt claim follows
from the H8 comparison.

Seven real installed-MAME captures cover initial phase, NTSC480, PAL, a change
during blank, shortening a frame past its elapsed age, GP1 reset and machine
soft reset. `compare_psx_screen_clock.py` compiles the actual C component and
replays their exact integer write/reset/sample timestamps. All queried start
and end targets match with zero tolerance; rounded floating-point period
diagnostics are not used for comparison. The installed executable's source
revision is still unknown. Evidence is `/tmp/mame-psx-screen-clock.json`.

`f0de5e78` executes GP1 mode writes from synthetic native MIPS guests on both
interpreter and ARI64. It checks exact absolute H8 edge clocks for NTSC480,
PAL and PAL-to-default GPU reset. The during-blank write retains the old end
and uses the previous start as the new period's origin; a main-CPU trace
confirms the write occurs inside that window. Warm reset at actual main cycle
850002 retains the active end and schedules the next default start from the
last NTSC480 start, rather than from reset time.

Standalone screen normal/UBSan tests and H8 normal, IRQ and deadline UBSan
tests pass. Native H8/C352 key-on/off, timed reads and warm-reset retention/PCM
phase gates pass on both interpreter and ARI64. Comparison evidence is
`/tmp/pcsx-system12-source-screen-final-comparison-20261005.json`; native
captures are `/tmp/pcsx-system12-source-screen-{vblank,vblank-only}-{interpreter,drc}-20261005.json`.
Console smoke and the existing synthetic System11, System10, Capcom ZN,
Taito FX1, System GV and G-Net gates also pass; they remain component/integration
regressions, not authentic-game validation of those families.

Additional reproduction commands, after the build used above:

```sh
make -f tests/Makefile.psx-screen-clock test sanitize
make -f tests/Makefile.h8 test irq-test deadline-ubsan-test
python3 tests/check_mame_psx_screen_clock.py --output /tmp/mame-screen-clock.json
python3 tests/compare_psx_screen_clock.py /tmp/mame-screen-clock.json
python3 tests/check_system12_screen_modes.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc --vblank-sync-only --output /tmp/mac-vblank-cadence.json
python3 tests/compare_system12_vblank.py --mame /tmp/mame-vblank.json --native /tmp/mac-vblank-cadence.json
```

An authentic NEON/ARI64 Soul Calibur replay completes **12000 frames** with
no illegal/unsupported H8 instruction or unmapped board access. Its final
image shows the continue prompt, credit 0. Main cycles are `10029192943`, H8
cycles `3396767298`, H8 instructions `598911789`, and PCM frames `17691496`.
The screen correction changes the gameplay timeline, so the previous replay's
framebuffer and counters are not an equivalence baseline for this run. Its
80.432 s elapsed time is a single smoke measurement, not a controlled speed
comparison. Frozen binary/log/image are under
`/tmp/pcsx-mac-screen-cadence-soul-12000-20261005/`, binary SHA-256
`f26faf50fcc9ed5644bc521e99ea8be45aec7ae4b4b7c42ce1017e57ed0c70d8`.
The framebuffer and all final board report lines equal the source-screen replay
before the frontend-cadence fix; `cadence-regression.json` records that control.

The following internal-device correction supersedes the earlier coarse
H8 timer/SCI/ADC scheduling. Timed shared-mailbox protocols,
MIPS IRQ ordering, real-game state/pixel/audio comparison and native/libretro
scheduling equivalence remain open. No worker, H8 DRC, new Vita or vitaGL
validation is claimed. Zero whole driver families are complete.

### Mac H8 internal-device correction (2026-10-05)

The ADC3337, H83002 timer16 and SCI source models now replace the coarse
register-array implementations. `h8_run_until` bounds partial CPU execution by
the next internal event as well as the external board deadline. ADC starts
from the charged write cycle; timers settle elapsed time before register
mutations and use source compare/divider arithmetic; SCI runs physical TX/CLK
transitions rather than delivering grouped bytes. System12 wires inverted
RTC CLK first, then settings CLK, and routes RTC DATA to RX1. SCI0's fixed
external clock period is `542534722222` attoseconds. Soft reset restarts local
peripheral cycles while keeping the board's absolute callback time and SCI
status-read/RX latches.

The independent installed-MAME probe injects four H8 programs: ADC start/poll,
timer0 counter polling, internal asynchronous SCI, and external-rate SCI.
Each emits 96 external observation stores. Native runs with 1/1024/10000-cycle
slices match **1152 value/address/cycle comparisons with zero differences**.
The fixed pre-port baseline `dde07db7` differs in 101 observations. For example,
the ADC read at cycle 280 still sees ADST in MAME and the new port, whereas
the old port has completed early; its observable store differs at cycle 288.
Internal SCI SSR polls bracket TEND between cycles 356 and 374; the source
external-rate case brackets it between 1508 and 1526. Parent ROM/template
hashes remain unchanged and all MAME runs exit under the watchdog.
Installed MAME is 0.287; matching that binary to the pinned source remains
unverified. These are register traces, not full RTC/JVS waveform captures.

Component normal/UBSan tests and H8 instruction/IRQ/deadline/reset tests pass,
including an odd soft-reset epoch above 32 bits. Native interpreter and ARI64
GP1 modes/reset, C352 key-on/off phase and warm-reset retention gates pass.
A fresh MAME vblank capture also matches the four initial old/new port-B read
pairs exactly for both CPU engines, both MIPS block sizes and both frontend
sync cadences.

An authentic Mac NEON/ARI64 **12000-frame** run uses the existing
`artifacts/system12/match-3600.csv` input script. It completes at main cycles
`10029193005`, H8 cycles `3396767298`, H8 instructions `590887759`, and PCM
frames `17691497`, without H8 illegal/unsupported or unmapped accesses. The
final image shows the VS-BATTLE ranking screen. This run has a different input
script from the former temporary replay and is not its state/image equivalence
baseline. The measured 57.912 s emulation elapsed is a smoke observation, not
a controlled optimization benchmark. The frozen executable SHA-256 is
`c991acc204feeffd8338b78a44f2f8013482a61232b09fcdd6e336602c4715b4`.

Evidence is retained in ignored `artifacts/validation/h8-peripherals-20261005/`:
`summary.json`, `mame-peripherals.json`, `vblank-comparison.json`,
`core-final.log`, `mac-gates.log`, and the frozen executable/input/log/PPM/PNG/WAV.
Older `/tmp` evidence referenced above is historical and was lost when the
execution environment restarted; current claims use these fresh captures.

Reproduction uses the Mac source-timing build command above, the component
gates in [H8 port details](h8-port.md), and:

```sh
python3 tests/check_mame_h8_peripherals.py --include-sci --output artifacts/validation/h8-peripherals-20261005/mame-peripherals.json
```

Open fidelity work includes the scheduler-attotime phase of RX clock starts
(the source distinguishes `now_as_cycles()` from TX `total_cycles()`),
standby/DMA/watchdog, timer external pins, complete RTC/JVS wire traces, timed
shared mailboxes and MIPS IRQ ordering, real-game MAME pixels/audio, and
native/libretro scheduler equivalence. The legacy whole-state `h8_run_cycles`
API retains coarse checkpoints. Worker/H8 DRC and new Vita/vitaGL validation
remain outside this milestone. No whole driver family is complete.
