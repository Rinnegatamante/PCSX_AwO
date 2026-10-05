# VitaGL readback validation

The private VitaGL build uses `HAVE_SHADER_CACHE=1 NO_DEBUG=1
READBACKS_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1`. Its original checkout remains
unchanged; `build_vitagl.py` records the source revision, patches, flags and
archive hash in the build copy.

The readback patch retains the source rectangle and stride but starts the
destination at `(0,0)`. VitaGL allocates a tight `width * height` destination,
so using the source coordinates as destination offsets can write outside it.
The renderer allocates its 2 MiB scratch buffer with `vglMemalign(64, ...)`,
which provides GXM-mapped storage, and frees it before VitaGL shutdown.
Guest VRAM reads explicitly finish GPU work because `READBACKS_SPEEDHACK`
removes the implicit scene submission/wait. RGBA readback restores each PSX
word from its R/G bytes; the Vita path packs rows tightly.
The added `vglReadPixelsChecked` API returns transfer/finish failure even with
`NO_DEBUG=1`. On failure the renderer restores the previous framebuffer and
keeps the GPU-newer tile markers rather than committing stale scratch bytes.
The strict runner rejects every `vitaGL readback FAIL` marker.

`VITA_CTR_GPU_TRACE=1` enables bounded packet, vertex, draw-state and pixel
diagnostics. It is disabled by default. The smoke guest writes an actual GP0
flat yellow triangle, then reads triangle/background pixels through the
emulated GPU interface and checks its RAM sentinel and ARMv7 DRC driver.
Startup/shutdown alone does not satisfy that pixel check.

The isolated M1 host forces Vulkan memory mapping to Disabled even if its
profile requests DoubleBuffer. Upstream transfer handling in that mode copies
guest backing RAM without synchronizing the Vulkan render surface. The
optional `--fix-unmapped-readback` host patch supplies staging readback on
demand; validation uses `disable-surface-sync: false`.

Before that host correction, the rebuilt mapped-buffer/tight-destination
guest still failed: both sampled pixels were `0000`, the sentinel was correct,
and native screenshots were black. The strict runner recorded 11 forwarded
faults with 11 matching native recoveries, no unresolved faults, successful
VitaGL shutdown and `Stopping -> Idle`. Evidence is in
`/tmp/pcsx-vitagl-readback2-pixels-20261005.log`. This remains failed graphics
evidence, despite the successful lifecycle.

Native captures use Vita3K's own `take_screenshot` hook with
`VITA3K_AUTO_SCREENSHOT` and PNG output. Each host launch has an independent
process-group watchdog and the runner closes its owned host even after a
successful guest leaves the GUI open. The original Vita3K profile is not used.

## Isolated Vulkan readback result

The host correction now compiles and links as an incremental private app. It
preserves the queued mapped-memory path and handles Disabled mapping through
a synchronous, on-demand surface copy. Interior source addresses are matched
against the cached surface extent. The fence completes before staging data is
invalidated through VMA, copied to guest RAM and consumed by the transfer
callback; no staging buffer or cache-info pointer is left in the request queue.
The changed-surface dependency list is cleared each frame. The source checkout
and original build are not modified.

The final readback host is
`build/vita-validation-readback2/emulator/Vita3K-fixed.app`, executable SHA256
`ac5d127190c8ea4cc67d0fe3eb361e599a3a9aceec952f79f5ca1c749638fc66`.
Its patch SHA256 is
`10bb23c657f227720bfcd442330afcdfd1190e261a86c66ab996ce1d242633ea`;
`pcsx-host-build.json` records the reused objects' source inputs and the linked
Dynarmic archive. The isolated profile uses Disabled mapping, surface sync
enabled, and asynchronous pipeline compilation disabled.

With that host, main-target reads contain the yellow triangle `(248,248,0,0)`
and background `(16,32,64,0)`. The remaining VRAM discrepancy was a second Y
inversion in the renderer: GPU packing stores guest row Y directly, while the
custom GPU transfer API also addresses raw FBO rows directly. Reading the
opposite window returned zeros. Using the guest Y produces a strict 180-frame
smoke PASS: `cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b`.
All 13 forwarded faults have matching recoveries, shutdown passes, and the
guest reaches Idle. See `/tmp/pcsx-vitagl-unmapped-direct-y-20261005.log`.
This is still a diagnostic build with `VITA_CTR_GPU_TRACE=1`; the default
screen's readback and native captures remain black, so presentation remains
unverified. Disabling asynchronous pipeline compilation and clearing the
whole presentation surface instead of its bars did not resolve that failure.

The existing NEON System 12 package also passes its three-frame console
pixel/signature regression on this host after explicit installation:
`/tmp/pcsx-vita-neon-readback-host-regression2-20261005.log`.
