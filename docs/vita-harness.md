# Vita standalone validation

The validation targets are tracked separately:

| Target | Recorded evidence | Remaining gate |
| --- | --- | --- |
| M1 harness, NEON GPU and native ARM64 DRC | Console probe, System 12 4/16 MiB generated guests, and a 12,000-frame Soul Calibur run reaching a Stage 2 fight | DRC/interpreter game-state and timing comparison; broader family coverage |
| Vita ARMv7 DRC and NEON, first in Vita3K | Console pixel/signature probe, System 12 4/16 MiB DRC signatures, and a 12,000-frame Soul Calibur input replay reaching Stage 2 with 23 native screenshots | Game-state/timing comparison, then physical Vita validation |
| Vita ARMv7 DRC and VitaGL, first in Vita3K | Console pixel/signature probe, upper-VRAM fixtures and native title/resource teardown and a 12000-frame Stage 1 input replay; screenshot review fails on horizontal corruption | Fix rendering and intermittent host protection diagnostic, then physical Vita validation |

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
ordered shutdown/shader-cache/readback/finish patch hashes, flags and archive SHA-256 in
`pcsx-build.json`. It rejects any extra edits in its build copy, including
changes outside patch hunks in the same files. It does not install over the
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
process group it launched. A separate watchdog process enforces the deadline
even if the runner stalls, with TERM followed by KILL for that owned group.
Output is streamed into bounded diagnostic capture instead of repeatedly
reading an ever-growing log. Vita3K normally keeps its UI open after the guest
exits; host-process exit is not the completion signal. The runner fails on
guest mismatches, ARM fallbacks, late teardown errors, host crashes or timeout,
and preserves the filtered log and diagnostic counts. For an
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

For a rebuilt VitaGL guest with the shutdown patch, add `--renderer vitagl
--check-lifecycle` to run the same guest and check `cpu_driver=ari64`,
`vitaGL shutdown PASS (0)` and `Stopping -> Idle`. This probe exercises renderer
startup, frame execution and resource teardown without asserting readback
pixels. Run the default pixel probe separately before calling VitaGL graphics
validated; a lifecycle PASS alone is not a graphics PASS.

VitaGL also needs the locally supplied `libshacccg.suprx` compiler module.
Copying only `vs0`/`os0`/`sa0` does not provide it. Add
`--shader-compiler /path/to/local/libshacccg.suprx` to `check_vita3k.py` to copy
that file into the isolated app's sibling portable profile at
`fs/ur0/data/libshacccg.suprx`. The runner records its SHA-256 and refuses to
replace a different existing compiler. It never downloads that module.

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
destroyed successfully with return value zero. A second trace identifies the
release as `ksceKernelFreeMemBlock`, UID 1, base `88000000`, size 128 MiB;
the DRC's 16 MiB VM block is UID 5 and is separate. The packaged guest ELF's
exit path calls `_exit -> _free_vita_newlib -> _free_vita_heap ->
sceKernelFreeMemBlock` before `sceKernelExitProcess`. Thus guest newlib teardown
releases the external heap containing VitaGL's main context before the host
tries to destroy that still-registered context. The resource-lifetime fix is
implemented in an isolated library patch but not yet runtime-verified
(`build/vita-validation-gxm-free-diagnostic3/vitagl-free-trace.log`).
The first fresh lifecycle guest failed earlier during shader startup. Supplying
the existing local compiler allowed a later diagnostic to reach shutdown/Idle,
but host protection diagnostics still reject an overall PASS. The triangle
mismatch also remains unresolved.
New instruction tests run directly in
Dynarmic without leaving an emulator or debugger window open.
The required four library build flags remain enabled. VitaGL is not validated
yet (`/tmp/pcsx-vita3k-vitagl-gxm-transfer-ab.log`).

### Shader startup diagnostic, 2026-10-05

A fresh portable profile reproduced an early `sceClibMemcpy` null-source
fault before `cpu_driver=ari64` or GPU initialization. The optional
`--diagnose-null-memcpy` flag on `rebuild_vita3k_fixed.py` adds a private host
log of guest arguments and PC/LR before the original memcpy call; it does not
skip the call or turn the guest fault into success. That host compiled and
linked successfully, preserving the original Vita3K source and CMake objects.

