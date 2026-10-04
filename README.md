PCSX-ReARMed - yet another PCSX fork
====================================

![CI (Linux)](https://github.com/notaz/pcsx_rearmed/workflows/CI%20(Linux)/badge.svg)

PCSX ReARMed is yet another PCSX fork based on the PCSX-Reloaded project,
which itself contains code from PCSX, PCSX-df and PCSX-Revolution. This
version was originally ARM architecture oriented (hence the name) with
its MIPS->ARM dynamic recompilation and assembly optimizations, but more
recently it targets other architectures too. A fork of this emulator was
used in [PS Classic](https://en.wikipedia.org/wiki/PlayStation_Classic)
(without any coordination or even notification).

## Features
* ARM/ARM64 dynamic recompiler by Ari64
* [lightrec](https://github.com/pcercuei/lightrec/) dynamic recompiler for other architectures
* NEON GPU by Exophase for ARM NEON and x86 SSE2+
* PCSX4ALL GPU by Una-i/senquack for other architectures
* heavily modified P.E.Op.S. SPU
* BIOS HLE emulation (most games run without proprietary BIOS)
* libretro support

## Native macOS harness and arcade board ports

An Apple Silicon headless harness runs the interpreter with the existing NEON
GPU. `make mac-smoke` builds and verifies a generated MIPS graphics program.
See [the harness instructions](tools/mac-harness/README.md) and
[Soul Calibur port status](docs/system12.md) for ROM verification, board tests,
and current limits. The verified Soul Calibur Asia C set reaches attract mode
and a match through scripted coin, start and character-selection inputs, with
C352 audio. Other regional sets and complete gameplay fidelity remain unverified.

The full MAME PSX-derived scope is recorded in
[the machine inventory](docs/mame-psx-systems.md): 491 registrations across ten
driver families. The inventory distinguishes declared hardware from verified
board support. Native generated guests also exercise System 10, System 11,
Capcom ZN and Taito FX1S. Standalone device checks are available with
`make -f Makefile.mac-harness devices-test`; each board's native test target
is documented in the harness instructions.
