# Build storage and historical evidence

On 2026-10-08, `build/` occupied 35,855,237,120 allocated bytes (33.39 GiB).
Most of it was accumulated isolated Vita3K diagnostic builds: copied app
bundles, firmware/runtime trees, source snapshots and repeated compilation
archives. The retired groups alone contained 12.64 GiB of `host-fix-obj`
compilation caches. No compiler, harness or emulator was running during cleanup.

43 historical Vita build directories were retired after preserving every file
except those compilation caches in a content-addressed gzip archive. Identical
files are stored once. Each newly created archive object was decompressed and
verified against its original SHA-256 before removal. Logs, screenshots,
packages, modified source and historical executables are retained. Recovery
was tested on an evidence/package directory, a Vita3K executable and a symlink.

| Storage after cleanup | Allocated bytes | GiB |
| --- | ---: | ---: |
| Remaining `build/` | 3,424,620,544 | 3.19 |
| Compressed historical archive and manifest | 1,709,703,168 | 1.59 |
| Net reclaimed, accounting for the archive | 30,720,913,408 | 28.61 |

Current Mac builds and device tests remain in place. Vita dependencies remain
in `build/vita-deps`, the latest H8 IRQ guest build in
`build/vita-system12-h8-irq-20261005`, and the recent runnable host in
`build/vita-validation-mspace-stats-fix2`. Retained NEON/GL harnesses and the
Vita3K executable have unchanged hashes. ROM archives and tracked source were
not part of the retirement.

The local, ignored archive is
`artifacts/validation/build-cleanup-20261008/`: `plan.json`, `manifest.json`,
`summary.json`, `restore-check.json`, `retained-hashes.json`, and `objects/`.
The manifest maps the original relative file paths to their compressed content
hashes, modes and timestamps. Recover an old build or individual file with:

```sh
python3 artifacts/validation/build-cleanup-20261008/restore.py build/vita-validation-preserve-target
python3 artifacts/validation/build-cleanup-20261008/restore.py build/vita-validation/host-fix-src --destination /tmp/pcsx-history
```

Restoration refuses to overwrite existing files or traverse symlink parents.
The excluded `host-fix-obj` caches are rebuilt from the retained source/patches
and original build commands when needed. This local archive is not a remote
backup; preserve it when moving or removing the workspace.

For future work:

- Reuse the active Mac build directory for incremental builds rather than
  retaining a new complete build for every experiment.
- Keep one current Vita3K runtime and a separate control only when a comparison
  needs it. Keep diagnostic outputs in `artifacts/validation` and retire old
  runtimes after preserving their exact source, executable and evidence.
- `rebuild_vita3k_fixed.py` retains the signed app, patched source and hash
  manifest, but discards intermediate objects after a successful build.
  `--keep-objects` retains them for link/build diagnosis; failed builds retain
  their intermediates automatically.
- Avoid copying ROMs, previous `build/` trees or accumulated `artifacts/` into
  source snapshots. Store exact inputs once and reference them by hashes.
- All `build/` outputs are ignored by Git; commits contain source and reproducible
  tests, with local validation evidence kept separately.
