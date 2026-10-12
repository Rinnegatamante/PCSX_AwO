import random
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "frontend/vita/post_processing.c").read_text(encoding="utf-8")
MATCH = re.search(r"static const float rows\[4\]\[6\] = \{(.*?)\};", SOURCE, re.S)
assert MATCH is not None
ROWS = [
    [float(x[:-1]) for x in re.findall(r"[-+]?(?:\d*\.\d+|\d+\.\d*|\d+)f", row)]
    for row in re.findall(r"\{([^{}]+)\}", MATCH.group(1))
]
assert len(ROWS) == 4 and all(len(row) == 6 for row in ROWS)

LMS_MATRIX = (
    (17.8824, 43.5161, 4.11935),
    (3.45565, 27.1554, 3.86714),
    (0.0299566, 0.184309, 1.46709),
)
RGB_MATRIX = (
    (0.0809444479, -0.130504409, 0.116721066),
    (-0.0102485335, 0.0540193266, -0.113614708),
    (-0.000365296938, -0.00412161469, 0.693511405),
)
DEFICIENCY_MATRICES = (
    ((0.0, 2.02344, -2.52581), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
    ((1.0, 0.0, 0.0), (0.494207, 0.0, 1.24827), (0.0, 0.0, 1.0)),
    ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (-0.395913, 0.801109, 0.0)),
)


def mul(matrix, vector):
    return [sum(a * b for a, b in zip(row, vector)) for row in matrix]


def original(rgb, mode):
    if mode == 0:
        return rgb
    lms = mul(LMS_MATRIX, rgb)
    deficient = mul(DEFICIENCY_MATRICES[mode - 1], lms)
    projected = mul(RGB_MATRIX, deficient)
    error = [rgb[i] - projected[i] for i in range(3)]
    return (
        rgb[0],
        rgb[1] + 0.7 * error[0] + error[1],
        rgb[2] + 0.7 * error[0] + error[2],
    )


def optimized(rgb, mode):
    rows = ROWS[mode]
    return (
        rgb[0],
        sum(rgb[i] * rows[i] for i in range(3)),
        sum(rgb[i] * rows[i + 3] for i in range(3)),
    )


random.seed(47)
maximum_error = 0.0
for mode in range(4):
    for rgb in ((0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (1.0, 0.0, 0.0),
                (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
        for a, b in zip(original(rgb, mode), optimized(rgb, mode)):
            maximum_error = max(maximum_error, abs(a - b))
    for _ in range(3000):
        rgb = tuple(random.random() for _ in range(3))
        for a, b in zip(original(rgb, mode), optimized(rgb, mode)):
            maximum_error = max(maximum_error, abs(a - b))
assert maximum_error < 2e-5, maximum_error
print(f"Daltonic matrix verification passed: max error {maximum_error:.3g}, 12020 samples")
