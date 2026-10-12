from pathlib import Path
import re
import zipfile

root = Path(__file__).resolve().parent.parent
backend = (root / "frontend/vita/post_processing.c").read_text(encoding="utf-8")
selector = (root / "frontend/vita/rom_selector.cpp").read_text(encoding="utf-8")
settings = (root / "frontend/vita/settings.c").read_text(encoding="utf-8")
parameters = (root / "frontend/vita/postfx_parameters.c").read_text(encoding="utf-8")
embedded = (root / "build-vita/postfx_embedded.c").read_text(encoding="utf-8")
vpk = root / "pcsx-awo.vpk"

effects = re.findall(r'\{"([a-z0-9_]+)",\s*"[^"]+"\}', backend)
choices = selector.split("static const OptionChoice k_postfx_choices[] = {", 1)[1].split("};", 1)[0]
selected = re.findall(r'\{\s*"([a-z0-9_]+)"\s*,', choices)
assert effects == [s for s in selected if s != "off"], (effects, selected)
assert len(effects) == 13
assert effects[:2] == ["crt", "crt_bleeding"]
params = re.findall(
    r'\{"(daltonic|gamma|hue|vignette)", "([^"]+)", "([^"]+)", "(vita_fx_[^"]+)", ([0-9.]+)f',
    parameters,
)
assert len(params) == 9
for effect in ("daltonic", "gamma", "hue", "vignette"):
    declared = {
        line.split("=", 1)[0]
        for line in (root / "assets/postfx" / effect / "unif.txt").read_text().splitlines()
    }
    actual = {uniform for name, _, uniform, _, _ in params if name == effect}
    assert actual == declared, (effect, actual, declared)
for _, _, _, key, default in params:
    assert f'"{key}"' in settings and f'"{key}"' in selector, key
    assert f'{{ "{key}", "{float(default):.2f}"' in settings, key
native = (root / "plugins/gpu_vita_ctr/native_renderer.c").read_text(encoding="utf-8")
crt = (root / "assets/postfx/crt/frag.cg").read_text(encoding="utf-8")
fxaa = (root / "assets/postfx/fxaa/vert.cg").read_text(encoding="utf-8")
finish = native.split("void NativeRenderer_FinishPostProcessingFrame(void)", 1)[1].split(
    "void NativeRenderer_SwapWindow(void)", 1
)[0]
assert "const SDL_Rect viewport = s_postfxViewport;" in finish
assert "NativeRenderer_GetFourThreeViewport()" not in finish
assert native.count("s_postfxViewport =") >= 5
assert "VitaPostFX_Finish(viewport.x, viewport.y, viewport.w, viewport.h," in native
assert "glViewport(x, y, width, height);" in backend
assert "glScissor(x, y, width, height);" in backend
assert "vUv = texcoord;" in fxaa
assert "uniform float4 uCrop;" in crt
assert "glUniform4f(s_crop_location, u0, v0, u1 - u0, v1 - v0);" in backend
assert "glUniform2f(s_output_size_location, (float)width, (float)height);" in backend

for effect in ("crt", "crt_bleeding", "fxaa", "lcd3x", "sharp_grid", "sharp_scanlines", "vignette"):
    shader = (root / "assets/postfx" / effect / "frag.cg").read_text(encoding="utf-8")
    assert "uniform float4 uCrop" in shader, effect
for effect in ("crt", "crt_bleeding", "sharp_grid", "sharp_scanlines"):
    shader = (root / "assets/postfx" / effect / "frag.cg").read_text(encoding="utf-8")
    assert "uniform float2 uOutputSize" in shader, effect

for x, y, width, height in ((117, 0, 725, 544), (0, 0, 960, 544),
                            (117, 34, 725, 476), (0, 34, 960, 476)):
    crop = (x / 960, y / 544, width / 960, height / 544)
    for px, py, qx, qy in ((x, y, 0.0, 0.0),
                           (x + width / 2, y + height / 2, 0.5, 0.5),
                           (x + width, y + height, 1.0, 1.0)):
        uv = (px / 960, py / 544)
        local = ((uv[0] - crop[0]) / crop[2], (uv[1] - crop[1]) / crop[3])
        assert abs(local[0] - qx) < 1e-6 and abs(local[1] - qy) < 1e-6

with zipfile.ZipFile(vpk) as bundle:
    assert bundle.testzip() is None
    for name in effects:
        folder = root / "assets/postfx" / name
        for stage in ("vert", "frag"):
            source = folder / f"{stage}.cg"
            assert source.is_file(), source
            array = f"postfx_{name}_{stage}[]"
            assert array in embedded, array
            head = embedded.split(array, 1)[1].split("};", 1)[0]
            assert "0x00, 0x00, 0x00, 0x00," in head[:140], array
    assert not any(path.endswith(".gxp") for path in bundle.namelist())
    assert bundle.getinfo("eboot.bin").file_size > 0
    assert "licenses/Daedalus-PostFX-NOTICE.txt" in bundle.namelist()

print(f"Validated {len(effects)} post-processing effects, {len(effects) * 2} embedded shader binaries and {len(params)} uniforms in {vpk.name}")
