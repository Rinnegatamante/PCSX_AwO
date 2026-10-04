# Native System 10 adapter test

`tests/check_system10_harness.py` runs synthetic MEM(M) and MEM(N) packages
through the native ARM64 headless harness. It downloads no ROMs and makes no
claim that these fixtures boot any game. Each package is assembled with
`prepare_system10_board.package_regions` from the pinned System 10 registry,
using the real region names, sizes, board profiles, and metadata format.

The MEM(M) fixture encodes a small MIPS-I program into the raw region with the
`mrdrilr2` passive XOR/permutation. The guest selects NAND bank zero, reads
the source bank predictor register, and executes a helper from that dynamic
window through JALR, both with and without interpreter instruction caching.
The MEM(N) fixture encodes its reset program in
`chocovdr` NAND0 pages 0x40 onward, then programs the real NAND read command,
column/page address bytes, and PSX DMA channel 5 to copy 16 bytes from page
0x300 into upper RAM. Boot data respects the 512-byte data and 16-byte OOB
stride of each raw NAND page. A second MEM(N) program requests a transfer
past the RAM boundary and must fault before copying any bytes.

Both guests poll the base active-low IN1 port while the harness applies a
frame-indexed button press and release. They read and acknowledge the board's
immediate timer IRQ status, write and read an upper-RAM sentinel, confirm that
the corresponding low-RAM address remains independent, and issue a GPU fill
followed by a VRAM readback. The harness trace checks guest register values;
the final device report checks DMA length and fault counters. Invalid sets,
duplicate metadata, mismatched NAND size metadata, and truncated MEM(M) flash
are also required to fail before the guest executes.

Build the native macOS ARM64 harness, then run:

```sh
python3 tests/check_system10_harness.py --harness build/mac-harness/pcsx-harness
```

The test needs roughly 130 MiB for generated MEM(N) fixture regions plus the
harness's loaded copies. Temporary packages, traces, and frames are removed
when the test ends.
