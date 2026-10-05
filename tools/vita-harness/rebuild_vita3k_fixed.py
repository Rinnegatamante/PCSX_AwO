#!/usr/bin/env python3
"""Rebuild only the patched Vita3K TUs into an isolated validation app.

Reads an existing macOS CMake build and never writes to that checkout, its
build, or the template app. Reuses its untouched objects and dependencies;
this is an incremental validation build, not a clean upstream rebuild.
"""
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import argparse
import hashlib
import json
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True, help='Local Vita3K source with existing CMake build')
parser.add_argument('--build', type=Path, help='Existing build directory; defaults to SOURCE/build/macos')
parser.add_argument('--app-template', type=Path, required=True, help='Built app bundle to copy, including resources/frameworks')
parser.add_argument('--output', type=Path, required=True, help='Fresh isolated output directory')
parser.add_argument('--dynarmic-archive', type=Path,
                    help='Tested patched Dynarmic archive to link into this private host')
parser.add_argument('--diagnose-null-memcpy', action='store_true',
                    help='Log invalid guest memcpy arguments and caller PC/LR without changing behavior')
parser.add_argument('--fix-host-page-protection', action='store_true',
                    help='Track native host-page coverage for direct guest memory protections')
parser.add_argument('--trace-protection-recovery', action='store_true',
                    help='Record successful native memory fault recovery and Dynarmic forwarding')
parser.add_argument('--fix-unmapped-readback', action='store_true',
                    help='Synchronize Vulkan color surfaces through CPU staging when mapping is disabled')
options = parser.parse_args()
ORIG = options.source.resolve()
BUILD = (options.build or ORIG / 'build/macos').resolve()
OUT = options.output.resolve()
if not ORIG.is_dir() or not BUILD.is_dir() or not options.app_template.is_dir():
    parser.error('source, build and app template must exist')
if options.dynarmic_archive and not options.dynarmic_archive.is_file():
    parser.error('Dynarmic archive must exist')
if options.trace_protection_recovery and not options.dynarmic_archive:
    parser.error('--trace-protection-recovery requires a tested --dynarmic-archive')
if OUT.exists():
    parser.error('output already exists; use a fresh directory')
if OUT == ORIG or ORIG in OUT.parents or OUT == BUILD or BUILD in OUT.parents:
    parser.error('output must be outside the source and its build')
OUT.mkdir(parents=True)
OBJ = OUT / 'host-fix-obj'
SRC = OUT / 'host-fix-src'
WORKTREE = SRC
APP = OUT / 'emulator/Vita3K-fixed.app'
BIN = APP / 'Contents/MacOS/Vita3K'
PATCH = Path(__file__).resolve().parent / 'patches/vita3k-teardown.patch'
SCREENSHOT_PATCH = Path(__file__).resolve().parent / 'patches/vita3k-vulkan-cpu-frame-dump.patch'
MEMCPY_PATCH = Path(__file__).resolve().parent / 'patches/vita3k-memcpy-diagnostic.patch'
HOST_PAGE_PATCH = Path(__file__).resolve().parent / 'patches/vita3k-host-page-protection.patch'
RECOVERY_PATCH = Path(__file__).resolve().parent / 'patches/vita3k-macos-recovery-trace.patch'
FORWARDED_PATCH = Path(__file__).resolve().parent / 'patches/dynarmic-forwarded-exceptions.patch'
READBACK_PATCH = Path(__file__).resolve().parent / 'patches/vita3k-vulkan-unmapped-readback.patch'
shutil.copytree(options.app_template.resolve(), APP)

SOURCES = [
    ('app', 'src/app_init.cpp', 'vita3k/app/src/app_init.cpp', 'app_init.cpp.o'),
    ('kernel', 'src/kernel.cpp', 'vita3k/kernel/src/kernel.cpp', 'kernel.cpp.o'),
    ('display', 'src/display.cpp', 'vita3k/display/src/display.cpp', 'display.cpp.o'),
    ('modules', 'SceDisplay/SceDisplay.cpp', 'vita3k/modules/SceDisplay/SceDisplay.cpp', 'SceDisplay.cpp.o'),
    ('renderer', 'src/gl/renderer.cpp', 'vita3k/renderer/src/gl/renderer.cpp', 'renderer-gl.cpp.o'),
    ('renderer', 'src/vulkan/renderer.cpp', 'vita3k/renderer/src/vulkan/renderer.cpp', 'renderer-vulkan.cpp.o'),
]
if options.diagnose_null_memcpy:
    SOURCES.append(('modules', 'SceLibKernel/SceLibKernel.cpp',
                    'vita3k/modules/SceLibKernel/SceLibKernel.cpp', 'SceLibKernel.cpp.o'))
