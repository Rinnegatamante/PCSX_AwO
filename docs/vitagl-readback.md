# VitaGL readback validation

The production console probe now passes both VRAM reads and native
presentation. GPU packing restored the main framebuffer through a helper that
also resized its texture from the current draw environment. In this probe,
the environment changed from 640×478 to 320×240 before presentation, so that
restoration discarded the rendered pixels. Binding the existing framebuffer
preserves its storage until the next scene sizes it deliberately.

The corrected production VPK is
`build/vita-validation-preserve-target/pcsx-rearmed-vita-vitagl.vpk`, SHA256
`14a19291183c963c9aad5ec1cf299e6dc8dd441c4c6702faf5fb292035d12a43`.
GPU tracing is disabled and the library uses the four flags below. The
180-frame ari64 probe passes with `03ff/2082/534d4f4b`, 10 paired fault
recoveries, successful shutdown and Idle. Three native screenshots taken
after the presentation draw show the yellow triangle and blue background;
the final image contains 98,284 yellow and 179,792 blue pixels. The guarded
runner closes its host and leaves no Vita3K process running.
`pcsx-vitagl-preserve-target-evidence.json` beside the package binds the host,
library, source delta and native PNG hashes. The log is
`/tmp/pcsx-vitagl-preserve-target-20261005.log`. This validates the console
graphics probe; Soul Calibur and physical Vita remain separate gates.

The failed runs below describe the diagnosis before that correction.

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

## Production pixel probe

The build with `VITA_CTR_GPU_TRACE=0` also passes 180 frames, with all original
presentation code and MSAA 4x restored. The ELF contains no GPU-trace marker;
both GPU source files match the repository versions by SHA256. The same
pixel/sentinel/ari64 results pass, all 11 forwarded faults match recoveries,
shutdown passes and the guest reaches Idle. Its VPK SHA256 is
`3ea9dcb7c06f6458726a0ab17506369e8e9bffde8933c184fd39c06cdec11d43`,
linked against VitaGL archive
`a829b08a11f27653957e06f2b2ff28cbf8a733d238a807deccc1f3de74fe8fbe`.
The retained package is
`build/vita-validation-readback2/pcsx-rearmed-vita-vitagl-production.vpk`.
See `/tmp/pcsx-vitagl-readback-production-20261005.log` and its runner log.

Three native PNGs at `033300`, `033301` and `033302` on 2026-10-05 were saved
under the isolated profile. Their RGB extrema are all `(0,0)` and alpha is
255 throughout: they are genuinely black, rather than a transparency artifact.
The console GPU pixel probe is validated; native presentation is not. Turning
off MSAA in a separate diagnostic copy did not fix presentation and was not
retained. No physical Vita validation is claimed.

## Display path trace

The private host builder accepts `--trace-display-presentation`. Its optional
patch records callback entry/return, accepted display parameters and Vulkan's
surface-cache selection. Each site records its first eight events, then every
60th event through 240; it does not change callback arguments or frame selection.
The patch and source hashes are retained in the host manifest.

The production package above passes the same 180-frame probe on this host:
`/tmp/pcsx-vitagl-display-trace2-20261005.log`. Callback payloads contain the
three VitaGL display buffers `60aa4000`, `60ca5000`, `60ea6000`; callbacks return
zero and `sceDisplaySetFrameBuf` accepts those addresses. Vulkan later presents
the corresponding cached GXM surface (`cached=true`). This excludes a missing
callback or continued selection of the initial CPU framebuffer `60000000` in
this run. It does not prove the surface contains the expected picture.

Native screenshots at `034317`, `034318`, `034319` are still black with opaque
alpha. All 11 forwarded faults match recoveries; shutdown and Idle pass. The
isolated host is in `build/vita-validation-display-trace2`; its portable
`vita3k.log` contains the bounded display records. The watchdog closes each
owned install/run process; no native host remains after the test.

Three scratch shader experiments further isolate the failure. Forcing only
presentation alpha to one leaves its RGB zero (`display-alpha` log). Replacing
the presentation fragment with constant red produces `(255,0,0,255)` and native
PNG captures show the red viewport with black side bars (`display-red` log).
Sampling the source texture at fixed UV `(0.5,0.5)` produces black again
(`display-constantuv` log). Each run passes the 180-frame VRAM probe and shutdown;
the latter host pairs 13 forwarded faults with 13 recoveries. These logs are
`/tmp/pcsx-vitagl-<name>-20261005.log`. The constant-UV run uses
`build/vita-validation-display-trace3`, which also locks the display-state
snapshot used by diagnostic logging. The runner's crash/timeout/installation
regression passes in `/tmp/pcsx-vita3k-display-runner-tests-20261005.log`.

These shader changes are confined to the SDK scratch tree. They are diagnostic
probes, not renderer fixes: native presentation draws correctly with a constant
fragment, while sampling the source render texture in this path still fails.

The sampler-binding scratch probe (`/tmp/pcsx-vitagl-display-sampler-20261005.log`)
records texture ID 3 on unit `GL_TEXTURE0` for both pack program 57 and
presentation program 59. Reassigning `s_src=0` immediately before presentation
does not fix the black backbuffer. Its native screenshots at `035443` and
`035444` were taken after the main-RT pixel reads at `03:54:42.825`; both remain
black. The earlier constant-UV screenshots preceded the first main-RT draw
because of shader compilation and are not evidence of a rendered-frame
failure; the constant-UV backbuffer read is the relevant result of that probe.
Future short probes must cover shader warmup or reuse the isolated shader
cache and compare capture timestamps with the first draw.

The optional host trace also records the first 64 texture-cache lookups and
texture synchronizations. It logs overlap/dirty state, the chosen GPU-surface
or guest-memory texture route, descriptor layout changes and texture viewport
ratio/offset. Its renderer TUs are rebuilt only in the isolated host.

The production VPK passes on `build/vita-validation-display-trace5` with all
11 forwarded faults recovered, shutdown and Idle. The retained run is
`/tmp/pcsx-vitagl-display-cache-20261005.log`. A texture synchronization at
`04:03:52.323` uses GPU surface `62a7f050`, size `640x478`, matching stride 2560,
dirty=false and viewport ratio 1:1. The next synchronization uses a different
`320x240` texture from guest memory. These records alone do not associate a
specific draw with the missing source binding. Native captures at `040353`
and `040354` remain black after the GPU-surface lookup. This host reuses only
the isolated guest shader cache; its source/patch hashes match its manifest.

Moving the presentation Y flip from fragment to vertex stage also leaves the
backbuffer black. That scratch probe passes 180 frames with 12 matched native
fault recoveries, shutdown and Idle:
`/tmp/pcsx-vitagl-display-vertexflip-20261005.log`. It is not retained in the
renderer source. Its package, source delta and evidence record are kept under
`build/vita-validation-display-trace5/vertexflip-probe-*` (the package is
`pcsx-vitagl-vertexflip-probe.vpk`). The SDK renderer source is restored to the
repository version; the separate production VPK remains unchanged. Native
VitaGL presentation and physical hardware validation remain pending.