The bounded lifecycle run used VPK SHA-256
`b71430c66c3c52f7fb3b564b696ee93af572f4e451bbfbfced4a083543d478e0`
and diagnostic host SHA-256
`839a53c4aba6b98ceba9333f7151674bad9667083ca47164b9fddf904509ab07`.
It logged `dst=8822be78`, `guest_src=00000000`, `len=10195`,
`pc=81247cf4`, `lr=811a4eb9`. Mapping the LR against the packaged ELF
identifies `serialize_shader` in VitaGL `custom_shaders.c:692`, copying
`s->prog` with its recorded size. The compiled-program pointer was null;
the shader compilation/cache path serialized failed output. The library now
skips that cache write and rejects linking before attribute/program analysis;
the renderer propagates compile/link failure into initialization failure and
cleans up partial resources. The raw host log also established that the fresh
profile lacked the local `libshacccg.suprx` compiler module.

The watchdog closed all owned processes. The native PNG saved at
`build/vita-validation-gxm-memcpy-diagnostic2/emulator/portable/screenshots/PCSX-ReARMed Vita vitagl/PCSX-ReARMed Vita vitagl_2026-10-05-015114.png`
is black, consistent with failure before rendering; the filtered log is
`/tmp/pcsx-vitagl-memcpy-20261005.log`.

### Compiler supplied and final shutdown diagnostic

The existing local compiler is 3,407,920 bytes with SHA-256
`188adb8c34c3983d63244c32d981fd7b518a8dcfd200f5886233fface6b5fa40`.
It was copied from the user's Vita3K filesystem into this private profile;
no module was downloaded. The final patched archive is
`build/vita-deps/vitaGL-shader-cache-final3/libvitaGL.a`, SHA-256
`76a0cf95c8c16f19ebe2c632c22c407bd723a0720fc8cea5d63ba79d080819f8`.
The linked VPK SHA-256 is
`2102ea666b6fcd13fc771503b287671619cbda1737a14ff3b1f09b7c9732dfd8`.

The bounded debugger diagnostic in
`/tmp/pcsx-vitagl-final3-bounded-lldb-20261005.log` reached GPU initialization,
`cpu_driver=ari64`, `vitaGL shutdown PASS (0)` and `Stopping -> Idle`.
It also recorded 15 Dynarmic `Unhandled EXC_BAD_ACCESS` messages and four
Vita3K critical protection-recovery messages, so it is not an overall
lifecycle PASS. The last native PNG at
`build/vita-validation-gxm-shader-cache/emulator/portable/screenshots/PCSX-ReARMed Vita vitagl/PCSX-ReARMed Vita vitagl_2026-10-05-021239.png`
is black. The pixel probe and extended arcade GPU remain unverified.

LLDB launched Vita3K into a different process group; closing the debugger
group left the child alive. The known child PID was then closed explicitly and
process inspection confirmed no Vita3K/LLDB/debugserver remained. Future
debugger diagnostics must launch the host under its own PID watchdog before
attaching LLDB, so both processes have independent ownership/deadlines.

## VitaGL GPU geometry limits

The VitaGL backend now selects 512 or 1024 VRAM rows before starting its
render thread. CPU mirror/tile/palette capacities cover 1024 rows; texture/FBO
storage, clipping, guest copies, transfers, readback and shader scales use the
active height. Native snapshots retain their 1 MiB console payload and use
2 MiB for 1024 rows. Arcade board savestates remain unavailable.

For type 2, TPAGE bit 11 selects the upper texture bank. Cached P4 keys retain
all six page bits; the vertex shader reconstructs unsigned CLUT IDs from its
signed-short attribute. Nearest texture fetches floor interpolated coordinates
before selecting a texel, avoiding the next empty texel for a 1×1 sprite.
GP0 fills use aligned X/width and wrap their CPU mirror at the active row
boundary. Generic A0/C0/move transfers remain serviced by GPULIB's wrapping
path; private PsyQ CopyVRAM/ReadVRAM helpers are not used for those commands.