if options.fix_host_page_protection or options.trace_protection_recovery:
    SOURCES.append(('mem', 'src/mem.cpp', 'vita3k/mem/src/mem.cpp', 'mem.cpp.o'))
if options.fix_unmapped_readback:
    SOURCES.extend([
        ('renderer', 'src/vulkan/context.cpp', 'vita3k/renderer/src/vulkan/context.cpp', 'context-vulkan.cpp.o'),
        ('renderer', 'src/vulkan/surface_cache.cpp', 'vita3k/renderer/src/vulkan/surface_cache.cpp', 'surface_cache-vulkan.cpp.o'),
        ('renderer', 'src/transfer.cpp', 'vita3k/renderer/src/transfer.cpp', 'transfer.cpp.o'),
    ])

# Only the patched sources/header are copied. Apply the checked-in patch to
# these private copies, so even a dirty user's source checkout stays untouched.
inputs = [row[2] for row in SOURCES] + ['vita3k/display/include/display/state.h']
original_hashes = {}
for relative in inputs:
    original = ORIG / relative
    copied = SRC / relative
    copied.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(original, copied)
    original_hashes[relative] = hashlib.sha256(original.read_bytes()).hexdigest()
patch_env = os.environ.copy()
patch_env.pop('GIT_DIR', None)
patch_env.pop('GIT_WORK_TREE', None)
patch_env['GIT_CEILING_DIRECTORIES'] = str(SRC.parent)
subprocess.run(['git', 'apply', '--check', str(PATCH)], check=True, cwd=SRC, env=patch_env)
subprocess.run(['git', 'apply', str(PATCH)], check=True, cwd=SRC, env=patch_env)
subprocess.run(['git', 'apply', '--check', str(SCREENSHOT_PATCH)], check=True, cwd=SRC, env=patch_env)
subprocess.run(['git', 'apply', str(SCREENSHOT_PATCH)], check=True, cwd=SRC, env=patch_env)
if options.diagnose_null_memcpy:
    subprocess.run(['git', 'apply', '--check', str(MEMCPY_PATCH)], check=True, cwd=SRC, env=patch_env)
    subprocess.run(['git', 'apply', str(MEMCPY_PATCH)], check=True, cwd=SRC, env=patch_env)
if options.fix_host_page_protection:
    subprocess.run(['git', 'apply', '--check', str(HOST_PAGE_PATCH)], check=True, cwd=SRC, env=patch_env)
    subprocess.run(['git', 'apply', str(HOST_PAGE_PATCH)], check=True, cwd=SRC, env=patch_env)
if options.trace_protection_recovery:
    subprocess.run(['git', 'apply', '--check', str(RECOVERY_PATCH)], check=True, cwd=SRC, env=patch_env)
    subprocess.run(['git', 'apply', str(RECOVERY_PATCH)], check=True, cwd=SRC, env=patch_env)
if options.fix_unmapped_readback:
    subprocess.run(['git', 'apply', '--check', str(READBACK_PATCH)], check=True, cwd=SRC, env=patch_env)
    subprocess.run(['git', 'apply', str(READBACK_PATCH)], check=True, cwd=SRC, env=patch_env)
for relative in inputs:
    if hashlib.sha256((SRC / relative).read_bytes()).hexdigest() == original_hashes[relative]:
        raise RuntimeError(f'patch did not modify expected source: {relative}')

def vars_file(path):
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r'([A-Z0-9_]+)\s*=\s*(.*)', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out

def compile_one(target, source_rel, patch_rel, obj_name):
    flags = vars_file(BUILD / 'vita3k' / target / 'CMakeFiles' / f'{target}.dir/flags.make')
    source = WORKTREE / patch_rel
    copied = source
    output = OBJ / obj_name
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = ['/usr/bin/c++', *shlex.split(flags['CXX_DEFINES']),
           '-I' + str(WORKTREE / 'vita3k/display/include'),
           '-I' + str(ORIG / 'vita3k' / target / Path(source_rel).parent),
           *shlex.split(flags['CXX_INCLUDES']), *shlex.split(flags['CXX_FLAGS']),
           '-c', str(copied), '-o', str(output)]
    subprocess.run(cmd, check=True, cwd=BUILD)
    return output

