# Standalone DSP16 / DSP16A

`libpcsxcore/arcade/dsp16.cpp` ports the complete **implemented MAME interpreter** for both AT&T DSP16 variants. It retains ROM execution phases, the 15-instruction DO/REDO cache, XAAU/YAAU addressing, DAU operations and flags, interrupt eligibility and delayed acknowledgment, and built-in serial output and parallel strobes. It is a device component; this does not establish that QSound or a Capcom ZN board boots.

The primary source is the local MAME checkout at commit `deb2f54fe7300e7a192690318beb860159bfd52a`, specifically `src/devices/cpu/dsp16/{dsp16.cpp,dsp16.h,dsp16core.h,dsp16core.ipp}`. Copyright Vas Crabb, BSD-3-Clause; the complete notice is in `libpcsxcore/arcade/dsp16_LICENSE.txt`. MAME's debugger, CPU-device scaffolding, memory-space objects and unused DRC scaffolding are removed. No replacement instruction semantics or invented DSP firmware are supplied.

## Build and verification

Build the single C++17 translation unit `libpcsxcore/arcade/dsp16.cpp`. The public `dsp16.h` is C99-compatible and exposes an opaque handle with C linkage. Generated includes are committed; Python/MAME are not runtime or ordinary build dependencies.

```sh
make -f tests/Makefile.dsp16 test
make -f tests/Makefile.dsp16 test BUILD=build/dsp16-sanitizers \
  CXXFLAGS='-O0 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
python3 libpcsxcore/arcade/dsp16_generate.py ../mamedev/src/devices/cpu/dsp16
```

Tests include 6,016 independent mathematical DAU expectations across all F1 operations, every defined F2 operation, source/destination accumulators, product alignment, overflow flags and randomized inputs. There are 62 legal top-level opcode-family/model cases, plus pipeline and memory transfers, circular addressing, DSP16 pointer sign extension, accumulator saturation, conditional branches, calls/returns, DO/REDO and cached two-cycle stores, page wrapping, external ROM callbacks, reset retention, IRQ/IACK/ireturn, active PIO strobe durations, serial bit order/word length, clock divisors, fault paths and 10,000-cycle scheduler-slice equivalence.

An executed synthetic firmware fixture loads PIOC/SIOC/SDX/PT registers, writes a parallel command, reads external sample ROM, stores internal RAM and sends a serial word recovered through callbacks using upstream clock ordering. Normal and ASan/UBSan builds pass. No authentic DSP firmware fixture was available for a boot or audio comparison.

## API and memory

`dsp16_create(config)` requires caller-supplied ROM and copies it. It accepts exactly 0x800 logical 16-bit words for `DSP16_MODEL_16`, or 0x1000 for `DSP16_MODEL_16A`. Convert a big-endian byte image to logical words before calling; pointer byte order does not perform ROM_LOAD transformations. Invalid models, absent ROM and incorrect lengths fail creation. There are no default keys, ROMs or fabricated commands.

All addresses are **word addresses**. `external_read(opaque,address)` services the MAME external ROM space. EXM input high selects internal ROM: DSP16 mirrors its 0x800 words over the entire address space; DSP16A uses internal ROM only below 0x1000 and reads external ROM above it. EXM low selects external ROM everywhere. An actual external read without its callback faults.

DSP16 has 512 internal RAM words, DSP16A 2048. The latter retains 16-bit YAAU pointers while mirroring RAM at the 11-bit physical address. `dsp16_ram_read/write` use that same mapping. MAME implements no separate external RAM here. ROM and RAM start from supplied/zeroed storage; hardware reset retains RAM.

`dsp16_read_register/write_register` use actual six-bit ISA R encodings, including peripheral access side effects. `dsp16_get_registers` exposes PC/PT/PR/PI/I, four RAM pointers and bounds/increments, X/Y/P, both signed 36-bit accumulators, three counters, AUC and exported PSW. `dsp16_set_registers` normalizes register widths and restarts fetch/cache execution. It is a debugging entry point, not a complete peripheral or mid-instruction save-state restore; A accumulator values take precedence over PSW guard-bit fields.

