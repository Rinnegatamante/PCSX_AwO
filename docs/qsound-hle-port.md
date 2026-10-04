# QSound HLE component

`libpcsxcore/arcade/qsound_hle.cpp` ports the full algorithm and state machine from pinned MAME `src/devices/sound/qsoundhle.cpp/.h`, commit `deb2f54fe7300e7a192690318beb860159bfd52a`. Copyright superctr and Valley Bell, BSD-3-Clause. Sample ROM mapping follows `src/emu/dirom.ipp`, copyright Olivier Galibert. `qsound_hle_LICENSE.txt` retains the complete notice.

This is MAME's **default** QSound selection: pinned `qsound.h` aliases `QSOUND` to HLE unless `QSOUND_LLE` is defined. The separate `qsound.cpp` wrapper implements the optional DSP16A LLE path. HLE runs the actual upstream algorithm derived from disassembled DSP firmware, rather than executing DSP instructions. This port implements all 16 PCM voices, three ADPCM voices, coefficient-based stereo panning, echo, both filter modes, wet/dry delays and firmware-derived initialization/refresh/normal states.

The firmware/coefficient ROM is mandatory caller input: exactly 0x1000 logical 16-bit words, copied on creation. It is the same logical `dl-1425` word image used by LLE. MAME HLE loads little-endian words without swapping; LLE loads a big-endian region with pair swapping. Both yield the same logical words from the original little-endian-byte file. Sample bytes are immutable caller-owned memory and must remain available until destruction. No replacement coefficient tables or production media are included. Synthetic coefficient images appear only in tests.

## Build and API

Build the single C++17 translation unit `qsound_hle.cpp`; there is no DSP16 engine dependency. The public header is C99-compatible and has C linkage.

```sh
make -f tests/Makefile.qsound_hle test
make -f tests/Makefile.qsound_hle test BUILD=build/qsound-hle-sanitizers \
  CXXFLAGS='-O0 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
```

`qsound_hle_create` accepts firmware words, sample bytes/length, crystal clock and optional PCM/diagnostic callbacks. Zero clock chooses 60 MHz; invalid/missing media or a clock below 2,496 Hz fails creation.

The Capcom ZN caller subtracts `0xd000` before writing offsets 0–2. Offset 0 sets the high data byte; offset 1 sets the low byte; offset 2 writes the 16-bit latch to the actual upstream command-register map. `qsound_hle_read` supplies the status at `0xd007`.

Every command clears ready, including an unmapped/unused command. Normal sample updates set ready to 0x80. Boot/reset starts busy, follows initialization over three stream frames, refreshes filters on the fourth, and becomes ready on the fifth normal update. There is no readiness timeout, fabricated DSP acknowledgment or invented command queue. This differs intentionally from the LLE component, whose ready transition comes from a real DSP PIO read.

The caller advances emulated sound time before host writes/status reads. That replaces upstream `m_stream->update()` calls and preserves the order of register mutations versus sample generation. Writes during busy update the same register map, as MAME does.

`qsound_hle_read_register` inspects the actual mapped 16-bit command value without changing readiness; unused addresses return -1. The register map preserves upstream's bank quirk: PCM bank register `i*8` writes voice `(i+1)%16`. Pan, voice echo, ADPCM trigger and DSP-state commands use the original addresses. No board-level command interpretation is added.

## Timing and samples

`qsound_hle_run_cycles` accepts nominal DSP machine cycles, each representing two crystal clocks. It generates HLE sample updates at the exact integer MAME stream rate `floor(crystal_hz / 2496)`, 24,038 Hz at 60 MHz. A rational phase accumulator preserves this rate and gives identical output for whole or single-cycle schedules. `qsound_hle_run_clocks` retains an odd crystal-clock remainder. HLE does not expose an instruction PC or pretend to execute a CPU.

`qsound_hle_run_frames` directly executes a requested number of stream samples for offline rendering/testing. It advances frame/state counters, not emulated cycle time or its phase accumulator; choose this interface or timed execution according to the caller's purpose. `qsound_hle_set_clock` is an adapter facility that changes stream rate while retaining fractional phase. It does not simulate a DSP divider register.

Each sample invokes `pcm` with signed left/right values and a cumulative frame index. The source DSP rounding and final range [-32,767, 32,767] are retained. The 16 PCM voices run each stream frame; one ADPCM voice is updated per frame with the original six-frame top/bottom nibble schedule. Pan/ADPCM/filter constants are read from the supplied ROM with upstream 12-bit word-address masking.

Samples are eight-bit ROM bytes shifted into the high eight bits of the signed 16-bit sample. Addresses are masked to MAME's 24-bit sample space. Sub-16-MiB regions mirror a rounded power-of-two aperture; holes return the default unmapped zero. Larger regions use 16 MiB windows, with `qsound_hle_set_sample_region_bank` selecting modulo the number of complete windows. There is no invented full-width 16-bit sample format.

Reset reproduces upstream selective reset: output zero, busy, boot state and state-counter zero. The data latch and voice/effect registers are retained until the next initialization sample clears/reinitializes them. Debug frame/cycle/command counts and timing phase remain cumulative. Status reports state/next-state, readiness, outputs, voice outputs, filter addresses/tap counts, echo length, clocks/rate and counters. It is not a complete serialized save state.

## Verification and source behavior

Normal and ASan/UBSan builds pass. Tests use explicitly synthetic caller-supplied coefficient/sample images and cover:

- Exact boot/filter/readiness transitions, command-register map, unused commands, reset retention and bank-to-next-voice mapping.
- Known nonzero stereo PCM, phase fractions, signed addressing, looping, sample-bank/region-window behavior and DSP rounding.
- 95-tap mode 1 and 45+44-tap mode 2, filter reload states and mode initialization defaults.
- ADPCM playback trigger, independent expected top/bottom nibble values, termination, echo moving average, effect delays and signed echo-length truncation.
- 512 independently calculated stereo phase/delay waveform frames and 256 sparse-FIR convolution frames.
- Whole/single-cycle equality, odd crystal clocks, rate changes, 24,038 output frames per 30 million nominal DSP cycles, and extreme legal register/coefficient values under sanitizers.

Upstream uses signed left shifts and signed accumulator overflow in several fixed-point paths. This port replaces those operations with explicit 32-bit modular shifts/add/subtract and multiplication where the original result fits. It preserves the source's intended fixed-width result while avoiding undefined C++ behavior. Signed narrowing and arithmetic right-shift behavior follow the targeted two's-complement C++ platforms. The source's ADPCM predictor/volume behavior, filter indexing, delayed delay-position changes, signed echo-length truncation and initialization semantics are retained, including their quirks.

No authentic production coefficient ROM/audio oracle was available. Synthetic checks establish algorithm, bus and scheduling behavior, not verified production audio or Capcom ZN boot. The upstream implementation does not model the TC9185P volume-control circuitry; this component does not add it. There is no claim that HLE is a hardware-accurate replacement for the separate DSP16A LLE engine.