compiled = {}
for target, source_rel, patch_rel, obj_name in SOURCES:
    compiled[(target, source_rel)] = compile_one(target, source_rel, patch_rel, obj_name)

# Reconstruct each touched static library from the exact existing CMake object
# list, substituting only the patched TU objects. This preserves distinct GL
# and Vulkan renderer.cpp archive members despite their common basename.
for target in ('display', 'modules', 'renderer', *(['mem'] if ('mem', 'src/mem.cpp') in compiled else [])):
    target_dir = BUILD / 'vita3k' / target
    link_txt = target_dir / 'CMakeFiles' / f'{target}.dir/link.txt'
    words = shlex.split(link_txt.read_text())
    object_paths = [Path(w) for w in words if w.endswith('.o')]
    archive = OBJ / f'lib{target}.a'
    if archive.exists():
        archive.unlink()
    args = ['/usr/bin/ar', 'qc', str(archive)]
    for obj in object_paths:
        replacement = next((output for (compiled_target, source_rel), output in compiled.items()
                            if compiled_target == target and obj.as_posix().endswith(source_rel + '.o')), None)
        args.append(str(replacement if replacement else target_dir / obj))
    subprocess.run(args, check=True, cwd=BUILD)
    subprocess.run(['/usr/bin/ranlib', str(archive)], check=True)

# Rebuild app/kernel archives in the isolated directory, sourcing untouched
# members from existing objects and replacing only the patched source member.
for target, source_rel, patch_rel, obj_name in SOURCES[:2]:
    target_dir = BUILD / 'vita3k' / target
    words = shlex.split((target_dir / 'CMakeFiles' / f'{target}.dir/link.txt').read_text())
    object_paths = [Path(w) for w in words if w.endswith('.o')]
    archive = OBJ / f'lib{target}.a'
    if archive.exists():
        archive.unlink()
    args = ['/usr/bin/ar', 'qc', str(archive)]
    for obj in object_paths:
        if obj.as_posix().endswith(source_rel + '.o'):
            args.append(str(compiled[(target, source_rel)]))
        else:
            args.append(str(target_dir / obj))
    subprocess.run(args, check=True, cwd=BUILD)
    subprocess.run(['/usr/bin/ranlib', str(archive)], check=True)

# Link against the original command's already-built dependencies, but redirect
# all patched archives and the executable to the isolated bundle.
link_txt = BUILD / 'vita3k/CMakeFiles/vita3k.dir/link.txt'
words = shlex.split(link_txt.read_text())
rewrites = {
    'app/libapp.a': str(OBJ / 'libapp.a'),
    'kernel/libkernel.a': str(OBJ / 'libkernel.a'),
    'display/libdisplay.a': str(OBJ / 'libdisplay.a'),
    'modules/libmodules.a': str(OBJ / 'libmodules.a'),
    'renderer/librenderer.a': str(OBJ / 'librenderer.a'),
}
if ('mem', 'src/mem.cpp') in compiled:
    if 'mem/libmem.a' not in words:
        raise RuntimeError('existing link command does not identify the mem archive')
    rewrites['mem/libmem.a'] = str(OBJ / 'libmem.a')
