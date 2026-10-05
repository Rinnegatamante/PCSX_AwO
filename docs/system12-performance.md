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
handler currently forces H8 synchronization on shared-RAM reads/writes. H8
register writes are also applied before generating the entire audio interval for
that slice; precise C352 write/sample ordering remains unverified.

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
