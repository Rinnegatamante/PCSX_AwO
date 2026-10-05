# Current PCSX arcade architecture and recommendation

Source review: 2026-10-05, following the System12 clock and performance
experiments recorded in [system12-performance.md](system12-performance.md).
This is an architecture proposal; no workers, affinity changes, scheduler
refactor or additional board ports are implemented by this document.

## Current execution path

```mermaid
flowchart TD
    F[Mac harness or Vita standalone/libretro] --> M[PSX MIPS: interpreter or ARI64 DRC]
    M --> B[PSX memory bus and arcade board adapter]
    M --> G[GPU command processing: NEON or Vita renderer]
    M --> T[PSX counters, DMA and interrupts]
    B --> H[System12 H8 interpreter]
    H --> R[Shared RAM, inputs and sound registers]
    B --> C[C352 sample generation]
    C --> A[Frontend audio callback and playback queue]
    G --> V[Frontend video conversion and display wait]
    T --> B
```

The tested Vita package executes the emulation paths sequentially. Its main
MIPS DRC produces ARMv7 code. Vita3K adds an outer translation layer to run the
ARMv7 application on the ARM64 Mac; native Mac tests do not have that layer.
Consequently, native throughput is not a prediction of physical Vita throughput.
GPU SIMD and hardware graphics submission are distinct from a CPU worker.

The **H8** is the emulated Hitachi H8/3002 auxiliary CPU of System12, not a host
thread or another PSX CPU. It runs its own ROM at 16.9344 MHz, handles board I/O
and sound register accesses, and communicates with the MIPS through shared RAM.
`libpcsxcore/system12.c` owns its interpreter, shared backing, C352, EEPROM and
RTC. `system12_sync()` advances H8, then generates the C352 samples due for the
interval. The PSX bus exposes the mapped shared window; H8 bus callbacks handle
the different byte order.

H8 is not equivalent to the PSX SPU. The SPU is a sound synthesis chip;
System12's board audio chain is MIPS commands/shared RAM → H8 firmware → C352
sample generation. The existing SPU worker does not accelerate that chain.

## Existing optional workers

| Worker | Existing implementation | Control and limits |
| --- | --- | --- |
| GPU | `plugins/gpulib/gpu_async.c`: command queue, render thread, synchronization and teardown | `USE_ASYNC_GPU=1` at build time; `pcsx_rearmed_gpu_thread_rendering` at runtime. This is gpulib infrastructure, including the NEON path; support and correctness of each renderer require validation. |
| PSX SPU | `plugins/dfsound/spu.c`: channel work queue and completion synchronization | `USE_ASYNC_SPU=1` and `pcsx_rearmed_spu_thread`. It does not execute System12 H8 or C352 audio. |
| CD read-ahead | `libpcsxcore/cdrom-async.c`: sector cache and prefetch thread | `USE_ASYNC_CDROM=1`, with frontend read-ahead configuration. It does not cover the arcade ATA/NCR adapters. |
| ARI64 compiler | `libpcsxcore/new_dynarec/emu_if.c`: background block compilation | `NDRC_THREAD=1`; the Vita platform sets it to zero with the source comment `can't write to tc from thread`. This parallelizes compilation, not MIPS instruction execution. |

`frontend/pcsxr-threads.c` supplies common thread creation. Its explicit
core-selection logic is for 3DS; there is no validated Vita 0/1/2 allocation in
this work. `Makefile.vita` disables GPU, SPU and CD async, and the standalone
option table also disables GPU/SPU threading. The tested VPK uses those choices.
SDL playback has a queue; actual backend threads and affinity have not been
measured. The profiler currently rejects async GPU/SPU because its state and
nested scopes assume synchronous execution.

## Board and frontend coverage

This matrix describes reachable integration, not complete game compatibility.
The full target remains the [491 MAME registrations in ten driver
families](mame-psx-systems.md). Optional adapters require their build flags.

