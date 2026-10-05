#!/usr/bin/env python3
"""Check harness process ownership/completion, without emulating a Vita."""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAMEPLAY_RUNNER = ROOT / "tools/vita-harness/check_system12_gameplay_vita3k.py"
FAKE_HOST = r'''#!/usr/bin/env python3
from pathlib import Path
import os
import sys
import time
mode = Path(sys.argv[0]).name
print(f"owned host pid={os.getpid()}", flush=True)
print("vitaGL-shutdown stage=stop-gc.purge.begin", flush=True)
print("Vita3K protection miss #1 vaddr=0x60800050 tree_count=3", flush=True)
if "--" in sys.argv:
    print("[install_archive_content]: test [PCSXN0012] installed successfully!", flush=True)
    # Deliberately include an old PASS: it must not validate the next launch.
    print("pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs=3", flush=True)
    print("Stopping -> Idle", flush=True)
else:
    if mode.startswith("forwarded"):
        print("Dynarmic forwarding EXC_BAD_ACCESS outside JIT registry pc=0x1234", flush=True)
        if mode == "forwarded-repeat-partial":
            print("Dynarmic forwarding EXC_BAD_ACCESS outside JIT registry pc=0x1234", flush=True)
        if mode in ("forwarded-matched", "forwarded-critical"):
            print("Vita3K recovered protected access pc=0x00001234 addr=0x60001000 write=1", flush=True)
        elif mode == "forwarded-wrong-pc":
            print("Vita3K recovered protected access pc=0x1238 addr=0x60001000 write=1", flush=True)
        elif mode == "forwarded-repeat-partial":
            print("Vita3K recovered protected access pc=0x1234 addr=0x60001000 write=1", flush=True)
        if mode == "forwarded-critical":
            print("|C| untracked protection fallback", flush=True)
    if mode == "recovered-alone":
        print("Vita3K recovered protected access pc=0x1234 addr=0x60001000 write=0", flush=True)
    if mode.startswith("lifecycle"):
        if mode != "lifecycle-no-drc":
            print("pcsx-vita: cpu_driver=ari64", flush=True)
        if mode != "lifecycle-missing":
            state = "FAIL (-1)" if mode == "lifecycle-fail" else "PASS (0)"
            print(f"pcsx-vita: vitaGL shutdown {state}", flush=True)
    elif mode != "no-guest":
        runs = 30 if mode == "wrong-runs" else 3
        print(f"pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs={runs}", flush=True)
    if mode != "no-idle":
        print("Stopping -> Idle", flush=True)
    if mode == "forwarded-delayed":
        time.sleep(.15)
        print("Vita3K recovered protected access pc=0x1234 addr=0x60001000 write=1", flush=True)
    if mode == "forwarded-normal-exit":
        sys.exit(0)
    if mode == "crash":
        sys.exit(17)
    if mode == "fatal":
        print("Unhandled EXC_BAD_ACCESS", flush=True)
    if mode == "late-fatal":
        time.sleep(.15)
        print("|C| guest teardown failed", flush=True)
    if mode == "smoke-fail":
        print("pcsx-vita: smoke FAIL triangle=0000", flush=True)
    if mode == "readback-fail":
        print("pcsx-vita: vitaGL readback FAIL transfer-copy rc=-1", flush=True)
    if mode == "flood":
        for _ in range(30000):
            print("[InterpreterFallback] Unimplemented instruction at 0x81155B94")
        sys.stdout.flush()
    if mode == "normal-exit":
        sys.exit(0)
time.sleep(30)  # A completed guest leaves its host UI open.
'''


