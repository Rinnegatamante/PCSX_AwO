# Vita standalone validation

The validation targets are tracked separately:

| Target | Recorded evidence | Remaining gate |
| --- | --- | --- |
| M1 harness, NEON GPU and native ARM64 DRC | Console probe, System 12 4/16 MiB generated guests, and a 12,000-frame Soul Calibur run reaching a Stage 2 fight | DRC/interpreter game-state and timing comparison; broader family coverage |
| Vita ARMv7 DRC and NEON, first in Vita3K | Console pixel/signature probe and System 12 4/16 MiB bank, SMC, DMA and MMIO signatures pass | Actual Soul Calibur captures and board progress, then physical Vita validation |
| Vita ARMv7 DRC and VitaGL, first in Vita3K | Required library build and packaging; GXM transfer readback recovers the background pixel | Host shutdown fault, triangle probe, extended arcade VRAM, actual game, then physical Vita validation |

The System 12 ARMv7 results and isolated runner are documented in
[system12-vita-drc.md](system12-vita-drc.md). Neither an emulator result nor a
successful package build establishes physical Vita compatibility.

The two builds select `VITA_GPU=neon` or `VITA_GPU=vitagl` and have separate
archives, frontend objects, VPKs and title IDs (`PCSXN0012` / `PCSXG0012`).
The NEON build uses the GPU's C SIMD implementation with ARM NEON intrinsics;
the GTE retains its ARM/NEON assembly and the CPU retains the `ari64` DRC.
The old GPU assembly does not describe the extended arcade GPU structure.

VitaGL is built in an isolated local checkout, using exactly:

```sh
HAVE_SHADER_CACHE=1 NO_DEBUG=1 READBACKS_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1
```

`tools/vita-harness/build_vitagl.py` clones the clean local `../vitaGL` source
into `build/vita-deps/vitaGL`, builds there, and records the source commit,
flags and archive SHA-256 in `pcsx-build.json`. It does not install over the
SDK library. The standalone build calls the helper and uses that checkout's
header and archive. A matching recorded archive can be reused. To build:

```sh
git submodule update --init deps/libretro-common deps/libchdr
make -f Makefile.mac-harness smoke
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
make -f Makefile.vita VITA_GPU=neon SMOKE_EXE=build/mac-harness/smoke.exe
make -f Makefile.vita VITA_GPU=vitagl SMOKE_EXE=build/mac-harness/smoke.exe
```

The core uses shared object paths, so build the two renderer variants
sequentially; the standalone Makefile forces the selected core to rebuild.
`VITAGL_DIR` can select another isolated helper build directory. The renderer's
local interfaces are now in `plugins/gpu_vita_ctr/include`, with provenance
and ABI notes in that directory's README.

The standalone accepts an executable path, `--frames N` (1–216000), and
`--verify-smoke`. The latter reads two GPU pixels through the emulated MMIO
interface and a RAM sentinel written by the generated guest. It requires the
DRC to be active and checks triangle `03ff`, background `2082` and sentinel
`534d4f4b`; any mismatch returns 5. These private probes do not expose the
core's internal state layout through the frontend ABI.

First validate in the local `../vita3k`, as requested. The local validation
used a copied Vita3K application with its own portable filesystem, config,
logs and installed test titles, so another running emulator task could
continue. Existing local firmware was read from that installation; no firmware
or ROM was downloaded.

`tools/vita-harness/check_vita3k.py` automates a bounded smoke run, requires the
exact PASS and the guest's `Stopping -> Idle` state, then terminates only the
host process it launched. Vita3K normally keeps its UI open after the guest
exits; host-process exit is not the completion signal. The runner fails on
guest mismatches, host crashes or timeout, and preserves the log. For an
already installed test package in an isolated emulator copy:

```sh
python3 tools/vita-harness/check_vita3k.py \
  --emulator build/vita-validation/emulator/Vita3K.app/Contents/MacOS/Vita3K \
  --renderer neon --log /tmp/pcsx-vita3k-neon-check.log
```

Add `--vpk PATH` to install and launch a package instead. The runner waits for
the matching installation result, closes its installer process, then launches
the installed title explicitly. This local emulator does not reliably
auto-boot a CLI-installed VPK. An absolute VPK path is placed after `--` so
the emulator's Windows-style option parser accepts it on macOS. Installation
output cannot satisfy the subsequent guest completion check.

