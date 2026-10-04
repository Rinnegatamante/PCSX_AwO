#!/usr/bin/env python3
"""Capture the MAME System 12 H8/3002 register/peripheral boot oracle."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame',default='mame')
    parser.add_argument('--rom',type=Path,default=ROOT/'roms/soulclbr.zip')
    parser.add_argument('--output',type=Path,default=ROOT/'artifacts/mame')
    args=parser.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='soulclbr-h8-') as directory:
        temp=Path(directory);shutil.copyfile(args.rom,temp/'soulclbrac.zip')
        command=[args.mame,'soulclbrac','-noreadconfig','-rompath',str(temp),'-cfg_directory',str(temp/'cfg'),'-nvram_directory',str(temp/'nvram'),'-autoboot_script',str(ROOT/'tests/mame_h8_oracle.lua'),'-autoboot_delay','0','-skip_gameinfo','-video','none','-sound','none','-nothrottle','-seconds_to_run','21']
        result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
        (output/'h8-oracle.log').write_text('$ '+' '.join(command)+'\n'+result.stdout+f'\nexit_code={result.returncode}\n')
        print(result.stdout);result.check_returncode()
        if 'LUA ERROR' in result.stdout or 'H8_ORACLE_PASS' not in result.stdout:raise RuntimeError('H8 oracle failed; inspect h8-oracle.log')
if __name__=='__main__':main()
