#!/usr/bin/env python3
"""From repository root: python3 tests/check_mame_boot_state.py"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'artifacts/mame'
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='soulclbr-boot-state-') as temporary:
    temp = Path(temporary)
    shutil.copyfile(root / 'roms/soulclbr.zip', temp / 'soulclbrac.zip')
    command = ['mame', 'soulclbrac', '-noreadconfig', '-rompath', str(temp),
               '-cfg_directory', str(temp / 'cfg'), '-nvram_directory', str(temp / 'nvram'),
               '-autoboot_script', str(root / 'tests/mame_boot_state_oracle.lua'), '-autoboot_delay', '0',
               '-skip_gameinfo', '-video', 'none', '-sound', 'none', '-nothrottle',
               '-seconds_to_run', '2']
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    (out / 'boot-state.log').write_text('$ ' + ' '.join(command) + '\n' + result.stdout
                                      + f'\nexit_code={result.returncode}\n')
    print(result.stdout)
    result.check_returncode()
    if 'LUA ERROR' in result.stdout or 'MILESTONE requested=1.000000' not in result.stdout:
        raise RuntimeError('CPU oracle capture failed')
