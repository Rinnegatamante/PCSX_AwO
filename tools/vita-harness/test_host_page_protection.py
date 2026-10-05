#!/usr/bin/env python3
"""Build and run the host-page regression against a privately patched Vita3K mem library.

Requires an existing macOS CMake build so the exact local include flags,
untouched mem objects, and link dependencies can be reused. No source/build
checkout files are modified. Pass --sanitize to compile and link the touched
code and test with AddressSanitizer and UndefinedBehaviorSanitizer.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='Vita3K source checkout')
parser.add_argument('--build', type=Path, required=True, help='Existing CMake build directory')
parser.add_argument('--sanitize', action='store_true', help='Enable ASan and UBSan')
parser.add_argument('--baseline', action='store_true', help='Run against unpatched mem.cpp and require the regression to fail')
parser.add_argument('--trace-recovery', action='store_true', help='Apply private Apple ARM64 signal-handler recovery trace patch')
parser.add_argument('--test-stale-faults', action='store_true',
                    help='Run the concurrent stale-fault and true-metadata-miss regression')
parser.add_argument('--baseline-stale-faults', action='store_true',
                    help='Require the host-page-patched baseline to log the concurrent stale fault')
args = parser.parse_args()
source = args.source.resolve()
build = args.build.resolve()
mem_build = build / 'vita3k/mem'
patch = HERE / 'patches/vita3k-host-page-protection.patch'
test_src = HERE / 'tests/host_page_protection.cpp'
recovery_patch = HERE / 'patches/vita3k-macos-recovery-trace.patch'
stale_fault_patch = HERE / 'patches/vita3k-stale-protection-fault.patch'
if args.baseline and args.trace_recovery:
    parser.error('--baseline and --trace-recovery cannot be combined')
if args.baseline and (args.test_stale_faults or args.baseline_stale_faults):
    parser.error('--baseline cannot be combined with stale-fault regression modes')
if args.test_stale_faults and args.baseline_stale_faults:
    parser.error('choose either --test-stale-faults or --baseline-stale-faults')
if (args.test_stale_faults or args.baseline_stale_faults) and sys.platform != 'darwin':
    parser.error('stale-fault regression modes require macOS Mach VM APIs')
for required in (source / 'vita3k/mem/src/mem.cpp', mem_build / 'CMakeFiles/mem.dir/link.txt',
                 mem_build / 'CMakeFiles/mem-tests.dir/link.txt'):
    if not required.is_file():
        parser.error(f'missing expected input: {required}')


def make_vars(path):
    values = {}
    for line in path.read_text().splitlines():
        if ' = ' in line and not line.lstrip().startswith('#'):
            key, value = line.split(' = ', 1)
            values[key] = value
    return values


with tempfile.TemporaryDirectory(prefix='vita3k-host-page-test-') as temp_name:
    temp = Path(temp_name)
    private_source = temp / 'source'
    patched_mem = private_source / 'vita3k/mem/src/mem.cpp'
    patched_mem.parent.mkdir(parents=True)
    shutil.copy2(source / 'vita3k/mem/src/mem.cpp', patched_mem)
    if not args.baseline:
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=private_source, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=private_source, check=True)
    if args.trace_recovery:
        subprocess.run(['git', 'apply', '--check', str(recovery_patch)], cwd=private_source, check=True)
        subprocess.run(['git', 'apply', str(recovery_patch)], cwd=private_source, check=True)
    if args.test_stale_faults or args.baseline_stale_faults:
        subprocess.run(['git', 'apply', '--check', str(stale_fault_patch)], cwd=private_source, check=True)
        subprocess.run(['git', 'apply', str(stale_fault_patch)], cwd=private_source, check=True)

    mem_vars = make_vars(mem_build / 'CMakeFiles/mem.dir/flags.make')
    test_vars = make_vars(mem_build / 'CMakeFiles/mem-tests.dir/flags.make')
    sanitizer = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    patched_obj = temp / 'mem.cpp.o'
    test_obj = temp / 'host_page_protection.o'
    exe = temp / 'host-page-protection-test'

    def compile_cmd(vars_, source_path, output):
        return ['/usr/bin/c++', *shlex.split(vars_['CXX_DEFINES']),
                *shlex.split(vars_['CXX_INCLUDES']), *shlex.split(vars_['CXX_FLAGS']),
                *(['-DVITA3K_MEM_STALE_FAULT_TEST']
                  if args.test_stale_faults or args.baseline_stale_faults else []),
                *(['-DVITA3K_MEM_STALE_FAULT_BASELINE'] if args.baseline_stale_faults else []),
                *sanitizer, '-c', str(source_path), '-o', str(output)]

    subprocess.run(compile_cmd(mem_vars, patched_mem, patched_obj), cwd=build, check=True)
    subprocess.run(compile_cmd(test_vars, test_src, test_obj), cwd=build, check=True)

    mem_link = shlex.split((mem_build / 'CMakeFiles/mem.dir/link.txt').read_text())
    objects = [Path(word) for word in mem_link if word.endswith('.o')]
    archive = temp / 'libmem-host-page-test.a'
    subprocess.run(['/usr/bin/ar', 'qc', str(archive), *[
        str(patched_obj) if obj.name == 'mem.cpp.o' else str(mem_build / obj)
        for obj in objects]], cwd=mem_build, check=True)
    subprocess.run(['/usr/bin/ranlib', str(archive)], check=True)

    test_link = shlex.split((mem_build / 'CMakeFiles/mem-tests.dir/link.txt').read_text())
    rewritten = []
    injected = False
    skip_test_objects = {
        'allocator_tests.cpp.o', 'guest_mapping_tests.cpp.o'
    }
    for word in test_link:
        if word in ('libmem.a', '../../vita3k/mem/libmem.a') or word.endswith('/libmem.a'):
            rewritten.append(str(archive))
        elif word.endswith('.o') and Path(word).name in skip_test_objects:
            if not injected:
                rewritten.append(str(test_obj))
                injected = True
        elif word == 'mem-tests':
            continue
        elif word == '-o':
            rewritten.extend(['-o', str(exe)])
        else:
            rewritten.append(word)
    if not injected:
        raise RuntimeError('could not replace CMake test object inputs')
    if args.sanitize:
        rewritten[1:1] = sanitizer
    subprocess.run(rewritten, cwd=mem_build, check=True)

    environment = os.environ.copy()
    if args.sanitize:
        environment['ASAN_OPTIONS'] = 'detect_leaks=0'
    result = subprocess.run([str(exe)], cwd=mem_build, env=environment,
                            text=True, capture_output=True, timeout=20)
    if args.baseline:
        if result.returncode == 0 or 'FAIL: same host page shares one protection segment' not in result.stderr:
            print(result.stdout, end='')
            print(result.stderr, end='')
            raise RuntimeError('unpatched baseline did not fail at the expected regression assertion')
        print('unpatched baseline fails at the expected host-page segment assertion')
    elif args.baseline_stale_faults:
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, [str(exe)])
        message = 'Unhandled write protected region was valid.'
        if result.stdout.count('STALE_FAULT_RACE_PASS') != 2 or result.stdout.count('STALE_FAULT_TRUE_MISS_PASS') != 1:
            raise RuntimeError('baseline did not complete both stale-fault scenarios')
        if (result.stdout + result.stderr).count(message) != 3:
            raise RuntimeError('host-page baseline did not log both the stale fault and true metadata miss')
        print('host-page baseline logs the stale concurrent fault and the true metadata miss')
    else:
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, [str(exe)])
        if args.trace_recovery:
            import re
            recovered = re.findall(
                r'Vita3K recovered protected access pc=0x[0-9a-f]+ '
                r'addr=0x[0-9a-f]+ write=[01]', result.stderr)
            if not recovered:
                raise RuntimeError('fault test passed but emitted no successful recovery trace')
            print(f'recovery trace records={len(recovered)}')
        if args.test_stale_faults:
            message = 'Unhandled write protected region was valid.'
            if result.stdout.count('STALE_FAULT_RACE_PASS') != 2 or result.stdout.count('STALE_FAULT_TRUE_MISS_PASS') != 1:
                raise RuntimeError('stale-fault regression did not complete both scenarios')
            if (result.stdout + result.stderr).count(message) != 1:
                raise RuntimeError('stale-fault fix must suppress only the duplicate miss and retain the true-miss critical')
            print('stale-fault regression suppresses the duplicate miss and retains the true-miss critical')
