#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS="$(dirname "$PROJECT")"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

git config --global --add safe.directory "$PROJECT"
git config --global --add safe.directory "$DEPS/vitaGL-pcsx-rearmed"
git config --global --add safe.directory "$DEPS/sdl3-vitagl"

if [[ ! -f "$DEPS/sdl3-vitagl/CMakeLists.txt" || ! -f "$DEPS/vitaGL-pcsx-rearmed/Makefile" ]]; then
  echo "Missing pinned SDL3 or vitaGL checkout next to the project." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -qq -y --no-install-recommends pkg-config libc6-dev-i386 zip python3

if ! command -v python >/dev/null 2>&1; then
  ln -s "$(command -v python3)" /usr/local/bin/python
fi

checkout_dep() {
  local name="$1" url="$2" commit="$3" dest="$DEPS/$1"
  git init -q "$dest"
  git -C "$dest" remote add origin "$url"
  git -C "$dest" fetch -q --depth=1 origin "$commit"
  git -C "$dest" checkout -q --detach FETCH_HEAD
}

checkout_dep vita-headers https://github.com/vitasdk/vita-headers.git HEAD
cp -a "$DEPS/vita-headers/include/." "$VITASDK/arm-vita-eabi/include/"

checkout_dep math-neon https://github.com/Rinnegatamante/math-neon.git 0faab814782c071ff4015527f1ca955ab1ccc470
make -C "$DEPS/math-neon" -j"$(nproc)"
make -C "$DEPS/math-neon" install

checkout_dep vitaShaRK https://github.com/Rinnegatamante/vitaShaRK.git df24065e65098b2d1ac533760109ad4367573f28
make -C "$DEPS/vitaShaRK" -j"$(nproc)"
make -C "$DEPS/vitaShaRK" install

make -C "$DEPS/vitaGL-pcsx-rearmed" -j"$(nproc)" \
  HAVE_SHADER_CACHE=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 \
  READBACKS_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1 DEPTH_STENCIL_HACK=1 install

checkout_dep imgui-vita https://github.com/Rinnegatamante/imgui-vita.git a0562251489838cc45b7ea287fa5304d090697f5
make -C "$DEPS/imgui-vita" -j"$(nproc)"
make -C "$DEPS/imgui-vita" install

cmake -S "$DEPS/sdl3-vitagl" -B "$DEPS/build-sdl3" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_INSTALL_PREFIX="$VITASDK/arm-vita-eabi" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
  -DVIDEO_VITA_VGL=ON -DVIDEO_VITA_PIB=OFF -DVIDEO_VITA_PVR=OFF

if ! grep -Eq '^HAVE_VITAGL_H:INTERNAL=(1|ON|TRUE)$' "$DEPS/build-sdl3/CMakeCache.txt"; then
  echo "SDL3 configure did not detect vitaGL.h; refusing a non-vitaGL SDL3 build." >&2
  exit 1
fi

cmake --build "$DEPS/build-sdl3" --parallel "$(nproc)"
cmake --install "$DEPS/build-sdl3"
test -s "$VITASDK/arm-vita-eabi/lib/libSDL3.a"

cd "$PROJECT"
python3 tools/verify_vita_postfx_cache.py
make -f Makefile.vita -j"$(nproc)" USE_PRECOMPILED_POSTFX=1

python3 tests/vita_livearea_assets_test.py
python3 tests/vita_postfx_assets_test.py
python3 tests/vita_postfx_crt_mask_test.py
python3 tests/vita_postfx_daltonic_test.py
python3 tests/vita_gpu_visible_subviewport_test.py