Observed NEON results on the local M1/Vita3K build: three consecutive runs
passed with `cpu=ari64`, the expected pixels and RAM sentinel, and the guest's
`Stopping -> Idle` state. The runner then closed its owned host process in
all three runs (`/tmp/pcsx-vita3k-neon-final-{1,2,3}.log`). These runs used
`build/vita-validation/emulator/Vita3K-fixed.app`, an isolated emulator copy
with teardown and display-buffer lifetime fixes. Earlier runs on the original
host build reproduced `EXC_BAD_ACCESS` after a successful guest probe and
correctly failed overall; the guest PASS alone is insufficient. The original
emulator checkout, binary and other running instance were not modified.

The host patch and incremental rebuild recipe are checked in under
`tools/vita-harness`. To reproduce the validation app from an existing,
compatible macOS Vita3K CMake build:

```sh
python3 tools/vita-harness/rebuild_vita3k_fixed.py \
  --source ../vita3k \
  --app-template build/vita-validation/emulator/Vita3K-fixed.app \
  --output build/vita-validation-repro
```

The output directory must be fresh. The helper copies the template bundle,
applies the patches to private source copies, recompiles six translation units,
and links against untouched objects from the existing build. It records input
source, patch and executable hashes in `pcsx-host-build.json`. This is an
incremental build tied to those local build artifacts, not a clean upstream
build. Give its app a separate portable profile before running it. The recipe
was rebuilt and its NEON console probe passed with the expected signature,
`Stopping -> Idle`, and owned-process closure in
`/tmp/pcsx-vita3k-host-recipe-console-1.log`.

The actual System 12 game exposed a missing A32 `VADDHN` instruction in the
local host's Dynarmic decoder. This is the emulator executing the Vita's ARM
code; the Vita core still uses ari64 for its guest MIPS CPU. Unsupported
`vaddhn.i32` operations produced corrupted NEON textures and repeated host
diagnostics, so the console probe alone did not detect this failure.
The checked-in patch adds the narrowing high-half add. Its direct JIT test
checks 16/32/64-bit source lanes, modular sums, high registers, sibling
preservation, source/destination aliasing, and the opcode observed in the game.

To rebuild and test that dependency, then link it into a fresh private host:

```sh
python3 tools/vita-harness/build_dynarmic_vaddhn.py
python3 tools/vita-harness/rebuild_vita3k_fixed.py \
  --source ../vita3k \
  --app-template build/vita-validation-repro/emulator/Vita3K-fixed.app \
  --dynarmic-archive build/dynarmic-vaddhn/libdynarmic.a \
  --output build/vita-validation-neon-fix
```

The Dynarmic helper reconstructs the three frontend files from the dependency's
committed base and applies the patch in a private staging directory. It
preserves the configured build's other backend changes, refuses unexpected
edits to those frontend files, builds only the Dynarmic target, and runs the
test with a 15-second timeout. Its manifest records the dependency revision
and source, patch and tested archive hashes. The host recipe records the linked
archive hash separately. This instruction test does not establish game
compatibility; actual captures are described in
[system12-vita-gameplay.md](system12-vita-gameplay.md).

The host also needs the checked-in Vulkan CPU-framebuffer screenshot patch.
Its original `dump_frame` only looked for GXM render surfaces, while the NEON
frontend presents pixels written directly into guest display memory. The
fallback copies that framebuffer with its stride and native dimensions,
checking guest memory and holding the display-buffer lifetime lock. The host
recipe applies it and records its hash. This enables Vita3K's existing native
screenshot function for NEON; the core's PPM probe remains separate evidence.

The console probe does not validate Soul Calibur
on Vita, the VitaGL renderer, sound
quality, performance or physical hardware. System 12 integration and both
renderer validations in Vita3K precede requesting a real Vita.

