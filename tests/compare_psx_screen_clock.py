#!/usr/bin/env python3
"""Compare the standalone C screen clock with captured MAME attotime samples."""

import argparse
import ctypes
import json
import pathlib
import subprocess
import tempfile


ATTOSECONDS_PER_SECOND = 10**18
PERIODS = {
    "ntsc240": 16715113233669674,
    "ntsc480": 16683335451858734,
    "pal50": 20000000000000000,
}
REQUIRED_CASES = {
    "initial", "ntsc480", "pal50", "during_blank", "short_period",
    "gpu_reset", "soft_reset",
}


class Time(ctypes.Structure):
    _fields_ = [("seconds", ctypes.c_int64), ("attoseconds", ctypes.c_uint64)]


class Event(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("time", Time)]


class Clock(ctypes.Structure):
    _fields_ = [
        ("now", Time),
        ("last_start", Time),
        ("last_end", Time),
        ("next_start", Time),
        ("next_end", Time),
        ("frame_period", ctypes.c_uint64),
        ("configured", ctypes.c_int),
        ("in_vblank", ctypes.c_int),
        ("start_pending", ctypes.c_int),
        ("end_pending", ctypes.c_int),
    ]


def make_time(value):
    return Time(value["seconds"], value["attoseconds"])


def time_pair(value):
    return value.seconds, value.attoseconds


def add_attoseconds(value, delta):
    total = value[0] * ATTOSECONDS_PER_SECOND + value[1] + delta
    return divmod(total, ATTOSECONDS_PER_SECOND)


def display_time(value):
    return f"{value[0]}:{value[1]:018d}"


def build_library(repo, output):
    source = repo / "libpcsxcore/arcade/psx_screen_clock.c"
    include = repo / "libpcsxcore/arcade"
    subprocess.run(
        ["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-dynamiclib",
         "-fPIC", "-I", str(include), str(source), "-o", str(output)],
        check=True,
    )


def load_clock_library(path):
    lib = ctypes.CDLL(str(path))
    lib.psx_screen_clock_init.argtypes = [ctypes.POINTER(Clock)]
    lib.psx_screen_clock_init.restype = None
    lib.psx_screen_clock_configure.argtypes = [
        ctypes.POINTER(Clock), Time, ctypes.c_uint64, ctypes.POINTER(Event)
    ]
    lib.psx_screen_clock_configure.restype = ctypes.c_int
    lib.psx_screen_clock_advance_to.argtypes = [
        ctypes.POINTER(Clock), Time, ctypes.POINTER(Event)
    ]
    lib.psx_screen_clock_advance_to.restype = ctypes.c_int
    lib.psx_screen_clock_is_vblank.argtypes = [ctypes.POINTER(Clock), Time]
    lib.psx_screen_clock_is_vblank.restype = ctypes.c_int
    return lib


class Model:
    def __init__(self, lib):
        self.lib = lib
        self.clock = Clock()
        self.lib.psx_screen_clock_init(ctypes.byref(self.clock))
        result = self.configure((0, 0), PERIODS["ntsc240"])
        if result is not None:
            raise AssertionError("cold NTSC240 configure unexpectedly emitted an event")

    def advance(self, at):
        target = Time(*at)
        event = Event()
        while self.lib.psx_screen_clock_advance_to(
            ctypes.byref(self.clock), target, ctypes.byref(event)
        ):
            pass

    def configure(self, at, period):
        self.advance(at)
        event = Event()
        immediate = self.lib.psx_screen_clock_configure(
            ctypes.byref(self.clock), Time(*at), period, ctypes.byref(event)
        )
        return time_pair(event.time) if immediate else None

    def query(self, at):
        self.advance(at)
        start = time_pair(self.clock.next_start)
        end = time_pair(self.clock.last_end)
        if not self.lib.psx_screen_clock_is_vblank(ctypes.byref(self.clock), Time(*at)):
            end = add_attoseconds(end, self.clock.frame_period)
        return start, end


def period_for_gp1(word):
    mode = word & 0x0C
    if mode & 0x08:
        return "pal50"
    if mode & 0x04:
        return "ntsc480"
    return "ntsc240"


