#!/usr/bin/env python3
"""Check diagnostic sampling and cycle rollover using a deterministic clock."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "system12_profile.h"
#include <time.h>
#include <assert.h>
static unsigned ticks;
int profile_test_clock_gettime(clockid_t id, struct timespec *t) {
    (void)id;
    ticks++;
    t->tv_sec = ticks / 10000;
    t->tv_nsec = (ticks % 10000) * 100000;
    return 0;
}
static void run_epoch(void) {
    uint32_t cycle = 0xfffffff0;
    uint64_t h8 = 100, pcm = 10;
    unsigned i;
    assert(pcsx_profile_begin(PCSX_PROF_CPU, 1) == 0);
    for (i = 0; i < 600; i++) {
        uint64_t start;
        pcsx_profile_frame_begin(cycle, h8, pcm);
        assert(pcsx_profile_begin((enum pcsx_profile_section)-1, 1) == 0);
        assert(pcsx_profile_begin(PCSX_PROF_COUNT, 1) == 0);
        assert(pcsx_profile_begin(PCSX_PROF_CPU, 0) == 0);
        start = pcsx_profile_begin(PCSX_PROF_CPU, 1);
        pcsx_profile_end(PCSX_PROF_CPU, start);
        start = pcsx_profile_begin(PCSX_PROF_GPU_LIST, 31);
        pcsx_profile_end(PCSX_PROF_GPU_LIST, start);
        cycle += 500000;
        h8 += 169344;
        pcm += 882;
        pcsx_profile_frame_end(cycle, h8, pcm, 50000000, 0);
    }
}
int main(void) {
    run_epoch();
    pcsx_profile_frame_begin(100, 100, 100);
    uint64_t started = pcsx_profile_begin(PCSX_PROF_CPU, 1);
    pcsx_profile_reset(); /* reset between begin/end must discard this frame */
    pcsx_profile_end(PCSX_PROF_CPU, started);
    pcsx_profile_frame_end(0, 0, 0, 50000000, 0);
    run_epoch();
    return 0;
}
'''
DISABLED = r'''
#include "system12_profile.h"
#include <assert.h>
int main(void) {
    assert(pcsx_profile_begin(PCSX_PROF_CPU, 1) == 0);
    pcsx_profile_end(PCSX_PROF_CPU, 123);
    pcsx_profile_reset();
    return 0;
}
'''


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="pcsx-profile-test-") as tmp:
        temp = Path(tmp)
        source = temp / "test.c"
        exe = temp / "test"
        source.write_text(SOURCE)
        subprocess.run(cc + ["-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-DPCSX_SYSTEM12_PROFILE",
                            "-Dclock_gettime=profile_test_clock_gettime",
                            "-I" + str(ROOT / "libpcsxcore"), str(source),
                            str(ROOT / "libpcsxcore/system12_profile.c"),
                            "-o", str(exe)], check=True)
        output = subprocess.check_output([str(exe)], text=True)
        assert "psx_cycles=300000000 main_cycle_hz=50000000" in output, output
        assert "h8_cycles=101606400 h8_hz=16934400" in output, output
        assert "pcm_samples=529200 pcm_hz=88200 modes=600,0,0,0" in output, output
        assert "section=cpu_inclusive calls=600 samples=600 stride=1 sample_us=60000 estimated_us=60000" in output, output
        assert "section=gpu_list calls=600 samples=20 stride=31 sample_us=2000 estimated_us=60000" in output, output
        assert output.count("psx_cycles=300000000") == 2, output
        assert "epoch=0" in output and "epoch=1" in output, output
        source.write_text(DISABLED)
        # Disabled instrumentation must link without the profiling module.
        subprocess.run(cc + ["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "libpcsxcore"), str(source),
                            "-o", str(exe)], check=True)
        assert subprocess.check_output([str(exe)]) == b""
    print("System12 profiler: rollover, reset epochs, sampling, invalid scopes and disabled build PASS")


if __name__ == "__main__":
    main()
