#!/usr/bin/env python3
"""Execute System12 synthetic MIPS code through a native DRC harness.

The fixture checks translated bank replacement at one logical PC, self-modifying
code above 2 MiB, and board-selected 4/16 MiB RAM page identities. No game ROM
or BIOS data is used.
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lui(rt, imm): return (0x0f << 26) | (rt << 16) | (imm & 0xffff)
def ori(rs, rt, imm): return (0x0d << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff)
def addiu(rs, rt, imm): return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff)
def addu(rs, rt, rd): return (rs << 21) | (rt << 16) | (rd << 11) | 0x21
def sw(rt, off, rs): return (0x2b << 26) | (rs << 21) | (rt << 16) | (off & 0xffff)
def lw(rt, off, rs): return (0x23 << 26) | (rs << 21) | (rt << 16) | (off & 0xffff)
def lbu(rt, off, rs): return (0x24 << 26) | (rs << 21) | (rt << 16) | (off & 0xffff)
def lhu(rt, off, rs): return (0x25 << 26) | (rs << 21) | (rt << 16) | (off & 0xffff)
def sh(rt, off, rs): return (0x29 << 26) | (rs << 21) | (rt << 16) | (off & 0xffff)
def jalr(rs, rd=31): return (rs << 21) | (rd << 11) | 9
def jr(rs): return (rs << 21) | 8
def j(addr): return (2 << 26) | ((addr >> 2) & 0x03ffffff)
def nop(): return 0


def emit(code, *words): code.extend(words)


def make_guest(ram_mb, vita_signature=False):
    """Return main/banked bytes with code and bank markers generated locally."""
    main = bytearray(0x400000)
    banked = bytearray(0x800000 if ram_mb == 16 else 0x2000000)
    # Reset code deliberately switches to the 1fc fixed-ROM alias; the
    # translated fixture itself starts there, not in the KSEG boot alias.
    struct.pack_into("<IIII", main, 0, lui(8, 0x1fc0), ori(8, 8, 0x1000), jr(8), nop())
    code = []
    # t0 = bank-select register, t1=bank 0, t2=logical bank code entry.
    emit(code, lui(8, 0x1f00), addiu(0, 9, 0), sh(9, 0, 8),
         lui(10, 0xbfa0), jalr(10), nop(), addu(2, 0, 16),
         j(0), nop())
    bank1_jump = len(code) - 2
    # Change the backing source and execute exactly the same virtual PC again.
    if ram_mb == 16:
        # COH-716's alt-bank state composes the bank in two writes.
        emit(code, addiu(0, 9, 8), sh(9, 0, 8))
    emit(code, addiu(0, 9, 1), sh(9, 0, 8), jalr(10), nop(),
         addu(2, 0, 17), j(0), nop())
    ram_jump = len(code) - 2
    # Upper-RAM sentinel and alias probe. RAM size is runtime selected by profile.
    emit(code, lui(8, 0x8021), lui(9, 0x51a7), ori(9, 9, 0xcafe),
         sw(9, 0, 8), lw(18, 0, 8))
    # 4 MiB RAM mirrors at 0x80600000 to physical 0x00200000. On 16 MiB it
    # addresses distinct RAM at offset 0x00600000; the fixture records both.
    emit(code, lui(12, 0x8020), addiu(0, 11, 0x2468), sw(11, 0, 12),
         lui(10, 0x8060), addiu(0, 11, 0x1357), sw(11, 0, 10),
         lw(19, 0, 12))
    # Install addiu v0,zero,0x1234 in upper RAM, execute, replace it with
    # addiu v0,zero,0x5678, and execute the same code address again.
    emit(code, lui(8, 0x8021), lui(9, 0x03e0), ori(9, 9, 0x0008),
         sw(9, 4, 8), sw(0, 8, 8),
         lui(9, 0x2402), ori(9, 9, 0x1234), sw(9, 0, 8))
    smc_entry = len(code)
    emit(code, jalr(8), nop(), addu(2, 0, 20),
         lui(9, 0x2402), ori(9, 9, 0x5678))
    if ram_mb == 4:
        # Rewrite the compiled block through the 4 MiB RAM mirror, then call
        # it through that same virtual alias. Both addresses resolve to the
        # same physical RAM, so stale translated code must be invalidated.
        # t4 is dead after the alias read; preserve s0, which holds bank 0's
        # signature and is copied into the Vita DRC result block below.
        emit(code, lui(12, 0x8061), sw(9, 0, 12), addu(8, 12, 0))
    else:
        # At 16 MiB, 0x80610000 is a distinct physical address, so update and
        # execute the original 0x80210000 code location instead.
        emit(code, sw(9, 0, 8))
    emit(code, jalr(8), nop(), addu(2, 0, 21))
    # Select source ROM offset 0x300000 (high bit selects fixed main ROM),
    # DMA three words into 2 MiB+ RAM, and execute that freshly copied helper.
    emit(code, lui(8, 0x1f70), ori(0, 9, 0x8030), sh(9, 0, 8),
         lui(10, 0x1f80), ori(10, 10, 0x10f0),
         lui(11, 0x0080), sw(11, 0, 10),
         lui(10, 0x1f80), ori(10, 10, 0x10d0),
         lui(11, 0x0022), sw(11, 0, 10),
         lui(11, 0x0001), ori(11, 11, 3), sw(11, 4, 10),
         lui(11, 0x0100), ori(11, 11, 0x0200), sw(11, 8, 10),
         lui(8, 0x8022), jalr(8), nop(), addu(2, 0, 22))
    # Exercise the guest-facing shared-RAM MMIO path in the translated CPU,
    # independent of the adapter's direct read/write selftest.
    emit(code, lui(8, 0x1f08), ori(8, 8, 0x0200),
         lui(9, 0x7654), ori(9, 9, 0x3210), sw(9, 0, 8),
         lui(9, 0x1357), ori(9, 9, 0x2468), sw(9, 4, 8),
         lui(9, 0x89ab), ori(9, 9, 0xcdef), sw(9, 8, 8),
         # Each load follows a store to a different address. DRC's dynamic
         # read callback must use its passed effective address, not the
         # global address left behind by the preceding store.
         lw(23, 0, 8), lhu(24, 6, 8), lbu(25, 11, 8))
    if vita_signature:
        # The Vita standalone probe reads only this guest-written RAM block.
        # The guest, not host register inspection, records each computed result.
        emit(code, lui(8, 0x8023), ori(8, 8, 0xf000),
             lui(9, 0x5331), ori(9, 9, 0x3244), sw(9, 0, 8),
             addiu(0, 9, ram_mb), sw(9, 4, 8),
             sw(16, 8, 8), sw(17, 12, 8), sw(18, 16, 8),
             sw(19, 20, 8), sw(20, 24, 8), sw(21, 28, 8),
             sw(22, 32, 8), sw(23, 36, 8), sw(24, 40, 8), sw(25, 44, 8))
    terminal = len(code)
    emit(code, j(0), nop())
    # Patch jumps after layout so both RAM profiles use correct branch targets.
    def rom_addr(index): return 0xBFC01000 + index * 4
    code[bank1_jump] = j(rom_addr(bank1_jump + 2))
    code[ram_jump] = j(rom_addr(ram_jump + 2))
    code[terminal] = j(rom_addr(terminal))
    struct.pack_into("<%dI" % len(code), main, 0x1000, *code)
    # Bank routines have different immediate signatures but share the same
    # virtual address and return sequence.
    struct.pack_into("<III", banked, 0x000000, addiu(0, 2, 0x1111), jr(31), nop())
    struct.pack_into("<III", banked, 0x200000, addiu(0, 2, 0x2222), jr(31), nop())
    struct.pack_into("<III", main, 0x300000, addiu(0, 2, 0x3333), jr(31), nop())
    if ram_mb == 16:
        # MAME COH-716 boot marker required for its documented startup path.
        main[0x20280:0x2028c] = b"BOOT-HACK-12"
    return main, banked


def make_regions(directory, ram_mb, vita_signature=False):
    main, banked = make_guest(ram_mb, vita_signature=vita_signature)
    (directory / "main.bin").write_bytes(main)
    (directory / "banked.bin").write_bytes(banked)
    sub = bytearray(0x80000)
    struct.pack_into(">I", sub, 0, 0x100)
    struct.pack_into(">H", sub, 0x100, 0x40fe)  # H8 BRA self
    (directory / "sub.bin").write_bytes(sub)
    (directory / "wave.bin").write_bytes(bytes(0x800000 if ram_mb == 16 else 0x1000000))
    if ram_mb == 16:
        (directory / "board.cfg").write_text(
            "schema=1\nmame_set=sws2001\nmachine=coh716\nstate=namcos12_altbank_state\n"
            "ram_size=16777216\nmain_size=4194304\nbank_size=8388608\n"
            "sub_size=524288\nwave_size=8388608\nboot_hack=1\nalt_bank=1\n"
            "tektagt_dma=0\nunsupported=none\n", encoding="ascii")
    elif vita_signature:
        (directory / "board.cfg").write_text(
            "schema=1\nmame_set=vita_drc_fixture4\nmachine=coh700\n"
            "state=namcos12_state\nram_size=4194304\nmain_size=4194304\n"
            "bank_size=33554432\nsub_size=524288\nwave_size=16777216\n"
            "boot_hack=0\nalt_bank=0\ntektagt_dma=0\nunsupported=none\n",
            encoding="ascii")


def check_trace(trace, ram_mb):
    rows = [line.split(",") for line in trace.read_text(encoding="ascii").splitlines()[1:]]
    if not rows:
        raise RuntimeError("DRC produced no guest execution trace")
    pcs = [int(row[2], 16) for row in rows]
    # The instruction-source provider must compile through both fixed ROM and
    # the selected bank window. Bank code can be observed through register 2.
    if pcs[0] != 0xBFC00000 or 0x1FC01000 not in pcs or 0xBFA00000 not in pcs:
        raise RuntimeError("guest did not execute reset ROM, 1fc ROM alias, and banked code")
    # Trace snapshots occur at dispatched block boundaries; a saved signature
    # must become visible before the second bank helper runs.
    s0 = [int(row[4 + 16], 16) for row in rows]
    s1 = [int(row[4 + 17], 16) for row in rows]
    if 0x1111 not in s0 or 0x2222 not in s1:
        raise RuntimeError("same-PC bank switch did not execute each bank's distinct code")
    if not any(int(row[4 + 18], 16) == 0x51a7cafe for row in rows):
        raise RuntimeError("4/16 MiB upper-RAM store/load failed")
    if not any(int(row[4 + 20], 16) == 0x1234 for row in rows):
        raise RuntimeError("first upper-RAM generated instruction did not execute")
    if not any(int(row[4 + 21], 16) == 0x5678 for row in rows):
        raise RuntimeError("self-modifying upper-RAM code was not recompiled")
    if not any(int(row[4 + 22], 16) == 0x3333 for row in rows):
        raise RuntimeError("DMA-loaded upper-RAM code did not execute after invalidation")
    if not any(int(row[4 + 23], 16) == 0x76543210 for row in rows):
        raise RuntimeError("translated guest shared-RAM word read used the wrong address")
    if not any(int(row[4 + 24], 16) == 0x1357 for row in rows):
        raise RuntimeError("translated guest shared-RAM halfword read used the wrong address")
    if not any(int(row[4 + 25], 16) == 0x89 for row in rows):
        raise RuntimeError("translated guest shared-RAM byte read used the wrong address")
    if ram_mb == 4 and not any(int(row[4 + 19], 16) == 0x1357 for row in rows):
        raise RuntimeError("4 MiB physical mirror alias was not visible")
    if ram_mb == 16 and not any(int(row[4 + 19], 16) == 0x2468 for row in rows):
        raise RuntimeError("16 MiB upper-RAM aliasing unexpectedly folded the 6 MiB address")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path,
                        default=ROOT / "build/mac-harness/drc/pcsx-harness")
    parser.add_argument("--drc-flag", default="--drc",
                        help="harness argument selecting native DRC (default: --drc)")
    parser.add_argument("--vita-signature", action="store_true",
                        help="append guest-written signature words used by the Vita private probe")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pcsx-system12-drc-") as tmp:
        root = Path(tmp)
        for ram_mb in (4, 16):
            region = root / f"ram{ram_mb}"
            region.mkdir()
            make_regions(region, ram_mb, vita_signature=args.vita_signature)
            trace = region / "trace.csv"
            output = region / "frame.ppm"
            command = [str(args.harness.resolve()), "--system12", str(region), args.drc_flag,
                       "--board-test", "--frames", "1", "--trace", str(trace),
                       "--trace-blocks", "300000", "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            print(result.stderr, end="")
            result.check_returncode()
            if "System12 selftest: PASS" not in result.stderr:
                raise RuntimeError("System12 board selftest failed in DRC fixture")
            if "cpu=ari64" not in result.stderr.lower() and "cpu=dynarec" not in result.stderr.lower():
                raise RuntimeError("harness did not report native DRC execution")
            if ("reserved instruction" in result.stderr or "Non-instruction" in result.stderr
                    or "\nNI " in result.stderr):
                raise RuntimeError("native DRC fixture raised an instruction-fetch fault")
            if "System12: DMA5=1 bytes=12" not in result.stderr:
                raise RuntimeError("DRC guest did not trigger the real channel 5 DMA transfer")
            check_trace(trace, ram_mb)
    print("System12 native DRC bank/RAM/SMC fixtures PASS")


if __name__ == "__main__":
    main()
