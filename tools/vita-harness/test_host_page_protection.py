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
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='Vita3K source checkout')
parser.add_argument('--build', type=Path, required=True, help='Existing CMake build directory')
parser.add_argument('--sanitize', action='store_true', help='Enable ASan and UBSan')
parser.add_argument('--baseline', action='store_true', help='Run against unpatched mem.cpp and require the regression to fail')
args = parser.parse_args()
source = args.source.resolve()
build = args.build.resolve()
mem_build = build / 'vita3k/mem'
patch = HERE / 'patches/vita3k-host-page-protection.patch'
test_src = HERE / 'tests/host_page_protection.cpp'
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

    mem_vars = make_vars(mem_build / 'CMakeFiles/mem.dir/flags.make')
    test_vars = make_vars(mem_build / 'CMakeFiles/mem-tests.dir/flags.make')
    sanitizer = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    patched_obj = temp / 'mem.cpp.o'
    test_obj = temp / 'host_page_protection.o'
    exe = temp / 'host-page-protection-test'

    def compile_cmd(vars_, source_path, output):
        return ['/usr/bin/c++', *shlex.split(vars_['CXX_DEFINES']),
                *shlex.split(vars_['CXX_INCLUDES']), *shlex.split(vars_['CXX_FLAGS']),
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
                            text=True, capture_output=True)
    if args.baseline:
        if result.returncode == 0 or 'FAIL: same host page shares one protection segment' not in result.stderr:
            print(result.stdout, end='')
            print(result.stderr, end='')
            raise RuntimeError('unpatched baseline did not fail at the expected regression assertion')
        print('unpatched baseline fails at the expected host-page segment assertion')
    else:
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, [str(exe)])
