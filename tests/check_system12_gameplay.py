#!/usr/bin/env python3
"""Run a deterministic Soul Calibur System 12 input replay against MAME and PCSX.

The replay produces evidence and device diagnostics.  It is not an assertion
that every screen or game state is correct merely because video/audio exists.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import time
import wave
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "artifacts/system12-profile"
MAME_SOURCE = ROOT.parent / "mamedev"
ROM = ROOT / "roms/soulclbr.zip"
SET = "soulclbrac"
MILESTONES = (1, 1800, 3600, 6000, 9000, 12000)

# Identical frame/control/value events are emitted to PCSX CSV and MAME Lua.
def replay_events(frames: int) -> list[tuple[int, str, int]]:
    events: list[tuple[int, str, int]] = []

    def pulse(at: int, control: str, width: int = 8) -> None:
        if at < frames:
            events.append((at, control, 1))
            events.append((min(at + width, frames - 1), control, 0))

    # Reach the attract screen first (the local MAME reference shows it by 30s),
    # then add credit and start a player game.
    pulse(1800, "coin1", 2)
    pulse(1810, "p1start", 60)
    pulse(1880, "p1button1", 8)
    pulse(1896, "p1button3", 8)

    directions = ("p1left", "p1right", "p1up", "p1down")
    attacks = ("p1button1", "p1button2", "p1button3", "p1button4")
    for index, base in enumerate(range(1980, max(1980, frames - 240), 360)):
        pulse(base, directions[index % len(directions)], 105)
        pulse(base + 28, attacks[index % len(attacks)], 8)
        pulse(base + 72, attacks[(index + 1) % len(attacks)], 8)

    # Leave a known active-low button state in the terminal diagnostic so the
    # test also proves the final input event reached the board input adapter.
    if frames > 1:
        events.append((frames - 1, "p1button3", 1))
    events.sort(key=lambda row: (row[0], row[1], row[2]))
    return events


MAME_FIELDS = {
    "coin1": (":JVS_COIN1", "Coin 1"),
    "p1start": (":JVS_PLAYER1", "1 Player Start"),
    "p1left": (":JVS_PLAYER1", "P1 Left"),
    "p1right": (":JVS_PLAYER1", "P1 Right"),
    "p1up": (":JVS_PLAYER1", "P1 Up"),
    "p1down": (":JVS_PLAYER1", "P1 Down"),
    "p1button1": (":JVS_PLAYER1", "P1 Button 1"),
    "p1button2": (":JVS_PLAYER1", "P1 Button 2"),
    "p1button3": (":JVS_PLAYER1", "P1 Button 3"),
    "p1button4": (":JVS_PLAYER1", "P1 Button 4"),
}


def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_wav(path: Path) -> dict:
    with wave.open(str(path), "rb") as wav:
        channels, width, rate, frame_count = (wav.getnchannels(), wav.getsampwidth(),
                                               wav.getframerate(), wav.getnframes())
        if channels != 2 or width != 2 or frame_count <= 0:
            raise RuntimeError(f"unexpected WAV format in {path}: {channels}ch/{width}B/{rate}Hz")
        peak = 0
        nonzero = False
        while True:
            block = wav.readframes(65536)
            if not block:
                break
            samples = struct.unpack("<%dh" % (len(block) // 2), block)
            if samples:
                peak = max(peak, max(abs(sample) for sample in samples))
                nonzero = nonzero or any(samples)
        if not nonzero:
            raise RuntimeError(f"WAV contains only silence: {path}")
    return {"channels": channels, "sample_width": width, "sample_rate": rate,
            "frames": frame_count, "peak": peak, "bytes": path.stat().st_size}


def inspect_ppm(path: Path) -> dict:
    data = path.read_bytes()
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not match:
        raise RuntimeError(f"invalid P6/255 PPM: {path}")
    width, height = map(int, match.groups())
    if len(data) != match.end() + width * height * 3:
        raise RuntimeError(f"truncated or oversized PPM payload: {path}")
    return {"width": width, "height": height, "bytes": len(data)}


def verify_profile() -> dict:
    manifest = json.loads((PROFILE / "system12_manifest.json").read_text(encoding="utf-8"))
    cfg = dict(line.split("=", 1) for line in (PROFILE / "board.cfg").read_text().splitlines()
               if "=" in line)
    if manifest.get("mame_set") != SET or cfg.get("mame_set") != SET:
        raise RuntimeError(f"expected {SET} profile at {PROFILE}")
    if manifest.get("mame_source_commit") != "deb2f54fe7300e7a192690318beb860159bfd52a":
        raise RuntimeError("System 12 profile is not from the audited pinned MAME commit")
    if cfg.get("machine") != "coh700" or cfg.get("ram_size") != "4194304":
        raise RuntimeError("local package does not describe the expected COH700 board")
    if manifest.get("board_execution_verified") is not False:
        raise RuntimeError("media manifest unexpectedly claims board execution is verified")
    for name, facts in manifest["regions"].items():
        path = PROFILE / name
        if not path.is_file() or path.stat().st_size != facts["size"]:
            raise RuntimeError(f"profile region missing or wrong size: {path}")
        crc = 0
        with path.open("rb") as src:
            for chunk in iter(lambda: src.read(1024 * 1024), b""):
                crc = zlib.crc32(chunk, crc)
        if f"{crc & 0xffffffff:08x}" != facts["crc32"] or sha1(path) != facts["sha1"]:
            raise RuntimeError(f"profile region digest mismatch: {path}")
    actual_commit = subprocess.run(["git", "-C", str(MAME_SOURCE), "rev-parse", "HEAD"],
                                   check=True, capture_output=True, text=True).stdout.strip()
    if actual_commit != manifest["mame_source_commit"]:
        raise RuntimeError("local MAME source checkout does not match the profile commit")
    return manifest


def write_events(path: Path, events: list[tuple[int, str, int]]) -> None:
    path.write_text("frame,control,pressed\n" + "".join(
        f"{frame},{control},{pressed}\n" for frame, control, pressed in events), encoding="ascii")


def write_mame_script(path: Path, events: list[tuple[int, str, int]], frames: int) -> None:
    lua_events = []
    for frame, control, value in events:
        port, field = MAME_FIELDS[control]
        lua_events.append(
            "{%d,%s,%s,%d}" % (frame, json.dumps(port), json.dumps(field), value))
    milestones = ",".join(str(frame) for frame in MILESTONES if frame <= frames)
    source = f'''-- Generated deterministically by tests/check_system12_gameplay.py.
local machine = manager.machine
local events = {{{",".join(lua_events)}}}
local milestones = {{{milestones}}}
local next_event, frame = 1, 0
local screen = machine.screens[":screen"]
local h8 = machine.devices[":sub"].state
local function apply_event(event)
    local port = machine.ioport.ports[event[2]]
    if not port or not port.fields[event[3]] then
        error("missing MAME input binding: " .. event[2] .. " / " .. event[3])
    end
    port.fields[event[3]]:set_value(event[4])
end
while next_event <= #events and events[next_event][1] == 0 do
    apply_event(events[next_event]); next_event = next_event + 1
end
local milestone_index = 1
emu.register_frame_done(function()
    frame = frame + 1
    while next_event <= #events and events[next_event][1] <= frame do
        apply_event(events[next_event]); next_event = next_event + 1
    end
    if milestone_index <= #milestones and frame >= milestones[milestone_index] then
        local filename = string.format("mame-%05d.png", frame)
        local result = screen:snapshot(filename)
        print(string.format("S12_MAME_CAPTURE frame=%d configured_refresh=%.12f frame_hz=%.12f period=%.12f height=%d h8pc=%06x image=%s result=%s",
            frame, screen.refresh, 1 / screen.frame_period, screen.frame_period,
            screen.height, h8["PC"].value, filename, tostring(result)))
        milestone_index = milestone_index + 1
    end
    if frame >= {frames} then
        print(string.format("S12_MAME_DONE frames=%d configured_refresh=%.12f frame_hz=%.12f period=%.12f height=%d h8pc=%06x player=%08x coin=%08x",
            frame, screen.refresh, 1 / screen.frame_period, screen.frame_period,
            screen.height, h8["PC"].value,
            machine.ioport.ports[":JVS_PLAYER1"]:read(), machine.ioport.ports[":JVS_COIN1"]:read()))
        machine:exit()
    end
end)
emu.wait({frames / 50 + 10})
if frame < {frames} then
    print(string.format("S12_MAME_ERROR early exit at frame %d", frame))
    machine:exit()
end
'''
    path.write_text(source, encoding="utf-8")


def run_mame(mame_bin: str, rom: Path, out: Path,
             events: list[tuple[int, str, int]], frames: int) -> dict:
    mame_rompath = out / "mame-rompath"
    mame_rompath.mkdir()
    shutil.copyfile(rom, mame_rompath / f"{SET}.zip")
    audit = subprocess.run([mame_bin, "-noreadconfig", "-rompath", str(mame_rompath),
                            "-verifyroms", SET], capture_output=True, text=True, timeout=180)
    (out / "mame-rom-audit.log").write_text(audit.stdout + audit.stderr, encoding="utf-8")
    if audit.returncode or "is good" not in audit.stdout + audit.stderr:
        raise RuntimeError("MAME rejected local Soul Calibur ROM; see mame-rom-audit.log")
    lua = out / "replay.lua"
    write_mame_script(lua, events, frames)
    command = [mame_bin, SET, "-noreadconfig", "-rompath", str(mame_rompath),
               "-cfg_directory", str(out / "mame-cfg"),
               "-nvram_directory", str(out / "mame-nvram"),
               "-input_directory", str(out / "mame-input"),
               "-snapshot_directory", str(out), "-wavwrite", str(out / "mame.wav"),
               "-autoboot_script", str(lua), "-autoboot_delay", "0",
               "-skip_gameinfo", "-video", "none", "-nothrottle"]
    started = time.monotonic()
    run = subprocess.run(command, capture_output=True, text=True,
                         timeout=max(180, int(frames / 25)))
    elapsed = time.monotonic() - started
    log = run.stdout + run.stderr
    (out / "mame-run.log").write_text("$ " + " ".join(command) + "\n" + log, encoding="utf-8")
    if run.returncode or "LUA ERROR" in log:
        raise RuntimeError("MAME replay failed; see mame-run.log")
    done = re.search(r"S12_MAME_DONE frames=(\d+) configured_refresh=([0-9.]+) frame_hz=([0-9.]+) period=([0-9.]+) height=(\d+) h8pc=([0-9a-f]+) player=([0-9a-f]+) coin=([0-9a-f]+)", log)
    if not done or int(done.group(1)) != frames:
        raise RuntimeError("MAME replay did not complete the requested frame count")
    if int(done.group(7), 16) & 0x8000 != 0x8000 or int(done.group(8), 16) != 0:
        raise RuntimeError("MAME terminal JVS input state did not match the shared replay")
    captured = sorted(out.glob("mame-*.png"))
    if not captured or any(image.stat().st_size < 1000 for image in captured):
        raise RuntimeError("MAME replay did not produce valid milestone screenshots")
    audio = inspect_wav(out / "mame.wav")
    return {"elapsed_seconds": elapsed, "frames": int(done.group(1)),
            "configured_refresh_hz": float(done.group(2)),
            "live_frame_hz": float(done.group(3)), "live_frame_period_s": float(done.group(4)),
            "height": int(done.group(5)), "h8_pc": done.group(6),
            "jvs_player1": done.group(7), "jvs_coin1": done.group(8),
            "captures": [image.name for image in captured],
            "wav": audio}


def run_pcsx(harness: Path, out: Path, events_path: Path, frames: int) -> dict:
    command = [str(harness.resolve()), "--system12", str(PROFILE), "--frames", str(frames),
               "--inputs", str(events_path), "--max-blocks", "10000000000",
               "--audio", str(out / "pcsx.wav"),
               "--output", str(out / "pcsx-final.ppm")]
    started = time.monotonic()
    run = subprocess.run(command, capture_output=True, text=True,
                         timeout=max(240, int(frames / 20)))
    elapsed = time.monotonic() - started
    log = run.stdout + run.stderr
    (out / "pcsx-run.log").write_text("$ " + " ".join(command) + "\n" + log, encoding="utf-8")
    if run.returncode:
        raise RuntimeError("PCSX replay failed; see pcsx-run.log")
    summary = re.search(r"frames=(\d+) cycles=(\d+).*?elapsed=([0-9.]+)s", log)
    h8 = re.search(r"System12: H8 pc=([0-9a-f]+).*?cycles=(\d+) instructions=(\d+) halted=(\d+) illegal=([0-9a-f]+):([0-9a-f]+) unsupported=([0-9a-f]+) shared r=(\d+) w=(\d+)", log)
    unmapped = re.search(r"System12: unmapped reads=(\d+) writes=(\d+)", log)
    jamma = re.search(r"System12: JAMMA=([0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2})", log)
    c352 = re.search(r"System12: C352 rate=(\d+) frames=(\d+) peak=(\d+) WAV_bytes=(\d+)", log)
    if not all((summary, h8, unmapped, jamma, c352)):
        raise RuntimeError("PCSX replay diagnostics are incomplete; see pcsx-run.log")
    if int(summary.group(1)) != frames:
        raise RuntimeError(f"PCSX replay stopped after {summary.group(1)} frames, expected {frames}")
    if (int(h8.group(3)) <= 0 or h8.group(5) != "000000" or h8.group(6) != "0000" or
            h8.group(7) != "0" or int(h8.group(8)) <= 0 or int(h8.group(9)) <= 0):
        raise RuntimeError("H8 had no activity or reported illegal/unsupported state")
    if tuple(map(int, unmapped.groups())) != (0, 0):
        raise RuntimeError("System 12 accessed an unmapped address; see pcsx-run.log")
    if jamma.group(1).split("/")[1] != "bf":
        raise RuntimeError(f"terminal input did not reach JAMMA adapter: {jamma.group(1)}")
    if int(c352.group(3)) <= 0 or int(c352.group(2)) <= 0:
        raise RuntimeError("C352 generated no nonzero audio samples")
    audio = inspect_wav(out / "pcsx.wav")
    if audio != {"channels": 2, "sample_width": 2, "sample_rate": 88200,
                 "frames": int(c352.group(2)), "peak": int(c352.group(3)),
                 "bytes": (out / "pcsx.wav").stat().st_size}:
        raise RuntimeError(f"unexpected PCSX audio output: {audio}")
    ppm = inspect_ppm(out / "pcsx-final.ppm")
    return {"elapsed_seconds": elapsed, "frames": int(summary.group(1)),
            "psx_cycles": int(summary.group(2)), "h8_pc": h8.group(1),
            "h8_cycles": int(h8.group(2)), "h8_instructions": int(h8.group(3)),
            "h8_shared_reads": int(h8.group(8)), "h8_shared_writes": int(h8.group(9)),
            "unmapped_reads": 0, "unmapped_writes": 0, "jamma": jamma.group(1),
            "c352_rate": int(c352.group(1)), "c352_frames": int(c352.group(2)),
            "c352_peak": int(c352.group(3)), "wav": audio, "ppm": ppm}


def main() -> int:
    global PROFILE, ROM
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    parser.add_argument("--mame", default="mame")
    parser.add_argument("--rom", type=Path, default=ROM)
    parser.add_argument("--profile", type=Path, default=PROFILE)
    parser.add_argument("--frames", type=int, default=12000)
    parser.add_argument("--output-dir", type=Path,
                        help="persistent evidence directory; default is a new OS temp directory")
    parser.add_argument("--skip-mame", action="store_true", help="run PCSX only")
    args = parser.parse_args()
    if args.frames < 3600 or args.frames > 216000:
        parser.error("--frames must be between 3600 and 216000")
    PROFILE, ROM = args.profile.resolve(), args.rom.resolve()
    if args.output_dir:
        out = args.output_dir.resolve()
        if out == ROOT or ROOT in out.parents:
            parser.error("keep replay artifacts outside the git worktree")
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix="pcsx-system12-gameplay-"))
    events = replay_events(args.frames)
    manifest = verify_profile()
    inputs = out / "inputs.csv"
    write_events(inputs, events)
    (out / "inputs.json").write_text(json.dumps(
        [{"frame": f, "control": c, "pressed": bool(v)} for f, c, v in events],
        indent=2) + "\n", encoding="utf-8")
    report = {"system": "Namco System 12", "set": SET, "mame_source_commit": manifest[
        "mame_source_commit"], "requested_frames": args.frames, "input_events": len(events),
        "claim": "device/protocol replay evidence only; game state and full playability are not inferred from nonblank video/audio"}
    if not args.skip_mame:
        report["mame"] = run_mame(args.mame, ROM, out, events, args.frames)
    report["pcsx"] = run_pcsx(args.harness, out, inputs, args.frames)
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    print(f"evidence directory: {out}")
    print("Review milestone screenshots manually; assertions cover media, replay completion, audio, H8, unmapped accesses, and input delivery only.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