## Timing and callbacks

`dsp16_run_cycles(cpu,n)` advances exactly `n` **machine cycles**, with two crystal clocks per machine cycle. It may stop between instruction phases, so it needs no instruction overshoot debt. Reset starts in the PURGE fetch phase. Callback order and peripheral stepping are retained per machine cycle. MAME's QSound DSP16A runs at 60 MHz crystal, hence 30 million machine cycles per emulated second; its serial-derived sample period is 1,248 machine cycles. Board scheduling and DAC/sample recovery belong to the caller.

`dsp16_set_input` accepts logical asserted/deasserted levels for INT, ICK, ILD, OCK, OLD and EXM. OCK/ICK inputs initially read high, matching MAME; supply a low transition before the first external rising edge. Internal IRQ masks and interruptible instruction rules apply. IRQs do not interrupt cached execution. IACK is physically active low and is deasserted after ireturn according to the retained delayed state machine.

`pio_read/write` callbacks represent the active PDX0/PDX1 interface, including selection and 1–4-machine-cycle strobes. Passive host transfers use `dsp16_host_pio_read/write`; attempted passive transfers during active mode produce upstream warnings and preserve its behavior. `parallel_bus_write` reports drive masks, including zero for high impedance. Output pin callbacks report IACK, serial clocks/frame/data/end signals, PSEL/PIDS/PODS; high impedance serial/strobe levels are represented high, as upstream.

Serial OCK callbacks occur **before** the corresponding shift/output update. A consumer reconstructing words should retain this ordering, as MAME QSound does. `dsp16_get_status` is safe from callbacks and reports current output levels, peripheral registers, phase/cache state, cumulative completed machine cycles and diagnostics. `instructions` counts initiated OP1 dispatches, including interrupted/retried dispatches, rather than retired instructions. Callback timestamps represented by the status cycle count are at the beginning of the current machine cycle. Caller callbacks must not throw C++ exceptions or recursively run/reset the CPU; IRQ changes and status queries are supported.

Hardware `dsp16_reset` reproduces the upstream selective reset: it clears fetch/cache execution, PC, RAM bounds, interrupt masks and peripheral configuration/status specified there, but preserves general arithmetic registers, RAM, external input levels and other retained peripheral latches. Counters are cumulative. Reset also releases a diagnostic halt.

## Explicit limitations

- MAME does not implement random heads/tails predicates CON 0x08/0x09. These fault, along with reserved instructions, F2 0x0a, invalid register encodings, ALIGN 3 and malformed reserved fields. Assertions in upstream become checked faults even in release builds.
- Serial receive data, SRTA and TDMS are unimplemented upstream. Actual reads of SDX/SRTA/TDMS, writes of SRTA/TDMS and selection of unimplemented PIO S/C mode fault. Ignored receive-frame clocks retain upstream ILD-flag behavior and warn once per reset; they do not generate receive data. SDX output is implemented.
- Upstream low-level PIO hooks are incomplete. The active callbacks and passive host transactions are available, but this does not model arbitrary external PDB/PIDS wire circuitry or time-varying bus contention.
- Upstream ambiguities remain: rounding F2 0x0b, overflow flag stickiness, some accumulator transfer saturation, serial signal details and interrupt sampling. F2 high-half increment currently clears the low half even when AUC preservation is selected because upstream overwrites its destination before reading that low half. Tests pin this source behavior; it has not been established as hardware-correct.
- Diagnostics stop invalid/unsupported instruction execution by setting `halted`; `run_cycles` returns the completed cycles preceding the fault. A fault may occur after part of the instruction's state update, so it is not a rollback mechanism. Firmware inputs must be authentic and the caller must handle faults explicitly.

The engine follows MAME's interpreter timing and behavior. The synthetic tests are not a hardware or complete MAME-oracle proof, and this port makes no claim of complete peripheral emulation or verified QSound output with production firmware.