| Family | Mac integration / main components | Libretro integration and remaining boundary |
| --- | --- | --- |
| Retail PSX | Existing console CPU, GPU, SPU and CD core; native EXE smoke | Existing console path; full pinned retail machine validation remains open. |
| Sony ZN, including Taito FX1 | `systemzn.c` / `systemtaito.c`; common ZN security/SIO/EEPROM, Capcom Z80/QSound and FX1 Z80/YM2610 | No arcade profile loader for these adapters. Other ZN daughterboards and authentic games remain open. |
| G-Net | `systemgnet.c`; common ZN, Zoom MN10200/TMS57002/ZSG2, RF5C296 and CHD-backed ATA card | Optional profile loader exists. Synthetic MIPS storage execution passes interpreter/DRC; authentic boot, title I/O, Zoom audio mixing and persistent frontend state remain open. |
| Konami GV | `systemgv.c`; NCR/CD/CHD, EEPROM, DUART, IRQ10 and DMA5 | No arcade profile loader. Authentic BIOS/CD boot and special I/O remain open. |
| Konami GQ | Standalone `arcade/konamigq.cpp`; sound CPU/DSP/PCM and NCR HDD components | No adapter connecting the board to the PSX execution/frontend path. |
| System 573 | Security, flash and shared media foundations | No native arcade board adapter or frontend path. |
| Twinkle | Standalone `arcade/twinkle_board.cpp`; MC68000, RF5C400, wave RAM and mailbox | No native PSX frontend path; ATA/media and main-board integration remain open. |
| System10 | `system10.c`; MEM(M)/(N), crypto/banks, NAND DMA and I/O | No profile loader; authentic games and extension boards remain open. |
| System11 | `system11.c`; C76/M37710, shared RAM, C352, keycus and EEPROM | No profile loader; authentic programs and variant validation remain open. |
| System12 | `system12.c`; H8, C352, shared RAM, EEPROM, RTC, banks and ROM DMA | Soul Calibur Asia C has guarded Mac/Vita3K DRC/NEON replays. Other variants, timing equivalence, physical Vita and vitaGL remain open. |

The Mac `HarnessBoard` table already groups load/unload/sync/input/report and
some persistence/audio functions. Libretro has separate System12 and G-Net
load/reset/unload flows. The memory bus dispatches to individual active adapters;
device composition alone does not make an adapter reachable from a frontend.

## Scheduling and clock gaps

Mac calls the selected adapter after every `ExecuteBlock`. Libretro executes
the core until its frame boundary; System12 catches up on the two vblank edges,
and G-Net additionally catches up after `Execute` and at mapped accesses.
System12 ignores a sync delta below 256 main cycles and retains it for a later
call. This leaves different device slice sizes and shared-memory observation
points between hosts. System12 shared reads/writes currently do not themselves
force H8 synchronization.

H8 executes its entire slice before C352 generates the interval's samples.
Register writes during that slice therefore precede all samples generated for
it. Coherent aggregate cycle/sample totals do not prove correct write/sample
ordering. A scheduler change must establish the intended causal order before
changing these boundaries; blindly adding synchronization to recursive bus
callbacks is not sufficient.

The source timing layer in `psxcommon.c` selects the PSX cycle frequency during
init/reset. It preserves independent device clocks and fractional conversion;
the [timing document](arcade-source-timing.md) explains the source input-clock
conversion and build options. System12 retains its separate source-timing
switch. Unsupported native families have no source-clock adapter yet.

There is also a PSX SPU clock contract to resolve: dfSound derives sample work
from supplied cycle differences in units of 768, and contains fixed 44.1 kHz
buffer/scheduling assumptions. A different main-CPU clock cannot automatically
be used as the SPU oscillator. Future integration needs explicit timestamp
conversion and output-rate handling, including direct DRC plugin calls, IRQ
deadlines, reset/state epochs, XA and CDDA. This is separate from System12's
88.2 kHz C352 callback, whose aggregate sample count was measured.

Frontend pacing is another clock: Vita video conversion and its display vblank
wait, plus audio queue backpressure, block the emulation producer. The headless
Mac path does not perform the same presentation work. Neither should determine
the emulated oscillator rates.

## Recommendation and review gates

Keep one causal board timeline for the current study. Before considering host
parallelism, define common synchronization points for both frontends: shared
memory, MMIO, DMA and IRQ edges, with device deadlines ordered and fractional
clock remainders preserved. Timestamp sound-register accesses so audio samples
can be generated in their correct interval. Keep reset epochs and whole
instruction overshoot explicit rather than equating instruction count with
cycle count.

A future common board interface can build on `HarnessBoard`: profile selection
before CPU/GPU allocation; bus and DMA routing; reset/unload; input; ordered
advance/deadlines; audio delivery; persistence; and declared state support.
Frontend code should own display/audio queues, while the board owns emulated
time and device state. This is a proposed contract, not an implemented refactor.

For System12, the measured priorities are H8 interpreter/peripheral costs,
display/audio pacing, and Mac/Vita scheduling equivalence. The pending-IRQ
optimization preserves the tested replay and improves the paired Vita3K run
from 16.60 to 32.58 emulated fps. H8 remains 47.5% of post-change diagnostic
elapsed time; GPU list time is an estimated 11.5%. Existing GPU async is therefore
a later candidate with limited scope, not evidence that three cores will reach
60 fps. H8 parallelism would require a separate shared-memory/IRQ design.

Review evidence must distinguish component fixtures, executed synthetic PSX
guests, authentic game replays, cross-frontend event equivalence and physical
hardware validation. A parsed media profile or one successful game cannot
complete a family. For each board variant, retain interpreter/DRC bus and DMA
checks, media/persistence checks, controls, timed IRQ/audio traces and authentic
game evidence against the pinned MAME baseline. Do not report speculative
engineering estimates as measured completion percentages.
