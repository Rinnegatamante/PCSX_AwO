# Native macOS ARM64 harness

This dependency-light frontend runs PCSX ReARMed's R3000A interpreter and the
existing GPU_NEON rasterizer on Apple Silicon. The rasterizer's C SIMD backend
uses ARM NEON intrinsics and produces ARM64 Advanced SIMD instructions; it does
not use the 32-bit ARM assembly backend. Sound emulation uses dfsound with its
null output driver. Install Xcode command-line tools and Python 3. CHD support
uses the repository's pinned `libchdr` submodule; initialize it before building.
No SDL, libretro frontend or BIOS download is required.

From the repository root:

```sh
git submodule update --init deps/libchdr
make -f Makefile.mac-harness -j8
make -f Makefile.mac-harness smoke
make -f Makefile.mac-harness gpu-type1-test board-test system11-test devices-test
build/mac-harness/pcsx-harness --exe build/mac-harness/smoke.exe \
  --frames 60 --output build/mac-harness/frame.ppm --verify-smoke
```

`HAVE_CHD=0` builds without the CHD reader and without that submodule. Changing
the option automatically rebuilds objects. The default CHD build uses system
zlib and libchdr's pinned LZMA, FLAC, Huffman and Zstandard decoders, with raw
CD sectors, subcode and block CRC verification enabled on ARM64.
`devices-test` includes [real generated CHD HDD tests](../../docs/chd-hd-backing.md)
when CHD is enabled; that test requires MAME's `chdman` utility. The native
arcade disk backing is available as a component; mounted SCSI board execution
remains pending.

`gpu-type1-test` checks both 1024-row and 512-row type-1 VRAM geometries.
Run `build/mac-harness/pcsx-harness --gpu-type1-test-1m` for the 1 MiB case
alone. Both cases check command fields and fixed MAME-derived interleaved
texture vectors; the diagnostic restores its VRAM pixels and resets GPU
registers before a board guest runs.

`BOARD=0` builds only the ordinary PS-X EXE path. The default `BOARD=1` includes
the experimental System 11 and System 12 adapters and type-1 GPU diagnostics. Changing this option
automatically rebuilds objects. The separate build directory and flags do not
change `config.mak` or existing frontend builds.

The synthetic smoke executable is generated locally from 63 MIPS instructions.
It writes normal GPU MMIO commands to enable a 320×240 display, clear a blue
background, draw a yellow triangle, and write a RAM sentinel. `--verify-smoke`
checks two exact BGR555 pixels and that sentinel. These checks verify CPU
execution, memory-mapped GPU access, command dispatch, and rasterization.

To boot an assembled System 12 ROM region directory:

```sh
build/mac-harness/pcsx-harness --system12 artifacts/system12 \
  --board-test --frames 1200 --output artifacts/system12/frame.ppm \
  --trace artifacts/system12/trace.csv --trace-blocks 1000
```

The region directory is prepared by the System 12 ROM tooling and contains
`main.bin`, `banked.bin`, `sub.bin`, and `wave.bin`. The board test runs after
core reset, before CPU execution. Board initialization precedes `psxInit()`;
the board module owns reset behavior and the memory map. Its final report is
printed after CPU execution. Missing H8 execution or board peripherals are
reported by that module; a completed harness run does not imply a working game.

System 11 directories prepared with `scripts/prepare_system11_board.py` use
`--system11` and include `c76.bin` plus the separate verified `c76bios.bin`.
The loader selects the registered COH-100/110 profile, 4 MiB RAM and GPU1/2,
and maps all eight executable bank windows. `system11-test` runs generated
MIPS and C76 ROMs, checks guest execution from each bank, input/ADC forwarding
and instruction-programmed nonzero C352 audio. Real System 11 game boot is
not yet verified.

