from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "livearea"
SIZES = {
    "sce_sys/icon0.png": (128, 128),
    "sce_sys/pic0.png": (960, 544),
    "sce_sys/livearea/contents/bg.png": (840, 500),
    "sce_sys/livearea/contents/startup.png": (280, 158),
}
TEMPLATE = "sce_sys/livearea/contents/template.xml"

for path, size in SIZES.items():
    data = (ASSETS / path).read_bytes()
    assert data.startswith(bytes.fromhex("89504e470d0a1a0a")), path
    assert data[12:16] == b"IHDR", path
    assert struct.unpack(">II", data[16:24]) == size, path

xml = ET.fromstring((ASSETS / TEMPLATE).read_bytes())
assert xml.tag == "livearea"
assert xml.findtext("./livearea-background/image") == "bg.png"
assert xml.findtext("./frame/liveitem/target") == "psla:eboot"
assert xml.findtext("./frame/liveitem/image") == "startup.png"

with zipfile.ZipFile(ROOT / "pcsx-awo.vpk") as vpk:
    assert vpk.testzip() is None
    for path in (*SIZES, TEMPLATE):
        assert vpk.read(path) == (ASSETS / path).read_bytes(), path
    assert "eboot.bin" in vpk.namelist()
    assert "sce_sys/param.sfo" in vpk.namelist()

print("Validated LiveArea assets and byte-identical VPK contents.")
