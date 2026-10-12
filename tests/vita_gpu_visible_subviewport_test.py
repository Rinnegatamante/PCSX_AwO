from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GPU = (ROOT / "plugins/gpu_vita_ctr/native_gpu.c").read_text(encoding="utf-8")
RENDERER = (ROOT / "plugins/gpu_vita_ctr/native_renderer.c").read_text(encoding="utf-8")


def display_subviewport(draw, display, size, rgb24=False):
    x, y, w, h = draw
    disp_x, disp_y = display
    visible_w, visible_h = size
    return (
        not rgb24
        and visible_w > 0
        and visible_h > 0
        and w > 0
        and h > 0
        and x >= disp_x
        and y >= disp_y
        and x + w <= disp_x + visible_w
        and y + h <= disp_y + visible_h
    )


def projected_clip_origin(draw, display, size):
    x, y, w, h = draw
    dx, dy = display
    visible_w, visible_h = size
    offset_x, offset_y = x - dx, y - dy
    if (
        w > 0 and h > 0
        and offset_x >= 0 and offset_y >= 0
        and offset_x + w <= visible_w
        and offset_y + h <= visible_h
    ):
        return offset_x, offset_y
    return 0, 0


assert "NativeGpu_SplitClipInsideDisplay(split)" in GPU
assert GPU.count("(split->drawenv.dfe || NativeGpu_SplitClipInsideDisplay(split))") == 2
assert "clip->x >= display->disp.x" in GPU
assert "clip->y >= display->disp.y" in GPU
assert "clipX + drawRect->w <= displayW" in RENDERER
assert "clipY + drawRect->h <= displayH" in RENDERER
assert "NativeRenderer_Ortho2D(-clipX, displayW - clipX, displayH - clipY, -clipY" in RENDERER

# MGS radar: a small GP0(E3/E4) clip is inside the displayed page,
# even though the old 75%-of-display DFE heuristic classifies it as offscreen.
assert display_subviewport((235, 16, 69, 52), (0, 0), (320, 240))
assert projected_clip_origin((235, 16, 69, 52), (0, 0), (320, 240)) == (235, 16)

# Ordinary drawing and visible PS1 HUD sub-clips remain onscreen.
assert display_subviewport((0, 0, 320, 240), (0, 0), (320, 240))
assert display_subviewport((12, 20, 36, 32), (0, 0), (320, 240))

# Double-buffered and offscreen VRAM pages must stay separate.
assert not display_subviewport((0, 256, 320, 224), (0, 0), (320, 240))
assert not display_subviewport((700, 20, 64, 40), (0, 0), (320, 240))
assert not display_subviewport((235, 16, 69, 52), (0, 0), (320, 240), rgb24=True)
assert not display_subviewport((0, 0, 0, 240), (0, 0), (320, 240))
assert not display_subviewport((235, 210, 69, 52), (0, 0), (320, 240))
assert projected_clip_origin((0, 256, 320, 224), (0, 0), (320, 240)) == (0, 0)

# Nonzero display-page origin is not equivalent to VRAM origin zero.
assert display_subviewport((250, 276, 40, 20), (0, 256), (320, 224))
assert projected_clip_origin((250, 276, 40, 20), (0, 256), (320, 224)) == (250, 20)

print("Validated visible GP0 subviewport routing and projection on representative VRAM pages.")