VitaGL currently fails the same guest pixel probe. Initially both pixels read
as zero while the ARMv7 DRC wrote the correct RAM sentinel. Its initialization API
returns whether the requested display resolution was reduced, rather than a
success flag; the adapter now follows that contract and reaches renderer
initialization. An explicit `glFinish()` before synchronous VRAM readback
alone did not resolve the pixel failure. A controlled GXM transfer readback
through `vglReadPixels` recovered the background value `2082` and preserved
the CPU mirror instead of replacing it with zero. The backend now uses that
transfer path with the explicit fence. The triangle still reads the background
color instead of `03ff`. An initial debugger stop while the guest's
`vitaGL Splashscreen` thread freed its vertex uniform ring was subsequently
identified as an expected protected-memory read, rather than heap corruption.
LLDB had stopped without passing SIGBUS to Vita3K's memory handler. Passing
the signal lets that handler restore access and the guest continues. A
separate fatal `Unhandled access` still occurs during host GXM-context cleanup;
its stack reaches `gxm::destroy_all_contexts`. A bounded instrumented run
identified the main context at guest address `8821c7e8`: it was valid when
created, but no longer allocated and not covered by a protection region before
cleanup dereferenced it. The splash context at `8821d5d8` had already been
destroyed successfully with return value zero. The main context's backing-memory
lifetime remains unresolved (`build/vita-validation-gxm-diagnostic/vitagl-run2.log`).
That shutdown fault and the
triangle mismatch remain unresolved. New instruction tests run directly in
Dynarmic without leaving an emulator or debugger window open.
The required four library build flags remain enabled. VitaGL is not validated
yet (`/tmp/pcsx-vita3k-vitagl-gxm-transfer-ab.log`).

## VitaGL GPU geometry limits

The current VitaGL backend is fixed to the console GPU's 1024x512 VRAM. Its
CPU mirror, transfer scratch space, dirty-tile tracking, GL texture and
framebuffer, shader UV scale, GP0 draw-area Y fields, display conversion and
save-state snapshot all use that geometry. Guest VRAM copies also initialize
only 1024x512 pixels. The plugin does not consult the shared GPU model or VRAM
height selector, so upper rows of a 2 MiB GPU profile are not represented.

Supporting 2 MiB VRAM requires runtime height through the renderer, cache,
readback, guest-copy, display, and save-state paths. Type 1 additionally needs
its distinct TPAGE field decoding, draw-area and offset semantics, and
interleaved texture layout. The current VitaGL plugin only implements its
console/type-2 command interpretation. A successful console VitaGL smoke test
does not establish either arcade GPU model's support.

## Opt-in libretro System 12

The libretro build can include the experimental System 12 path with
`ARCADE_SYSTEM12=1`. Load a prepared region directory as content; this selects
its PSX memory geometry before reinitializing the already-started core, keeps
the configured DRC backend, routes board inputs, and advertises the C352
88.2 kHz stream. Console builds remain the default when the flag is omitted.
The standalone frontend opens a 48 kHz Vita output device and sets the SDL
stream's source format to the core's advertised rate. A System 12 run confirmed
`src=88200Hz dst=48000Hz device=48000Hz`; opening the device directly at 88.2 kHz
previously failed with `SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ`. This checks
stream initialization and conversion setup, not audible game sound quality.
The current Vita `vita_ctr` renderer is fixed at 1024x512, so that renderer's
build rejects System 12 profiles before changing core memory or opening
plugins. NEON uses the C GPU path with runtime VRAM height. This opt-in path
is integration work and does not claim System 12 game validation or Vita
2 MiB GPU runtime validation. Arcade savestates are explicitly unavailable
because the console state format does not preserve the board's devices.

Build the standalone opt-in with `make -f Makefile.vita VITA_GPU=neon
ARCADE_SYSTEM12=1`. It uses a separate core archive and frontend object
directory. Launch it with `--system12 ux0:data/pcsx-rearmed/REGION_DIR` to pass
the prepared region directory directly to the core. The frontend reads the
AV sample rate after content loading and opens its SDL input stream at
88.2 kHz for System 12, or 44.1 kHz for console executables. The console
`--verify-smoke` probe cannot be used for a System 12 game.

The full NEON opt-in SDK build linked and packaged successfully. Its console
smoke guest passed in the isolated fixed Vita3K host with `cpu=ari64`, both
expected pixels, the RAM sentinel and `Stopping -> Idle`; the runner closed
its own host process (`/tmp/pcsx-vita3k-neon-system12-build-console-smoke.log`).
That checks console execution in the arcade-enabled binary. The separate
System 12 generated guests now pass on its ARMv7 DRC for both RAM profiles;
actual game validation remains a separate gate.