JAMMA inputs can be scheduled by emulated frame with `--inputs FILE.csv`.
Frames are zero-based from the start of the requested run; controls stay held
until another row changes them. The ordered CSV has `frame,control,pressed`
columns, where `pressed` is `1` or `0`. Accepted controls are `coin1`, `coin2`,
`service`, `test`, and `p1`/`p2` plus `start`, `up`, `down`, `left`, `right`, or
`button1` through `button8`. System 11 also accepts `coin3`/`coin4`, player
3/4 controls and `dip1_test`/`dip2_freeze` where its specific MAME input layout
defines them. Its extra Tekken/Soul Edge buttons use the real ADC/PLAYER4
wiring. Undefined controls for a board fail explicitly. Raw paddle, pedal
and gun coordinates retain the MAME defaults; the CSV exposes boolean controls.
For example:

```csv
frame,control,pressed
2000,coin1,1
2020,coin1,0
2100,p1start,1
2120,p1start,0
```

Omitting `--inputs` leaves all controls released. On boards using the JAMMA
interface, the harness maps JVS-style controls to the four active-low bytes at
H8 sub-bus address `0x300000` using MAME's `in_r` wiring.

`--audio FILE.wav` records the board C352's generated front-left/front-right
mix as stereo PCM16 at 88.2 kHz. The WAV header is finalized after emulation;
this is captured C352 output and does not use the console SPU. For example:

```sh
build/mac-harness/pcsx-harness --system12 artifacts/system12 --frames 3600 \
  --inputs artifacts/system12/controls.csv --audio artifacts/system12/run.wav \
  --output artifacts/system12/run.ppm
```

For board-test validation only, add `--audio-test-fixture` to play a synthetic
C352 sample into the WAV; it requires both `--board-test` and `--audio` and is
never enabled for normal game runs.

`--nvram FILE` imports an exact 2080-byte AT28C16 image (2048 data bytes plus
32 ID bytes) before execution on System 11/12, Capcom ZN and Taito FX1S. A missing file starts with the
device's erased state. A successful run writes the image via an atomic rename;
faults, malformed images and interrupted runs leave an existing file intact.
The image stores persistent cells, not pending write timers or a save state.
Device resets preserve EEPROM data and an in-flight 200 microsecond write.

`--frames` accepts 1 through 216000 vsyncs for sustained gameplay captures.
Execution is limited by the requested emulated vsync count, with cycle and block
caps for boots that stop producing vsync. CPU quanta end at a taken branch or
4096 calls to the core's public interpreter step; this keeps branchless code
bounded while preserving branch and load-delay semantics. SIGINT and SIGTERM
stop between steps. The optional `--max-blocks N` defaults to one billion and
accepts up to 100 billion for long replays. Input CSV frames accept the same
216000-frame upper bound as `--frames`.
The harness prints frame, cycle, block, PC, flip and wall-time counters.
`--icache` enables the existing PCSX interpreter instruction-cache model;
it is off by default. System 10 tests also execute banked code with this enabled.

System 10 base MEM(M)/MEM(N) packages use `--system10 REGION-DIR` with the
[source-derived media preparer](../../docs/system10-boards.md). The native
adapter runs with 4 MiB RAM, 2 MiB VRAM, dynamic bank fetches and NAND DMA5.
`make -f Makefile.mac-harness system10-test` executes generated MIPS guests
for both board types, checks input/GPU/IRQ/DMA behavior, and rejects malformed
or unsupported profiles before execution. These fixtures do not establish
authentic game boot. See [adapter limitations](../../docs/system10-native.md).
Capcom ZN packages use `--capcom-zn REGION-DIR` with the
[media preparer](../../docs/capcom-zn-boards.md). The
[native adapter](../../docs/capcom-zn-native.md) supports source-selected
COH1000C, COH1002C and COH3002C geometries. Its `--audio` capture is QSound
stereo at 24,038 Hz. `make -f Makefile.mac-harness capcom-zn-test` executes
generated MIPS/Z80 guests for banks, SIO/MCU/IRQ, inputs, EEPROM, GPU and sound,
including instruction-cache and persistent EEPROM checks. Authentic Capcom
game boot and mixing the mainboard SPU remain unverified.

