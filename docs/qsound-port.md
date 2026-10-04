# QSound LLE wrapper

`libpcsxcore/arcade/qsound.cpp` implements MAME's real `qsound.cpp` DSP16A device using the standalone DSP16 engine. It preserves command latches and buffering, IRQ/active PIO acknowledgment, delayed sample-bank selection, and serial sample recovery. It requires caller-supplied firmware and sample ROM; it contains no synthesized command handlers or ready timers.

This is the explicit **LLE** path. At pinned MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a`, `qsound.h` aliases `QSOUND` and `qsound_device` to `QSOUND_HLE` unless `QSOUND_LLE` is defined. The default `sony/zn.cpp` configuration therefore selects HLE. The two implementations must remain explicit board choices.

Source: MAME `src/devices/sound/qsound.cpp/.h`, copyright Vas Crabb, BSD-3-Clause. Sample-space mirroring follows `src/emu/dirom.ipp` and default unmapped values from `src/emu/addrmap.cpp`, copyright Olivier Galibert. The accompanying `qsound_LICENSE.txt` retains the BSD notice. See `dsp16-port.md` for the instruction engine and its limitations.

## API

`qsound_create` accepts exactly 0x1000 logical 16-bit firmware words (copied), a caller-owned immutable sample-byte region, optional clock, PCM/DAC callbacks and diagnostics. Required missing media fails creation. The default crystal clock is 60 MHz. The original `dl-1425.bin` has little-endian word bytes; MAME's LLE `ROM_REGION16_BE` plus `ROM_LOAD16_WORD_SWAP` produces those same logical words. Only its first 0x2000 bytes are mapped; the additional 0x4000 bytes are ignored upstream. The wrapper does not load or transform files.

Capcom ZN callers subtract `0xd000` before `qsound_write`:

| Offset | Source behavior |
| --- | --- |
| 0 | Set command data high byte |
| 1 | Set command data low byte |
| 2 | Snapshot both data bytes, set 8-bit command address, clear ready and assert DSP INT |

`qsound_read` returns the status mapped at `0xd007`: 0x80 when ready, otherwise 0. Creation/reset ready=1 follows MAME. Subsequent readiness changes require a real DSP PIO read: the first active read consumes the pending command address, clears INT and sets ready; subsequent reads return the buffered data. Active reads are latent, so firmware uses the same repeated-read sequence as upstream. Commands issued while pending replace the current command, as source code does; there is no invented FIFO or busy-write rejection.

The caller schedules host writes at its current emulated time. MAME's scheduler barriers are represented by the synchronous host bus boundary and DSP PIO callback boundary. New data-byte writes cannot corrupt the buffered data of an already dispatched command. `qsound_get_dsp` provides debugging access to the actual CPU; it does not shortcut command processing.

## Sample ROM and serial audio

DSP PDX0/PDX1 output selects the sample offset and the upper bank bit. DSP external reads mirror word-address bit 15, fetch from the **previous** sample bank, then update the lower bank bits from the external word address. This one-read latency is preserved.

The sample-ROM interface masks addresses to 24 bits. Regions smaller than 16 MiB mirror the smallest power-of-two aperture containing the region; holes in non-power-of-two regions return zero, matching the default MAME ROM-space unmapped value. Regions at least 16 MiB use 16 MiB windows, with `qsound_set_sample_region_bank` selecting modulo the number of complete windows. DSP bank bits beyond the 24-bit sample aperture do not create extra address lines. The data returned to the DSP is the selected **8-bit byte shifted left eight**, matching MAME; there is no invented 16-bit sample mode or byte smearing.

Serial recovery literally follows upstream OCK/OLD/OSE/PSEL/DO ordering. PSEL at the beginning of a serial word selects its stereo channel. Each recovered signed 16-bit DAC value updates that channel and optionally calls `dac_write` with a machine-cycle timestamp. OCK callbacks precede the DSP serial shift, as required by the existing QSound recovery logic.

`pcm` emits a uniform signed stereo stream holding the current recovered DAC values. Its rate is `floor(crystal_hz / 2 / 1248)`, exactly the integer rate requested by MAME's `stream_alloc`: 24,038 Hz at 60 MHz. A rational phase accumulator preserves this rate without accumulating scheduler-slice error. DSP firmware's nominal iteration period is 1,248 machine cycles; its exact physical 24,038.4615 Hz cadence and MAME's integer stream rate are distinct. TC9185P volume circuitry is not modeled upstream or here.

## Clock and lifecycle

`qsound_run_cycles` advances DSP **machine cycles**, each equal to two crystal clocks. It slices execution at PCM boundaries and preserves mid-instruction DSP state. `qsound_run_clocks` accepts crystal clocks and retains an odd-clock remainder. Both return the number of DSP cycles completed; faults are visible in status and diagnostic callbacks. `qsound_set_clock` changes the crystal/sample rate while retaining fractional PCM phase. Callbacks must not recursively run or reset the device.

Reset clears pending command/IRQ and sets ready, then applies the DSP selective hardware reset. It preserves upstream retained command data/latch, sample bank/offset, recovered audio and serial-recovery state, plus cumulative debug counters and stream timing. Status includes command latches, ready/pending, sample-bank state, recovered samples/counters, clock/rate and the full DSP status.

## Verification and limits

```sh
make -f tests/Makefile.qsound test
make -f tests/Makefile.qsound test BUILD=build/qsound-sanitizers \
  CXXFLAGS='-O0 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
```

Normal and ASan/UBSan tests pass. Executed synthetic firmware covers actual IRQ/PIO command transfers into DSP RAM, readiness without timeout shortcuts, command-data buffering and overwrite behavior, one-read bank latency, 24-bit/mirrored ROM behavior, region banks and non-power-of-two holes. Separate firmware reads known sample bytes and transmits nonzero stereo values, recovered as 0xa300 and 0x5c00. Whole and single-cycle schedules produce identical state and audio. Clock tests cover odd crystal clocks, rate changes, and exactly 24,038 PCM frames after 30 million DSP cycles.

No authentic production DSP firmware was available for a boot/audio comparison. The standalone DSP16 engine's explicitly unsupported receive data, random predicates and other upstream peripheral limitations remain applicable. This component does not establish Capcom ZN boot, hardware-perfect DAC glue, or equivalence to MAME's default HLE device.