def main():
    spec = importlib.util.spec_from_file_location("system12_gameplay_vita3k", GAMEPLAY_RUNNER)
    gameplay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gameplay)
    assert gameplay.select_milestones() == (400, 800, 1800)
    assert gameplay.select_milestones(1800) == (1800,)
    assert gameplay.select_milestones(None, True) == (12000,)
    try:
        gameplay.select_milestones(1800, True)
    except ValueError as error:
        assert "cannot be combined" in str(error)
    else:
        raise AssertionError("--frames and --gameplay-replay should conflict")

    with tempfile.TemporaryDirectory(prefix="pcsx-vita3k-runner-") as tmp:
        root = Path(tmp)
        trace_host = root / "lifecycle-trace"
        trace_host.write_text(FAKE_HOST)
        trace_host.chmod(0o700)
        trace_log = root / "shutdown-trace.log"
        passed, issues = gameplay.run_guest(trace_host, "test", trace_log, 2,
                                            ("pcsx-vita: vitaGL shutdown PASS (0)",))
        assert passed and not issues
        assert "vitaGL-shutdown stage=stop-gc.purge.begin" in trace_log.read_text()
        assert "Vita3K protection miss #1 vaddr=0x60800050 tree_count=3" in trace_log.read_text()
        result_path = root / "results.json"
        gameplay.write_results(result_path, {"renderer": "vitagl"}, [
            {"frames": 400, "status": "completed", "runtime_issues": {}},
            {"frames": 800, "status": "completed", "runtime_issues": {}},
            {"frames": 1800, "status": "failed",
             "runtime_issues": {"pcsx-vita: vitaGL shutdown FAIL": 1},
             "error": "RuntimeError: System12 guest run failed at 1800 frames"},
        ], [400, 800], "failed", {"frames": 1800,
                                  "error": "RuntimeError: System12 guest run failed at 1800 frames"})
        aggregate = json.loads(result_path.read_text())
        assert aggregate["status"] == "failed"
        assert aggregate["completed_milestones"] == [400, 800]
        assert aggregate["failed_milestone"]["frames"] == 1800
        assert aggregate["milestones"][-1]["runtime_issues"]
        assert aggregate["host_execution_issues"] is True

        for mode, install, expected in (
                ("idle-host", False, 0),
                ("crash", False, 1),
                ("fatal", False, 1),
                ("late-fatal", False, 1),
                ("forwarded-matched", False, 0),
                ("forwarded-unresolved", False, 1),
                ("forwarded-wrong-pc", False, 1),
                ("forwarded-critical", False, 1),
                ("forwarded-repeat-partial", False, 1),
                ("forwarded-delayed", False, 0),
                ("forwarded-normal-exit", False, 1),
                ("recovered-alone", False, 0),
                ("smoke-fail", False, 1),
                ("readback-fail", False, 1),
                ("wrong-runs", False, 1),
                ("normal-exit", False, 0),
                ("flood", False, 1),
                ("lifecycle-ok", False, 0),
                ("lifecycle-fail", False, 1),
                ("lifecycle-missing", False, 1),
                ("lifecycle-no-drc", False, 1),
                ("no-idle", False, 1),
                ("idle-host", True, 0),
                ("no-guest", True, 1)):
            emulator = root / mode
            emulator.write_text(FAKE_HOST)
            emulator.chmod(0o700)
            log = root / f"{mode}-install{install}.log"
            command = [sys.executable, str(ROOT / "tools/vita-harness/check_vita3k.py"),
                       "--emulator", str(emulator), "--log", str(log), "--timeout", "2"]
            if install:
                vpk = root / "package.vpk"
                vpk.write_bytes(b"test package argument")
                command += ["--vpk", str(vpk)]
            if mode.startswith("lifecycle"):
                command += ["--renderer", "vitagl", "--check-lifecycle"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            assert result.returncode == expected, (mode, install, result.stdout, result.stderr)
            assert log.stat().st_size < 3 * 1024 * 1024
            pids = [int(line.split("=")[1]) for line in log.read_text().splitlines()
                    if line.startswith("owned host pid=")]
            assert len(pids) == (2 if install else 1)
            for pid in pids:
                # The runner must reap only the host processes it launched.
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    continue
                raise AssertionError(f"owned test host still running: {pid}")
    print("Vita3K runner completion, crash, timeout and installation isolation PASS")


if __name__ == "__main__":
    main()