def check_pair(case_name, label, kind, expected, actual):
    expected_pair = (expected["seconds"], expected["attoseconds"])
    if expected_pair != actual:
        raise AssertionError(
            f"{case_name}:{label} {kind} mismatch: MAME={display_time(expected_pair)} "
            f"C={display_time(actual)}"
        )


def compare_case(name, case, lib):
    model = Model(lib)
    events = []
    for row in case["writes"]:
        at = time_pair(make_time(row["after"]))
        events.append((at, 0, "write", row))
    for row in case["samples"]:
        at = time_pair(make_time(row["now"]))
        events.append((at, 1, "sample", row))
    reset_rows = case["reset_events"]
    if not reset_rows or time_pair(make_time(reset_rows[0]["time"])) != (0, 0):
        raise AssertionError(f"{name}: initial reset epoch must be time zero")
    # The first device reset is at t=0 and follows the initial cold configure.
    check_reset(name, reset_rows[0], model)
    reset_index = 1
    for row in reset_rows[1:]:
        events.append((time_pair(make_time(row["time"])), 0, "reset", row))
    for at, _, kind, row in sorted(events, key=lambda item: (item[0], item[1])):
        if kind == "reset":
            model.configure(at, PERIODS["ntsc240"])
            check_reset(name, row, model)
            reset_index += 1
        elif kind == "write":
            word = row.get("word")
            write_kind = row["kind"]
            if write_kind == "soft_reset":
                # schedule_soft_reset requests a reset; the notifier records
                # when it actually happens. Replay that event, not the request.
                if len(reset_rows) != 2:
                    raise AssertionError(f"{name}: missing delivered soft reset")
            elif write_kind == "gpu_reset":
                model.configure(at, PERIODS["ntsc240"])
            elif write_kind == "gp1" and word is not None:
                if word == 0:
                    model.configure(at, PERIODS["ntsc240"])
                elif (word >> 24) == 0x08:
                    model.configure(at, PERIODS[period_for_gp1(word)])
        else:
            actual_start, actual_end = model.query(at)
            check_pair(name, row["label"], "start", row["start"], actual_start)
            check_pair(name, row["label"], "finish", row["finish"], actual_end)
    if reset_index != len(reset_rows):
        raise AssertionError(f"{name}: did not reproduce all {len(reset_rows)} MAME resets")


def check_reset(case_name, row, model):
    at = time_pair(make_time(row["time"]))
    start, end = model.query(at)
    check_pair(case_name, f"reset#{row['n']}", "start", row["start"], start)
    check_pair(case_name, f"reset#{row['n']}", "finish", row["finish"], end)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "oracle", nargs="?", default="/tmp/mame-psx-screen-clock.json",
        help="captured MAME JSON (default: /tmp/mame-psx-screen-clock.json)",
    )
    parser.add_argument(
        "--repo", default=pathlib.Path(__file__).resolve().parents[1],
        type=pathlib.Path, help="repository root",
    )
    args = parser.parse_args()
    oracle_path = pathlib.Path(args.oracle)
    oracle = json.loads(oracle_path.read_text())
    if oracle.get("source_revision_pin") != "deb2f54fe7300e7a192690318beb860159bfd52a":
        raise SystemExit("oracle source revision does not match the requested MAME pin")
    if not REQUIRED_CASES.issubset(oracle.get("cases", {})):
        raise SystemExit("oracle is missing required screen mode/reset cases")
    for artifact in ("rom", "fixture"):
        before = oracle.get(f"{artifact}_sha256_before")
        if not before or before != oracle.get(f"{artifact}_sha256_after"):
            raise SystemExit(f"oracle {artifact} hash is missing or changed")
    with tempfile.TemporaryDirectory(prefix="psx-screen-clock-") as temp_dir:
        library_path = pathlib.Path(temp_dir) / "libpsx_screen_clock.dylib"
        build_library(args.repo, library_path)
        lib = load_clock_library(library_path)
        for name, case in oracle["cases"].items():
            compare_case(name, case, lib)
            print(f"{name}: exact attotime match")
    print(f"all {len(oracle['cases'])} oracle cases matched exactly")


if __name__ == "__main__":
    main()
