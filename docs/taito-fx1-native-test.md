# Taito FX-1S native fixture

`tests/check_taito_fx1_harness.py` packages deterministic synthetic media for
the `sfchamp` FX-1S profile and runs it through the native harness. It checks
the board adapter and connected devices; it does not boot or emulate a retail
game. The generated MIPS and Z80 programs, arbitrary CAT702 bytes, and erased
ADPCM samples are test inputs only.

Run it after building the native macOS harness:

```sh
python3 tests/check_taito_fx1_harness.py
make -f Makefile.mac-harness taito-fx1-test
```

The fixture executes from the PSX reset vector and checks the FX-1S bank
window, independent upper RAM, type-2 GPU fill/readback, input transitions,
the ZN MCU's serial header and IRQ7, AT28C16 busy/completion and persisted
data, and the TC0140SYT master/slave nibble exchange. Its sound Z80 enables
the TC0140SYT NMI path, replies to the command, and programs the YM2610B SSG;
the test checks that the harness writes nonzero stereo PCM at 55,555 Hz.
The test repeats execution with instruction cache enabled and banked code
fetched through `9F000100` instead of `BF000100`. Register checks use completed
guest traces so initial zero register values cannot satisfy the assertions.
The harness rejects a mismatched set, wrong VRAM geometry, duplicate config
keys and truncated Z80 media before guest execution.
An additional 310-frame run leaves the watchdog unfed. It verifies a real
five-second timeout requests one deferred reset, the CPU restarts safely,
and the host still completes all 310 frames without a device fault.

The harness uses three emulated frames with `--board-test`, a frame-indexed
P1 button press/release input file, a synthetic 2080-byte EEPROM image, a CSV
instruction trace, and a WAV output. All generated files live in a temporary
directory and are removed after the test. The script imports MIPS instruction
encoders from `tests/check_system10_harness.py` and packages regions with
`scripts/prepare_taito_fx1_board.py`; it does not use copyrighted game ROMs or
claim game boot compatibility.
