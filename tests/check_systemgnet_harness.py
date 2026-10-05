#!/usr/bin/env python3
"""Exercise the native G-Net PSX bus with an executed MIPS fixture."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True, type=pathlib.Path)
    args = parser.parse_args()
    run = subprocess.run([str(args.harness), "--system-gnet-fixture", "--board-test"],
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=30)
    if run.returncode:
        raise RuntimeError(f"G-Net MIPS bus fixture failed ({run.returncode}):\n{run.stdout}")
    expected = (
        "SystemGNet MIPS fixture: PASS cpu=interpreter const=2 bank1_flash=1234",
        "SystemGNet set=coh3002t model=ZN2 ram=4194304 vram=2097152 gpu=type2",
        "reads=2 writes=1 unsupported=0 bank=1 fault=0 execution_verified=false",
    )
    missing = [token for token in expected if token not in run.stdout]
    if missing:
        raise RuntimeError(f"G-Net fixture report missing {missing}:\n{run.stdout}")
    print(run.stdout, end="")

    with tempfile.TemporaryDirectory(prefix="pcsx-systemgnet-invalid-") as temp:
        directory = pathlib.Path(temp)
        (directory / "board.ini").write_text("format=wrong\n", encoding="ascii")
        rejected = subprocess.run([str(args.harness), "--system-gnet", str(directory)],
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=10)
        if rejected.returncode == 0 or "invalid or unsupported G-Net board.ini" not in rejected.stdout:
            raise RuntimeError(f"G-Net loader accepted an invalid profile:\n{rejected.stdout}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
