#!/usr/bin/env python3
"""Verify System12 upper texture/CLUT/VRAM semantics on installed MAME."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame', default='mame')
    parser.add_argument('--rom', type=Path, default=ROOT / 'roms/soulclbr.zip')
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/mame')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='soulclbr-gpu-') as directory:
        temp = Path(directory)
        shutil.copyfile(args.rom, temp / 'soulclbrac.zip')
        command = [args.mame, 'soulclbrac', '-noreadconfig', '-rompath', str(temp),
                   '-cfg_directory', str(temp / 'cfg'), '-nvram_directory', str(temp / 'nvram'),
                   '-autoboot_script', str(ROOT / 'tests/mame_gpu_vram_oracle.lua'),
                   '-autoboot_delay', '0', '-skip_gameinfo', '-video', 'none',
                   '-sound', 'none', '-nothrottle', '-seconds_to_run', '2']
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        log = result.stdout + result.stderr
        (args.output / 'gpu-vram-oracle.log').write_text(log)
        print(log)
        result.check_returncode()
        if 'LUA ERROR' in log or 'GPU_VRAM_ORACLE_PASS' not in log:
            raise RuntimeError('GPU oracle failed; see gpu-vram-oracle.log')

if __name__ == '__main__':
    main()
