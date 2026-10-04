#!/usr/bin/env python3
"""Capture and compare one no-input Soul Calibur C352 replay in MAME and PCSX.

No ROMs are fetched. MAME gets a fresh per-output-directory NVRAM/config root.
The comparison is diagnostic (the PCSX System 12 CPU clock is not cycle-matched
with MAME), not a bit-exact conformance threshold.
"""
from __future__ import annotations
import argparse
import json
import math
import os
import time
from array import array
from pathlib import Path
import subprocess
import sys
import wave

SET = "soulclbrac"

def run_checked(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)

def capture(args: argparse.Namespace, output: Path) -> dict:
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = output / f"run-{time.time_ns()}-{os.getpid()}"
    root.mkdir()
    for d in ("nvram", "cfg", "ini", "snapshots"):
        (root / d).mkdir(parents=True, exist_ok=True)
    mame_wav = root / f"mame-{SET}-{args.seconds}s.wav"
    native_wav = root / f"native-{args.seconds}s.wav"
    frame = root / f"native-{args.seconds}s.ppm"
    run_checked([
        str(args.mame), SET, "-rompath", str(args.rompath), "-skip_gameinfo",
        "-video", "none", "-samplerate", "48000", "-seconds_to_run", str(args.seconds),
        "-wavwrite", str(mame_wav), "-nvram_directory", str(root / "nvram"),
        "-cfg_directory", str(root / "cfg"), "-inipath", str(root / "ini"),
        "-snapshot_directory", str(root / "snapshots"),
    ], cwd=args.repo)
    run_checked([
        str(args.harness), "--system12", str(args.regions), "--frames", str(args.seconds * 60),
        "--audio", str(native_wav), "--output", str(frame),
    ], cwd=args.repo)

    result = compare(mame_wav, native_wav, args.seconds)
    result.update({
        "mame_set": SET,
        "mame_wav": str(mame_wav),
        "native_wav": str(native_wav),
        "native_frame": str(frame),
        "input_replay": "no host inputs in either emulator",
        "note": "diagnostic only; different host sample rates and non-matched PSX CPU timing prevent bit-exact comparison",
    })
    (root / "audio-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return result

def read_wav(path: Path) -> tuple[int, array]:
    with wave.open(str(path), "rb") as w:
        params = (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getcomptype())
        if params[0] != 2 or params[1] != 2 or params[3] != "NONE":
            raise ValueError(f"{path}: expected stereo PCM16 WAV, got {params}")
        raw = w.readframes(w.getnframes())
        rate, frames = w.getframerate(), w.getnframes()
    samples = array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != frames * 2:
        raise ValueError(f"{path}: malformed PCM frame count")
    return rate, samples

def channel_stats(samples: array) -> dict:
    sums = [0, 0]
    peak = [0, 0]
    clipped = [0, 0]
    nonzero = [0, 0]
    frames = len(samples) // 2
    for i in range(frames):
        for ch in range(2):
            x = samples[2*i + ch]
            sums[ch] += x*x
            peak[ch] = max(peak[ch], abs(x))
            clipped[ch] += x == -32768 or x == 32767
            nonzero[ch] += x != 0
    return {"frames": frames,
            "rms": [round(math.sqrt(x / frames), 4) if frames else 0 for x in sums],
            "peak": peak, "clipped_samples": clipped, "nonzero_samples": nonzero}

def correlation(a: array, arate: int, b: array, brate: int, frames: int) -> list[float]:
    # Linear interpolation only removes nominal-rate difference; it does not
    # compensate for game-state or scheduler drift.
    frames = min(frames, len(a)//2, int((len(b)//2 - 1) * arate / brate))
    sums_a = [0.0, 0.0]; sums_b = [0.0, 0.0]
    sums_aa = [0.0, 0.0]; sums_bb = [0.0, 0.0]; sums_ab = [0.0, 0.0]
    for i in range(frames):
        pos_num = i * brate
        lo, rem = divmod(pos_num, arate)
        f = rem / arate
        for ch in range(2):
            av = float(a[2*i+ch])
            bv = b[2*lo+ch] * (1.0-f) + b[2*(lo+1)+ch] * f
            sums_a[ch] += av; sums_b[ch] += bv
            sums_aa[ch] += av*av; sums_bb[ch] += bv*bv; sums_ab[ch] += av*bv
    out=[]
    for ch in range(2):
        cov=sums_ab[ch]-sums_a[ch]*sums_b[ch]/frames
        va=sums_aa[ch]-sums_a[ch]*sums_a[ch]/frames
        vb=sums_bb[ch]-sums_b[ch]*sums_b[ch]/frames
        out.append(round(cov/math.sqrt(va*vb), 6) if va>0 and vb>0 else None)
    return out

def compare(mame_path: Path, native_path: Path, seconds: int) -> dict:
    mr, m = read_wav(mame_path); nr, n = read_wav(native_path)
    if mr != 48000 or nr != 88200:
        raise ValueError(f"expected MAME 48 kHz/native 88.2 kHz, got {mr}/{nr}")
    ms, ns = channel_stats(m), channel_stats(n)
    expected_m, expected_n = seconds*mr, seconds*nr
    if abs(ms["frames"]-expected_m)>2 or abs(ns["frames"]-expected_n)>2:
        raise ValueError(f"unexpected capture duration: {ms['frames']}/{ns['frames']} expected {expected_m}/{expected_n}")
    if not any(ms["nonzero_samples"]) or not any(ns["nonzero_samples"]):
        raise ValueError("one capture is silent; compare cannot validate active playback")
    end=min(ms["frames"], int((ns["frames"]-1)*mr/nr))
    first=min(end, mr*min(seconds,5))
    return {"mame": {"rate": mr, **ms}, "native": {"rate": nr, **ns},
            "native_to_mame_rms_ratio": [round(ns["rms"][i]/ms["rms"][i],5) if ms["rms"][i] else None for i in range(2)],
            "resampled_linear_pearson_r": {
                "first_up_to_5s": correlation(m,mr,n,nr,first),
                "whole_overlap": correlation(m,mr,n,nr,end)},
            "comparison_method": "native front L/R PCM16 linearly resampled from 88.2 kHz to MAME 48 kHz; no lag search or level fitting"}

def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--mame", type=Path, default=Path("../mamedev/mame"))
    p.add_argument("--rompath", type=Path, default=Path("roms"))
    p.add_argument("--regions", type=Path, default=Path("artifacts/system12"))
    p.add_argument("--harness", type=Path, default=Path("build/mac-harness/pcsx-harness"))
    p.add_argument("--output", type=Path, default=Path("artifacts/system12/audio-reference"))
    p.add_argument("--seconds", type=int, default=5)
    args=p.parse_args()
    if args.seconds < 1 or args.seconds > 3600:
        p.error("--seconds must be between 1 and 3600")
    for attr in ("mame", "rompath", "regions", "harness"):
        path=getattr(args,attr)
        if not path.is_absolute(): setattr(args,attr,(args.repo/path).resolve())
    if not (args.regions/"wave.bin").is_file() or not args.harness.is_file() or not args.mame.is_file():
        p.error("missing prepared System 12 wave.bin, harness, or MAME executable")
    manifest_path = args.regions / "manifest.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("mame_set") != SET:
            p.error(f"expected prepared {SET} regions, got {manifest.get('mame_set')}")
    output=args.output if args.output.is_absolute() else args.repo/args.output
    capture(args,output)
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError, wave.Error) as exc:
        print(f"system12 audio audit failed: {exc}",file=sys.stderr)
        raise SystemExit(1)
