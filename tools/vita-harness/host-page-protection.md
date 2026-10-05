# Vita3K host-page protection regression

`patches/vita3k-host-page-protection.patch` aligns `protect_tree` segments to the effective 16 KiB host protection range when `MemState` uses the non-page-table memory layout. Callback blocks retain their 4 KiB guest-page keys, so distinct watchers that share one host page remain distinct. Segment merging uses the effective range, rather than the unrounded request length. Page-table and external-mapping behavior is left unchanged.

The C++ regression runs Vita3K's real `init(mem, false)`, fixed allocation, `add_protect`, and native protected-memory accesses. An un-watched 4 KiB neighbor must conservatively fire both watchers sharing its 16 KiB host page, after which each watcher must fire again when rearmed. A second case checks watchers on opposite sides of a host-page boundary. Run it with:

```sh
python3 tools/vita-harness/test_host_page_protection.py \
  --source ../vita3k --build ../vita3k/build/macos --sanitize
```

This only addresses protection bookkeeping for the non-page-table allocation. It does not claim to fix guest aliases, shared backing mappings, or page-table/external-mapping protection behavior. The harness applies the patch to a private temporary source copy and replaces `mem.cpp.o` in a private archive; it does not modify the Vita3K checkout or its build.

Add `--fix-host-page-protection` to `rebuild_vita3k_fixed.py` to apply the same patch to an isolated host. The recipe recompiles `mem.cpp`, reconstructs a private `libmem.a`, redirects every occurrence of that archive in the original link command, and records the patch and executable hashes. It preserves the original checkout and compiled objects.

The 2026-10-05 run with `memory-mapping: disabled` used host SHA-256 `7bba1fb6cdb3b93832257cc2bed6cbd2eef532f9aa688ec4efb048959c42e5ca` and the final3 VitaGL VPK `2102ea666b6fcd13fc771503b287671619cbda1737a14ff3b1f09b7c9732dfd8`. GPU/ari64 initialization, VitaGL shutdown and `Stopping -> Idle` were observed, with no critical protection errors. Eleven Dynarmic `Unhandled EXC_BAD_ACCESS` diagnostics remain and the strict lifecycle runner still reports FAIL. The separate bounded pixel diagnostic also reports FAIL: triangle `2082`, background `2082`, sentinel `534d4f4b`, 180 frames. No graphics validation is claimed.

Logs: `/tmp/pcsx-vitagl-host-pages-lifecycle-20261005.log` and `/tmp/pcsx-vitagl-host-pages-pixels-20261005.log`. Both owned host processes were closed by their watchdogs. The regression passes normally and with ASan/UBSan; `--baseline` verifies the expected failure without the patch.
