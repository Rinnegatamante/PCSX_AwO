# Vita standalone validation

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
`VITAGL_DIR` can select another isolated helper build directory. Currently the
VitaGL renderer still needs its missing `ctr-native` header dependencies
integrated before its VPK can be built.

The standalone accepts an executable path, `--frames N` (1–216000), and
`--verify-smoke`. The latter reads two GPU pixels through the emulated MMIO
interface and a RAM sentinel written by the generated guest. It requires the
DRC to be active and checks triangle `03ff`, background `2082` and sentinel
`534d4f4b`; any mismatch returns 5. These private probes do not expose the
core's internal state layout through the frontend ABI.

First validate in the local `../vita3k`, as requested. For example, launch the
NEON VPK with the emulator's comma-space argument format:

```sh
../vita3k/build/macos/bin/Vita3K.app/Contents/MacOS/Vita3K \
  --keep-config pcsx-rearmed-vita-neon.vpk \
  --app-args 'pcsx-vita, ux0:app/PCSXN0012/smoke.exe, --frames, 3, --verify-smoke'
```

Use `PCSXG0012` and the VitaGL VPK for the other renderer. The local validation
used a copied Vita3K application with its own portable filesystem, config,
logs and installed test titles, so another running emulator task could
continue. Existing local firmware was read from that installation; no firmware
or ROM was downloaded.

Observed NEON results on the local M1/Vita3K build: the generated guest passed
with `cpu=ari64` and the expected pixels and RAM sentinel. An initial run hit a
host `EXC_BAD_ACCESS` during Vita3K application shutdown after the PASS. A
subsequent run linked to the required isolated VitaGL archive passed and
reached the emulator's `Stopping -> Idle` state. Shutdown stability still
needs checking in the longer runs.

This is a console executable bring-up test. It does not validate Soul Calibur
on Vita, extended arcade RAM/banking in the DRC, the VitaGL renderer, sound
quality, performance or physical hardware. System 12 integration and both
renderer validations in Vita3K precede requesting a real Vita.
