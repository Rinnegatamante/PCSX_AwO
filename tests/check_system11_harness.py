#!/usr/bin/env python3
"""Run synthetic, instruction-driven System 11 board fixtures in the harness.

The ROM bytes are generated here and contain real MIPS-I and M37710/C76
instructions. They validate profile loading, PSX bus aliases/banks/shared RAM,
Tekken digital/ADC controls, C76 sound programming, AT28C16 persistence and
pre-execution rejection paths; they are not game ROMs and do not download media.
"""
from __future__ import annotations

import argparse
import csv
import re
import shutil
import struct
import subprocess
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _i(op: int, rs: int, rt: int, immediate: int) -> int:
    return ((op & 0x3f) << 26) | ((rs & 31) << 21) | ((rt & 31) << 16) | (immediate & 0xffff)


def _lui(rt: int, imm: int) -> int:
    return _i(0x0f, 0, rt, imm)


def _ori(rt: int, rs: int, imm: int) -> int:
    return _i(0x0d, rs, rt, imm)


def _addiu(rt: int, rs: int, imm: int) -> int:
    return _i(0x09, rs, rt, imm)


def _lw(rt: int, base: int, offset: int) -> int:
    return _i(0x23, base, rt, offset)


def _lhu(rt: int, base: int, offset: int) -> int:
    return _i(0x25, base, rt, offset)


def _sw(rt: int, base: int, offset: int) -> int:
    return _i(0x2b, base, rt, offset)


def _sh(rt: int, base: int, offset: int) -> int:
    return _i(0x29, base, rt, offset)


def _sb(rt: int, base: int, offset: int) -> int:
    return _i(0x28, base, rt, offset)


def _j(address: int) -> int:
    return 0x08000000 | ((address >> 2) & 0x03ffffff)


def _jalr(rs: int, rd: int = 31) -> int:
    return ((rs & 31) << 21) | ((rd & 31) << 11) | 0x09


def make_mips_program(eeprom_write: bool = False) -> bytes:
    """Set all eight ROM8 windows, read real bank markers and touch shared RAM."""
    code: list[int] = [_lui(8, 0xbfa1), _lui(13, 0xbfa0), _ori(13, 13, 0x4000)]
    if eeprom_write:
        # One guest-issued byte write on boot; the MIPS loop never touches NVRAM.
        code.extend([_lui(17, 0xbfa3), _addiu(18, 0, 0x5a), _sb(18, 17, 0)])
    for window in range(8):
        bank_entry = window
        selector = bank_entry if bank_entry < 4 else 0x40 | (bank_entry - 4)
        code.extend([
            _addiu(9, 0, selector),
            _sh(9, 8, 0x20 + window * 2),
            _lui(10, 0xbf00 + window * 0x10),
            _lw(11, 10, 0),
            0,  # MIPS-I load delay slot.
            _sw(11, 13, window * 4),
            _lui(10, 0xbf00 + window * 0x10),
            _ori(10, 10, 0x100),
            _jalr(10),
            0,
        ])

    # KSEG0 alias of the first KSEG1 ROM bank, then a second read through
    # the board's PSX shared-memory aperture.
    code.extend([
        _lui(14, 0x9f00), _lw(15, 14, 0), 0,
        _sw(15, 13, 0x20),
        _lw(24, 13, 0), 0,
        _sw(24, 13, 0x24),
    ])
    loop_address = 0xbfc00000 + len(code) * 4
    code.extend([
        _lhu(12, 13, 4), 0, _sw(12, 13, 0x40),
        _lhu(12, 13, 6), 0, _sw(12, 13, 0x44),
        _lhu(20, 13, 8), 0,
        _lhu(21, 13, 10), 0,
        _lhu(22, 13, 12), 0,
        _addiu(23, 0, 0x5a),
        _j(loop_address), 0,
    ])
    rom = bytearray(0x400000)
    struct.pack_into(f"<{len(code)}I", rom, 0, *code)
    return bytes(rom)


