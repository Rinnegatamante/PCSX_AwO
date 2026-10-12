import csv
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "build-vita/postfx"
BASELINE = {
    "crt": 162,
    "crt_bleeding": 162,
    "daltonic": 21,
    "fxaa": 72,
    "gamma": 8,
    "greyscale": 5,
    "hue": 31,
    "lcd3x": 41,
    "negative": 4,
    "sepia": 7,
    "sharp_grid": 39,
    "sharp_scanlines": 39,
    "vignette": 15,
}
rows = []
for name, before in BASELINE.items():
    shader = SHADERS / name / "frag.gxp"
    result = subprocess.run(["psp2shaderperf", "-stats", str(shader)],
                            capture_output=True, text=True, check=True)
    result_text = result.stdout + result.stderr
    cycles = re.search(r"Total estimated cost:\s*(\d+) cycles, (\S+(?: \S+)?)", result_text)
    reads = re.search(r"Texture reads:\s*(\d+) non-dependent, dependent:\s*(\d+) unconditional", result_text)
    regs = re.search(r"Register count:\s*(\d+) PAs,\s*(\d+) temps,\s*(\d+) SAs", result_text)
    assert cycles and reads and regs, (name, result_text)
    after = int(cycles.group(1))
    mode = cycles.group(2)
    nondependent, dependent = int(reads.group(1)), int(reads.group(2))
    assert "per-instance" not in mode, f"{name} is not in parallel USSE mode"
    assert dependent <= {"crt": 2, "crt_bleeding": 3, "fxaa": 6, "sharp_grid": 1, "sharp_scanlines": 1}.get(name, 0)
    if name not in ("hue",):
        assert after <= before, (name, before, after)
    rows.append({
        "effect": name,
        "baseline_cycles": before,
        "optimized_cycles": after,
        "relative_reduction_pct": round(100 * (before - after) / before, 1),
        "thread_mode": mode,
        "nondependent_reads": nondependent,
        "dependent_reads": dependent,
        "primary_registers": int(regs.group(1)),
        "temporary_registers": int(regs.group(2)),
        "secondary_registers": int(regs.group(3)),
    })
    print(f"{name:16s} {before:3d} -> {after:3d} cycles; "
          f"fetch {nondependent} nondep/{dependent} dep; {mode}")
out = ROOT / "build-vita/postfx-usse-results.csv"
with out.open("w", newline="", encoding="utf-8") as file:
    writer = csv.DictWriter(file, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)
print(f"Performance report saved: {out}")
