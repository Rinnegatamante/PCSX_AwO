# Vita GPU compatibility headers

These headers restore the interfaces needed to compile the Vita GPU backend
after the separate `ctr-native` checkout disappeared. They are intentionally
limited to the code used by `plugins/gpu_vita_ctr`.

`psx/libgpu.h`, `psx/libgte.h`, and the `platform/native_gpu*.h` and
`platform/native_renderer*.h` interfaces were recovered from the MIT-licensed
PsyCross/REDRIVER2 source tree vendored in [CTR Native](https://github.com/CTR-tools/ctr-native)
at revision `fc3fa26bc3a4003d91f4eca68308aff8c0a83cef`. Their source notices are retained;
see the project `THIRD_PARTY_NOTICES.md`. The declarations were reconciled with
the implementations in this repository where this backend has moved ahead of
that checkout. In particular, `GrVertex` keeps its 20-byte layout by using the
two bytes formerly reserved as padding for `orderDepth`, and the renderer/GPU
function declarations match the local definitions.

The small `macros.h`, `platform.h`, and platform service headers are local
compatibility interfaces, not copied CTR Native headers. The upstream CTR
Native project is GPL-3.0-only, so its unrelated headers were not imported into
this GPL-2.0 project. These local files declare only the types, Vita platform
services, and GPU/renderer links referenced by the recovered MIT interfaces
and this backend.

For Vita builds, OpenGL symbols come from the public `vitaGL.h` in the pinned
local dependency at `build/vita-deps/vitaGL/source`; do not use the old
`ctr-native` GL headers. The adapter was compile-checked with ARM Vita GCC and
`HAVE_SHADER_CACHE=1`, `NO_DEBUG=1`, `READBACKS_SPEEDHACK=1`, and
`CIRCULAR_POOL_SPEEDHACK=1`.

The PsyCross `DR_PSYX_TEX` producer in this tree supplies the texture ID and
texture dimensions. The private PSYX STP extension has no producer here, so the
adapter leaves `overrideTexturePsxStp` disabled and does not interpret a
reserved packet bit as STP state.
