import math
import struct
from pathlib import Path

root = Path(__file__).resolve().parent.parent
source = (root / "assets/postfx/crt/frag.cg").read_text(encoding="utf-8")
assert "varying in float2 screenPosition : WPOS;" in source
assert "short pixelX = (short)floor(screenPosition.x);" in source
assert "(float)(pixelX & 1)" in source
bleeding = (root / "assets/postfx/crt_bleeding/frag.cg").read_text(encoding="utf-8")
for shader in (source, bleeding):
    assert "varying in float2 screenPosition : WPOS;" in shader
    assert "short pixelX = (short)floor(screenPosition.x);" in shader
    assert "float2 uv = curve(q);" in shader
    assert "float2 fetchUV = uCrop.xy + uv * uCrop.zw;" in shader
    assert "uv.y * uOutputSize.y * 0.2387324146 + 0.25 * iTime" in shader
    assert "scanY" not in shader
assert source.count("tex2D(") == 2
assert bleeding.count("tex2D(") == 3
assert "float2 bleedOffset" not in source and "float2 bleedOffset" in bleeding


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


for x0, width in ((0, 960), (117, 725), (120, 720), (173, 613)):
    previous = None
    for x in range(width):
        screen_pos = f32(x0 + x + 0.5)
        index = math.floor(screen_pos)
        assert index == x0 + x, (x0, width, x, index)
        mask = 1.0 - 0.65 * (index & 1)
        if previous is not None:
            assert abs(mask - previous) > 0.64
        previous = mask
    print(f"CRT shadow-mask phase continuous: x={x0}, width={width}, pixels={width}")


def curve(x, y):
    px, py = (x - 0.5) * 2.2, (y - 0.5) * 2.2
    px *= 1.0 + 0.04 * py * py
    py *= 1.0 + 0.0625 * px * px
    return 0.5 + 0.46 * py


for center_y in (0.2, 0.35, 0.65, 0.8):
    target = curve(0.5, center_y)
    for edge_x in (0.05, 0.95):
        low, high = 0.0, 1.0
        for _ in range(45):
            mid = (low + high) / 2.0
            if curve(edge_x, mid) < target:
                low = mid
            else:
                high = mid
        edge_y = (low + high) / 2.0
        assert abs(curve(edge_x, edge_y) - target) < 1e-10
        if center_y < 0.5:
            assert edge_y > center_y, (center_y, edge_x, edge_y)
        else:
            assert edge_y < center_y, (center_y, edge_x, edge_y)
print("CRT scanline isocurves match the curved framebuffer sampling at both edges")

scanline_frequency = 0.2387324146
assert math.isclose(scanline_frequency * 2.0 * math.pi, 1.5, abs_tol=1e-8)
for x in (0.0, 0.05, 0.25, 0.5, 0.75, 0.95, 1.0):
    for y in (0.0, 0.15, 0.25, 0.5, 0.75, 0.85, 1.0):
        step_y = 1.0 / 544.0
        gradient = (curve(x, y + step_y) - curve(x, y - step_y)) / (2.0 * step_y)
        sampled_frequency = scanline_frequency * gradient
        assert 0.0 < sampled_frequency < 0.5, (x, y, sampled_frequency)
        assert 0.75 * gradient > 0.5, (x, y, gradient)
print("CRT scanline frequency remains below Nyquist across the curved viewport")