def make_c76_bios() -> bytes:
    """C76 firmware polls real input ports and programs voice zero on C352."""
    program = bytearray()

    def lda_imm(value: int) -> None:
        program.extend((0xa9, value & 0xff, (value >> 8) & 0xff))

    def lda_abs(address: int) -> None:
        program.extend((0xad, address & 0xff, (address >> 8) & 0xff))

    def sta_abs(address: int) -> None:
        program.extend((0x8d, address & 0xff, (address >> 8) & 0xff))

    def sample_adc(channel: int, output: int) -> None:
        # M37710 AD control bit 6 starts one-shot conversion; the low three
        # bits select AN0..AN7. Poll that same hardware register until it clears.
        lda_imm(0x40 | channel)
        sta_abs(0x001e)
        wait = len(program)
        lda_abs(0x001e)
        program.extend((0x29, 0x40, 0x00))  # AND #$0040
        displacement = wait - (len(program) + 2)
        program.extend((0xd0, displacement & 0xff))  # BNE wait
        lda_abs(0x0020 + channel * 2)
        sta_abs(output)

    # Poll PLAYER1 and SWITCH and publish their raw active-low words to the
    # real shared RAM; the loop observes later frame-indexed host input changes.
    lda_abs(0x1004)
    sta_abs(0x4004)
    lda_abs(0x1002)
    sta_abs(0x4006)
    lda_imm(0x8080); sta_abs(0x2000)  # C352 front L/R volume
    lda_imm(0x4040); sta_abs(0x2002)  # C352 rear L/R volume
    lda_imm(0x8000); sta_abs(0x2004)  # C352 frequency
    lda_imm(0x4006); sta_abs(0x2006)  # key-on, filter and loop
    lda_imm(0); sta_abs(0x2008); sta_abs(0x200a)
    lda_imm(63); sta_abs(0x200c)
    lda_imm(0); sta_abs(0x200e)
    lda_imm(0); sta_abs(0x2404)  # C352 global key-on strobe
    loop = len(program)
    lda_abs(0x1004); sta_abs(0x4004)
    lda_abs(0x1002); sta_abs(0x4006)
    lda_abs(0x1000); sta_abs(0x4008)  # PLAYER4, including P2 button 3/4 on Tekken.
    sample_adc(1, 0x400a)             # Tekken P1 button 4 on ADC1.
    sample_adc(2, 0x400c)             # Tekken P1 button 3 on ADC2.
    displacement = loop - (len(program) + 2)
    program.extend((0x80, displacement & 0xff))  # BRA loop
    bios = bytearray(0x4000)
    bios[:len(program)] = program
    bios[0x100] = 0x40  # RTI for the synthetic IRQ0/IRQ2 service path.
    bios[0x3ff0:0x3ff2] = b"\x00\xc1"
    bios[0x3ff4:0x3ff6] = b"\x00\xc1"
    # C76/M37710 reset vector points at the internal ROM base $c000.
    bios[0x3ffe:0x4000] = b"\x00\xc0"
    return bytes(bios)


def create_fixture(directory: Path, model: str, eeprom_write: bool = False) -> None:
    directory.mkdir(parents=True)
    main = make_mips_program(eeprom_write)
    (directory / "main.bin").write_bytes(main)
    banked = bytearray(0x1000000)
    for bank in range(16):
        marker = 0xbeef0000 | bank
        struct.pack_into("<I", banked, bank * 0x100000, marker)
        struct.pack_into("<I", banked, (bank + 1) * 0x100000 - 4, marker ^ 0xffffffff)
        # The CPU executes this real MIPS-I routine through each selected ROM
        # window after checking its independent marker at offset zero.
        struct.pack_into("<4I", banked, bank * 0x100000 + 0x100,
                         _addiu(17, 0, bank + 1), 0x03e00008, 0, 0)
    (directory / "banked.bin").write_bytes(banked)
    c76 = bytearray([0xff]) * 0x80000
    c76[:16] = b"S11C76-REAL-CODE"
    (directory / "c76.bin").write_bytes(c76)
    (directory / "c76bios.bin").write_bytes(make_c76_bios())
    (directory / "wave.bin").write_bytes(bytes([0x40]) * 0x10000)

    if model == "COH100":
        set_name, machine, banking, keycus = "tekken", "tekken", "ROM8", "C406"
    else:
        set_name, machine, banking, keycus = "tekken2", "tekken2", "ROM8", "C406"
    (directory / "board.ini").write_text(
        "format=pcsx-aow-system11-v1\n"
        f"mame_set={set_name}\n"
        "mame_parent=none\n"
        f"board_model={model}\n"
        f"banking={banking}\n"
        f"keycus={keycus}\n"
        "lightgun=false\n"
        f"mame_machine={machine}\n"
        "mame_input=tekken\n"
        "mame_flags=0\n"
        "board_execution_verified=false\n",
        encoding="utf-8")


