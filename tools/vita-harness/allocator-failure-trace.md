# Bounded VitaGL allocator failure trace

`--trace-allocator-failures` applies an optional diagnostic patch to a fresh
private VitaGL build copy. It does not modify the local VitaGL checkout or the
existing `vitaGL-finish2` build. Recreate the archive with:

```sh
python3 tools/vita-harness/build_vitagl.py \
  --trace-allocator-failures \
  --output build/vita-deps/vitaGL-allocator-failure-v1
```

The option preserves the normal PCSX build flags:
`HAVE_SHADER_CACHE=1`, `NO_DEBUG=1`, `READBACKS_SPEEDHACK=1`, and
`CIRCULAR_POOL_SPEEDHACK=1`. The source remains at vitaGL commit
`8a47034e078f7c39aa45e43493d7fe360a52a871`; the output's
`pcsx-build.json` records source, flags, every patch hash, and the resulting
archive hash. The new archive is
`build/vita-deps/vitaGL-allocator-failure-v1/libvitaGL.a` with SHA-256
`7d991dc2522c8489dc4f129329373335497fb1c4ea24bf98a916a81ae433b576`.

The probe writes directly with `sceClibPrintf`, independently of `NO_DEBUG`
and `LOG_ERRORS`, and stops after 12 events per process. It logs the initial
mapped allocation failure immediately before the first unsafe retry, each
unsafe retry count, and the final retry result. Each event includes requested
size and alignment, CPU/GPU allocation route, `unsafe_allocator_counter`, and
free bytes for VRAM, RAM, PHYCONT, and CDLG pools. Those internal pool values
come from VitaGL's existing allocator statistics (`vgl_mem_get_free_space`).
The external/newlib pool is labeled unknown because that API deliberately
returns zero for `VGL_MEM_EXTERNAL`; the trace does not present that as a free
space measurement. The route reflects the existing attempt order: GPU-mapped
allocations try VRAM, RAM, PHYCONT, CDLG, then external memory; CPU-mapped
allocations try RAM, PHYCONT, CDLG, VRAM, then external memory.

The initial failure is observed only after the ordinary allocator has tried
its configured pools and, when compiled in, texture-cache eviction has failed
to recover enough space. With the four recorded flags, `HAVE_TEX_CACHE` is not
enabled, so this build reports that failure immediately before the GC retry.
The probe does not change the allocation algorithm, frame-purge count,
selected pool order, or GC behavior.

The allocator trace archive is prepared for the frozen System12 investigation
whose input VPK SHA-256 is
`cd5df339f5db4f7b94b3e10c16aaad7917c43c7f1c20f7a0650e501a39b731da`. No
Vita3K or macOS app run is part of archive regeneration; the package owner can
link this archive into a new VPK and decide when to run it.