Taito FX1S packages use `--taito-fx1 REGION-DIR` with the
[media preparer](../../docs/taito-fx1-boards.md). The
[native adapter](../../docs/taito-fx1-native.md) runs COH1000T with 4 MiB RAM,
1 MiB VRAM, Z80, YM2610B and the TC0140SYT sound link. Its `--audio` capture
is stereo PCM16 at 55,555 Hz. `make -f Makefile.mac-harness taito-fx1-test`
executes generated MIPS/Z80 guests for bank fetches with and without instruction
cache, RAM, GPU, serial MCU/IRQ, sound packets, inputs and EEPROM persistence.
These fixtures do not establish authentic Taito game boot or mainboard SPU mixing.

The PPM saves a snapshot of the last rendered video flip, using its display
dimensions and framebuffer origin and converting BGR555 or packed RGB24 pixels
to RGB888 inside the callback. This preserves a completed frame even when the
CPU stops midway through clearing or drawing the next frame. Blank callbacks
retain the previous rendered snapshot. If no rendered frame was captured, the
harness falls back to current VRAM. The capture log records the source, size,
origin, format and current display-enable state. It does not scale or deinterlace
the output. The smoke stays at 320×240 and origin (0, 0).

`--trace FILE --trace-blocks N` writes a CSV containing each initial CPU block's
cycle, PC, opcode and 32 general registers. It defaults to 1000 trace rows
and supports up to one million; execution continues after tracing finishes.

For System12, `--cycle-snapshots FILE --snapshot-start CYCLES
--snapshot-interval N` records CPU registers, interrupt state, the RAM exception
vector and Soul Calibur's copy-loop instruction words at `800d2190..800d21dc`.
It stops recording after 200,000 snapshots. DRC PCs/opcodes are saved block
state and may be stale; these samples are not instruction-exact checkpoints.
`--board-bus-trace FILE` records event counts and FNV-1a hashes in 8,192-cycle
bins for board MMIO and banked-ROM fallback reads. It omits declared no-op
writes and stops after 819,200,000 elapsed cycles. Standard PSX hardware/DMA
register accesses are outside this hook. Both options are disabled by default.

Check the executable architecture and the rasterizer instructions with:

```sh
file build/mac-harness/pcsx-harness
xcrun llvm-objdump --disassemble \
  build/mac-harness/plugins/gpu_neon/psx_gpu/psx_gpu_simd.o
```

Vector instructions such as `smull.4s`, `smlsl.4s`, `add.4s`, and `abs.4s`
demonstrate that the compiled rasterizer uses NEON. The frontend also refuses to
compile unless Apple ARM64 and ARM NEON are enabled.

The native framebuffer comparison executes the Vita probe's GP0 stream on the
Mac core. It adds only terminal RAM-to-register loads so cycle snapshots expose
the eight transfer readbacks and ten interpolated FT3/FT4 pixels:

```sh
python3 tools/mac-harness/check_framebuffer_guest.py \
  --harness build/mac-harness/pcsx-harness \
  --output-dir /tmp/pcsx-mac-framebuffer-check
```

The output directory must be empty. It retains generated board regions, the
expected register oracle, snapshots, a framebuffer, the harness log and binary,
guest, command-stream and capture hashes. Failure and timeout evidence is kept.
The same command accepts another native harness binary for a renderer
comparison; the recorded host summary identifies what actually ran. The NEON
interpreter passes all eighteen pixels in
`/tmp/pcsx-mac-framebuffer-runner-neon-fixed-20261005`. This probe does not
establish whole-game graphics compatibility.

An experimental `GPU=vitagl-like` build compiles the Vita-path GPU parser,
batching, shader selection and VRAM code against macOS OpenGL, using a small
SDL/OpenGL compatibility shim for VitaGL entry points. It keeps its objects and
binary in `build/mac-harness-vitagl-like/`, so it can be compared with the
default NEON harness without changing that build:

```sh
make -f Makefile.mac-harness GPU=vitagl-like all
python3 tools/mac-harness/check_framebuffer_guest.py \
  --harness build/mac-harness-vitagl-like/pcsx-harness \
  --output-dir /tmp/pcsx-mac-framebuffer-vitagl-like-check
```

The Vita-path GL build currently matches all eighteen framebuffer probe
readbacks; one run is recorded in
`/tmp/pcsx-mac-framebuffer-vitagl-like-current-20261005`. The probe validates
GPU readback and rasterization, while the harness PPM may still come from its
CPU VRAM fallback when no frontend presentation occurs. Treat it as a
readback comparison, not a captured GL presentation or whole-game result.

Add `--capture-gl` to capture the final emulated frame from OpenGL's default
back buffer immediately before swap. It is accepted only by the
`GPU=vitagl-like` build and fails if that frame is never presented. A 3,600
frame Soul Calibur capture using the scheduled gameplay inputs is recorded in
`/tmp/pcsx-mac-vitagl-like-soul-3600-capture-20261005`; its log confirms
`source=OpenGL-back-buffer-before-swap` and the PPM is 1920×1088. This captures
the Mac renderer output and is not a Vita hardware screenshot.

`--dump-vram FILE` exports the complete synchronized VRAM after CPU execution
ends. The binary begins with eight bytes `PSXVRAM1`, then little-endian 32-bit
width and height, followed by row-major little-endian 16-bit PSX pixels.
Width is 1024; height follows the selected GPU profile. The native GL path
resolves pending targets and GPU-newer tiles before reading its CPU mirror;
NEON flushes its rasterizer. The dump retains the mask/STP bit. It is an
end-of-execution diagnostic snapshot, which may include drawing after the
last displayed frame; it is not the same capture as `--capture-gl` or the
NEON last-rendered flip. Use a separate path from other inputs and outputs.

Add `--texture-window` for six further reads after setting GP0 E2 mask/offset
and resetting E2. Mac NEON passes this 24-pixel probe in
`/tmp/pcsx-mac-framebuffer-e2-neon-20261005`; Vita NEON also passes its native
version. The Vita-path GL compatibility build passes all 24 readbacks in
`/tmp/pcsx-mac-framebuffer-vitagl-like-final-e2-20261005`. Its shaders apply
the E2 mask and offset to indexed and direct-color PSX textures; Vita hardware
validation is still pending. This proves the native backend had omitted
texture-window sampling in the tested render path, but does not establish that
Soul Calibur's remaining artifacts use E2.

`--vram-x-wrap` separately checks the texture-page X seam and P8 CLUT carry.
A palette at (1008,400) with index 20 selects (4,401), since palette entries
are consecutive VRAM words. The original native shader wrapped X to the same
row and failed the two index-20 samples, while NEON passed. The shared native
shader now carries the overflow into Y; all nine samples pass in
`/tmp/pcsx-mac-palette-carry-seam-fixed-20261005`. The 24-read E2 control and
15-read sprite UV-wrap control also pass with that binary. These are Mac GPU
readback results; native Vita shader compilation and execution still require
separate validation.

`--coord-alias` draws FT3, FT4 and sprites with canonical coordinates and
alternate 16-bit encodings of the same signed 11-bit coordinates. NEON passes
all nine samples; the preserved original GL binary fails the six alias cases.
The GP0 bridge now sign-extends only geometry XY words before native packet
parsing, preserving UVs, colors and transfer fields. All nine reads pass in
`/tmp/pcsx-mac-gp0-coords-alias-fixed-20261005`. A guarded 3,600-frame source-
timing Mac GL replay also removes the vertical sky bands after this change;
a flat blue lake area remains in both synchronized VRAM and the GL capture
(`/tmp/pcsx-mac-vitagl-like-fixed-source-timing-vram-20261005`). This narrows
the remaining issue to submitted geometry/textures or rendered VRAM rather
than presentation capture. Native Vita verification remains separate.