dynarmic_input = BUILD / 'external/dynarmic/src/dynarmic/libdynarmic.a'
if options.dynarmic_archive:
    original_link_path = '../external/dynarmic/src/dynarmic/libdynarmic.a'
    if words.count(original_link_path) != 1:
        raise RuntimeError('existing link command does not identify one Dynarmic archive')
    dynarmic_input = options.dynarmic_archive.resolve()
    private_dynarmic = OBJ / 'libdynarmic.a'
    shutil.copy2(dynarmic_input, private_dynarmic)
    if options.trace_protection_recovery:
        relative = 'external/dynarmic/src/dynarmic/backend/exception_handler_macos.cpp'
        original = BUILD / 'external/dynarmic-patched/src/dynarmic/backend/exception_handler_macos.cpp'
        private_source = SRC / relative
        private_source.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, private_source)
        original_hashes[relative] = hashlib.sha256(original.read_bytes()).hexdigest()
        subprocess.run(['git', 'apply', '--check', str(FORWARDED_PATCH)], check=True, cwd=SRC, env=patch_env)
        subprocess.run(['git', 'apply', str(FORWARDED_PATCH)], check=True, cwd=SRC, env=patch_env)
        flags = vars_file(BUILD / 'external/dynarmic/src/dynarmic/CMakeFiles/dynarmic.dir/flags.make')
        private_object = OBJ / 'exception_handler_macos.cpp.o'
        subprocess.run(['/usr/bin/c++', *shlex.split(flags['CXX_DEFINES']),
                        *shlex.split(flags['CXX_INCLUDES']), *shlex.split(flags['CXX_FLAGS']),
                        '-c', str(private_source), '-o', str(private_object)], check=True, cwd=BUILD)
        members = subprocess.run(['/usr/bin/ar', 't', str(private_dynarmic)],
                                 check=True, capture_output=True, text=True).stdout.splitlines()
        if members.count(private_object.name) != 1:
            raise RuntimeError('Dynarmic archive must contain exactly one macOS exception handler')
        subprocess.run(['/usr/bin/ar', 'r', str(private_dynarmic), str(private_object)], check=True)
        subprocess.run(['/usr/bin/ranlib', str(private_dynarmic)], check=True)
    rewrites[original_link_path] = str(private_dynarmic)
rewritten = [rewrites.get(w, w) for w in words]
for i, word in enumerate(rewritten):
    if word == '-o':
        rewritten[i + 1] = str(BIN)
BIN.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(rewritten, check=True, cwd=BUILD / 'vita3k')
for framework in ('QtWidgets', 'QtSvg', 'QtMultimedia', 'QtGui', 'QtNetwork', 'QtConcurrent', 'QtCore'):
    for prefix in ('qtbase', 'qtsvg', 'qtmultimedia'):
        old = f'/opt/homebrew/opt/{prefix}/lib/{framework}.framework/Versions/A/{framework}'
        new = f'@executable_path/../Frameworks/{framework}.framework/Versions/A/{framework}'
        subprocess.run(['/usr/bin/install_name_tool', '-change', old, new, str(BIN)], check=False)
subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(BIN)], check=True)
manifest = {
    'source': str(ORIG), 'build': str(BUILD),
    'source_files_sha256': original_hashes,
    'patch_sha256': hashlib.sha256(PATCH.read_bytes()).hexdigest(),
    'screenshot_patch_sha256': hashlib.sha256(SCREENSHOT_PATCH.read_bytes()).hexdigest(),
    'memcpy_diagnostic_patch_sha256': (hashlib.sha256(MEMCPY_PATCH.read_bytes()).hexdigest()
                                      if options.diagnose_null_memcpy else None),
    'host_page_protection_patch_sha256': (hashlib.sha256(HOST_PAGE_PATCH.read_bytes()).hexdigest()
                                        if options.fix_host_page_protection else None),
    'recovery_trace_patch_sha256': (hashlib.sha256(RECOVERY_PATCH.read_bytes()).hexdigest()
                                  if options.trace_protection_recovery else None),
    'forwarded_exception_patch_sha256': (hashlib.sha256(FORWARDED_PATCH.read_bytes()).hexdigest()
                                       if options.trace_protection_recovery else None),
    'unmapped_readback_patch_sha256': (hashlib.sha256(READBACK_PATCH.read_bytes()).hexdigest()
                                     if options.fix_unmapped_readback else None),
    'executable_sha256': hashlib.sha256(BIN.read_bytes()).hexdigest(),
    'dynarmic_archive': str(dynarmic_input),
    'dynarmic_archive_sha256': hashlib.sha256(dynarmic_input.read_bytes()).hexdigest(),
    'linked_dynarmic_archive_sha256': hashlib.sha256(
        (OBJ / 'libdynarmic.a' if options.dynarmic_archive else dynarmic_input).read_bytes()).hexdigest(),
    'validation_scope': 'incremental macOS arm64 host; untouched CMake objects reused',
}
(OUT / 'pcsx-host-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(f'linked {BIN}')