def run_harness(harness: Path, directory: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([str(harness), "--system11", str(directory), *arguments],
                            capture_output=True, text=True, timeout=45)
    if result.stderr:
        print(result.stderr, end="")
    return result


def validate_success(result: subprocess.CompletedProcess[str], model: str, gpu: int) -> None:
    result.check_returncode()
    required = (
        "System11 selftest: PASS",
        f"System11: profile=", f"model={model} GPU={gpu} RAM=4194304 banking=1 KEYCUS=406",
    )
    for expected in required:
        if expected not in result.stderr:
            raise RuntimeError(f"System 11 {model} run is missing diagnostic {expected!r}")
    c76 = re.search(
        r"System11: C76 pc=[0-9a-fA-F]+ cycles=(\d+) instructions=(\d+) illegal=(\d+) unsupported=(\d+) fault=(\d+)",
        result.stderr)
    if not c76:
        raise RuntimeError("System 11 C76 instruction diagnostics are missing")
    cycles, instructions, illegal, unsupported, fault = map(int, c76.groups())
    if not cycles or not instructions or illegal or unsupported or fault:
        raise RuntimeError("synthetic C76 firmware did not execute cleanly")


def check_fixture(harness: Path, directory: Path, model: str, gpu: int,
                  with_inputs_and_audio: bool) -> None:
    trace = directory / "trace.csv"
    frame = directory / "frame.ppm"
    command = ["--board-test", "--frames", "4", "--trace", str(trace),
               "--trace-blocks", "5000", "--output", str(frame)]
    audio = directory / "sound.wav"
    if with_inputs_and_audio:
        inputs = directory / "inputs.csv"
        inputs.write_text(
            "frame,control,pressed\n"
            "0,coin1,1\n0,p1start,1\n"
            "0,p2button3,1\n0,p2button4,1\n"
            "0,p1button4,1\n"
            "1,p2button3,0\n1,p2button4,0\n1,p1button4,0\n",
            encoding="utf-8")
        command += ["--inputs", str(inputs), "--audio", str(audio)]
    result = run_harness(harness, directory, *command)
    validate_success(result, model, gpu)
    if with_inputs_and_audio:
        if "System11: inputs=ff/df/7f/ff" not in result.stderr:
            raise RuntimeError("CSV coin/start/Tekken extra-button press/release did not reach input reporting")
        c352 = re.search(r"System11: C352 rate=88200 frames=(\d+) peak=(\d+) WAV_bytes=(\d+)",
                         result.stderr)
        if not c352 or not int(c352.group(1)) or not int(c352.group(2)) or not int(c352.group(3)):
            raise RuntimeError("C76 firmware did not program C352 audio")
        with wave.open(str(audio), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 88200):
                raise RuntimeError("System 11 WAV format is not stereo PCM16 at 88.2 kHz")
            samples = wav.readframes(wav.getnframes())
            if not wav.getnframes() or not any(samples):
                raise RuntimeError("instruction-programmed C352 fixture produced no nonzero sound")
    if not frame.is_file() or not frame.read_bytes().startswith(b"P6\n"):
        raise RuntimeError("System 11 rendered frame is missing")
    if not trace.is_file():
        raise RuntimeError("MIPS trace was not produced")
    with trace.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    if len(rows) < 2 or rows[0]["pc"] != "bfc00000":
        raise RuntimeError("synthetic MIPS program did not execute from the PSX reset vector")
    if not any(row.get("r11") == "beef0007" and row.get("r15") == "beef0000"
               and row.get("r24") == "beef0000" for row in rows):
        raise RuntimeError("guest MIPS did not fetch ROM8 banks/shared RAM through both KSEG aliases")
    if not any(row.get("r17") == "00000008" for row in rows):
        raise RuntimeError("guest MIPS did not execute the eighth bank's ROM subroutine")
    if with_inputs_and_audio:
        if not any(row.get("r20") == "000000cf" for row in rows):
            raise RuntimeError("C76 firmware did not publish live PLAYER4/P2 button 3/4 inputs")
        if not any(row.get("r23") == "0000005a" and row.get("r21") == "00000000"
                   and row.get("r22") == "000000ff"
                   for row in rows):
            raise RuntimeError("C76 ADC1 conversion did not publish Tekken P1 button 4 raw value")


def check_adc2_input_fixture(harness: Path, directory: Path) -> None:
    create_fixture(directory, "COH100")
    inputs = directory / "inputs.csv"
    inputs.write_text("frame,control,pressed\n0,p1button3,1\n1,p1button3,0\n", encoding="utf-8")
    trace = directory / "trace.csv"
    result = run_harness(harness, directory, "--board-test", "--frames", "2",
                         "--inputs", str(inputs), "--trace", str(trace),
                         "--trace-blocks", "45000")
    validate_success(result, "COH100", 1)
    with trace.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    if not any(row.get("r23") == "0000005a" and row.get("r21") == "000000ff"
               and row.get("r22") == "00000000" for row in rows):
        raise RuntimeError("C76 ADC2 conversion did not publish Tekken P1 button 3 raw value")
    if not any(row.get("r23") == "0000005a" and row.get("r21") == "000000ff"
               and row.get("r22") == "000000ff" for row in rows[5000:]):
        raise RuntimeError("released Tekken P1 button 3 did not return to ADC2 raw default")


def check_nvram_persistence(harness: Path, directory: Path) -> None:
    create_fixture(directory, "COH100", eeprom_write=True)
    nvram = directory / "namcos11.nv"
    trace = directory / "nvram-trace.csv"
    first = run_harness(harness, directory, "--board-test", "--frames", "4",
                        "--nvram", str(nvram), "--trace", str(trace),
                        "--trace-blocks", "20")
    validate_success(first, "COH100", 1)
    if not nvram.is_file():
        raise RuntimeError("successful System 11 run did not create its NVRAM image")
    saved = nvram.read_bytes()
    if len(saved) != 2080 or saved[0] != 0x5a or saved[1:] != b"\xff" * 2079:
        raise RuntimeError("guest NVRAM write or erased-byte persistence is incorrect")

    # Import a known second byte at word-address one, then confirm a later guest
    # write to byte zero preserves that untouched word.
    imported = bytearray(b"\xff" * 2080)
    imported[2] = 0x33
    nvram.write_bytes(imported)
    second = run_harness(harness, directory, "--board-test", "--frames", "4",
                         "--nvram", str(nvram), "--trace", str(trace),
                         "--trace-blocks", "20")
    validate_success(second, "COH100", 1)
    saved = nvram.read_bytes()
    if len(saved) != 2080 or saved[0] != 0x5a or saved[2] != 0x33:
        raise RuntimeError("System 11 NVRAM import, update, or untouched-byte retention failed")

    malformed = directory / "malformed.nv"
    malformed_bytes = b"\x33" * 2079
    malformed.write_bytes(malformed_bytes)
    bad_trace = directory / "malformed-trace.csv"
    rejected = run_harness(harness, directory, "--nvram", str(malformed),
                           "--trace", str(bad_trace), "--trace-blocks", "20")
    if rejected.returncode == 0 or malformed.read_bytes() != malformed_bytes:
        raise RuntimeError("malformed System 11 NVRAM was accepted or overwritten")
    if "System11: C76 pc=" in rejected.stderr:
        raise RuntimeError("malformed System 11 NVRAM reached board/guest execution")
    if bad_trace.is_file() and len(bad_trace.read_text(encoding="utf-8").splitlines()) > 1:
        raise RuntimeError("malformed System 11 NVRAM reached guest CPU execution")


def rejected_before_guest(harness: Path, valid: Path, case_dir: Path,
                          missing_file: str | None = None,
                          board_ini: str | None = None) -> None:
    shutil.copytree(valid, case_dir)
    if missing_file:
        (case_dir / missing_file).unlink()
    if board_ini is not None:
        (case_dir / "board.ini").write_text(board_ini, encoding="utf-8")
    trace = case_dir / "must-not-run.csv"
    result = run_harness(harness, case_dir, "--frames", "1", "--trace", str(trace))
    if result.returncode == 0:
        raise RuntimeError("malformed or incomplete System 11 profile was accepted")
    trace_has_guest = False
    if trace.is_file():
        try:
            trace_has_guest = len(trace.read_text(encoding="utf-8").splitlines()) > 1
        except OSError:
            trace_has_guest = False
    if "System11: C76 pc=" in result.stderr or trace_has_guest:
        raise RuntimeError("invalid System 11 input reached guest CPU execution")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    args = parser.parse_args()
    harness = args.harness.resolve()
    if not harness.is_file():
        raise SystemExit(f"harness not found: {harness}; build with `make -f Makefile.mac-harness` first")
    with tempfile.TemporaryDirectory(prefix="pcsx-system11-test-") as temp:
        root = Path(temp)
        coh100 = root / "coh100"
        coh110 = root / "coh110"
        create_fixture(coh100, "COH100")
        create_fixture(coh110, "COH110")
        check_fixture(harness, coh100, "COH100", 1, True)
        check_fixture(harness, coh110, "COH110", 2, False)
        check_adc2_input_fixture(harness, root / "adc2-only")
        check_nvram_persistence(harness, root / "nvram")

        malformed = (coh100 / "board.ini").read_text(encoding="utf-8").replace(
            "keycus=C406\n", "keycus=UNKNOWN\n")
        rejected_before_guest(harness, coh100, root / "unknown-keycus", board_ini=malformed)
        rejected_before_guest(harness, coh100, root / "missing-bios", missing_file="c76bios.bin")
        unknown_set = (coh100 / "board.ini").read_text(encoding="utf-8").replace(
            "mame_set=tekken\n", "mame_set=unknown-board\n")
        rejected_before_guest(harness, coh100, root / "unknown-profile", board_ini=unknown_set)
        truncated = root / "truncated-main"
        shutil.copytree(coh100, truncated)
        (truncated / "main.bin").write_bytes(b"bad")
        rejected_before_guest(harness, truncated, root / "truncated-main-check")
    print("System11 synthetic MIPS/C76 board, profile and rejection checks PASS")


if __name__ == "__main__":
    main()