The generated ari64 guest passes upper 4/8/odd-8/16-bit texture reads, row-800
CLUTs, separate lower/upper cached P4 pages, and A0/C0/fill wrap from row 1023
to zero. The retained production package is
`build/vita-system12-vitagl/pcsx-rearmed-vita-vitagl.vpk`, SHA256
`25a10718420c436022721ed3699dccdac233b0703558e258e77188605b11df48`.
`runtime-vram-evidence.json` beside it binds the source delta/library/host
hashes and logs. The final upper-VRAM log is
`/tmp/pcsx-vitagl-system12-vram-final-20261005/vita3k.log`: eight paired faults,
zero unresolved, shutdown PASS and Idle. The same arcade-enabled package
passes the 180-frame console probe with ten paired faults and three post-draw
native PNGs showing the triangle/background. Early shader-compilation PNGs
are black and are not used as rendered-frame evidence.

The RGB24 row decoder passes a scalar ASan/UBSan check for distinct upper rows
and 512/1024 vertical wrap (`tests/vita_rgb24_fixture.c`); RGB24 native display
is not yet validated. Type 1 is explicitly rejected: its distinct command
fields and interleaved texture layout still need implementation. These checks
establish the type-2 graphics fixture, not Soul Calibur compatibility or
physical Vita operation.

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
Both NEON and VitaGL use runtime VRAM geometry. VitaGL accepts the System 12
type-2 path after the upper-VRAM fixture above; actual game validation remains
a separate gate. Arcade savestates are explicitly unavailable because the
console state format does not preserve the board's devices.

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

That separate NEON game gate now includes the 12,000-frame input replay through
Stage 2 documented in [system12-vita-gameplay.md](system12-vita-gameplay.md).
It establishes observed match progression in Vita3K, while full board timing
equivalence and physical Vita validation remain pending.

For the direct-memory M1 host protection correction and its real fault
regression, see [host-page-protection.md](../tools/vita-harness/host-page-protection.md).
The production console probe now passes with triangle `03ff`, background
`2082` and the expected sentinel after correcting host synchronization and
the VRAM row address. Preserving the main render target during GPU-pack state
restoration also makes the triangle and background visible in native PNGs.
See [VitaGL readback validation](vitagl-readback.md) for the evidence and the
optional host synchronization correction.

## Varied upper-bank texture probe

`check_system12_vram_vita3k.py --texture-pattern` adds 48 GP0/C0 readbacks to
its legacy checks: every P4 palette index and eight asymmetric P8 byte indices,
spread over eight upper-bank texture rows with nonzero CLUT X on row 800.
It then uploads changed texture and palette contents and repeats the samples.
P4 and P8 upload words are packed at their actual nibble/byte widths; the marker
`53315458` versions the appended RAM results. The standalone frontend requires
FNV signature `fec74789`; the default mode retains its original ABI/output.
This tests spatial data and cache refresh, not triangle interpolation or a
complete game framebuffer.

The native ARMv7/ari64 runs pass for both NEON and VitaGL in private host
`f996dfab1d9d0712e2fe51fcf93602a477655bf70e7bd45ac021fc115b513855`, with the
stale-fault patch. VitaGL VPK
`2c4d1724b1ecadc7c190da5b385e5a9c5dde2359b32ce68ff0eda05320482115`
passes all legacy checks and 48 samples, eight paired fault recoveries, shutdown
PASS and Idle. Log: `/tmp/pcsx-vitagl-texture-pattern-20261005/vita3k.log`.
The NEON VPK
`ecea6ea3a9552bbdde3a118799a165c7b4de3f7f3b5eddb0f8f359faeadc9083`
passes the same generated guest and reaches Idle; log:
`/tmp/pcsx-neon-texture-pattern-20261005/vita3k.log`.
Frozen packages and `texture-pattern-evidence.json` are retained under
`build/vita-system12-vitagl-teardown` and `build/vita-system12`.
