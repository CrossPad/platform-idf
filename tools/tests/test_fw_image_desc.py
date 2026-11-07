import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fw_image_desc


def image(with_desc=True, board_rev=2, kit=b"sampler", proto=0x000F, version=b"1.2.3"):
    header = bytes(0x20)                                   # image + segment header
    app = bytearray(0x100)
    struct.pack_into("<I", app, 0, 0xABCD5432)             # esp_app_desc magic
    app[0x10:0x10 + len(version) + 1] = version + b"\0"    # version[32]
    app[0x30:0x30 + 9] = b"CrossPad\0"                     # project_name[32]
    desc = b""
    if with_desc:
        desc = struct.pack("<IBBH16s32s", 0x49465043, 1, board_rev, proto,
                           kit.ljust(16, b"\0"), bytes(range(32)))
    return header + bytes(app) + desc + bytes(64)


def test_reads_desc(tmp_path):
    p = tmp_path / "a.bin"; p.write_bytes(image())
    d = fw_image_desc.read(p)
    assert d["board_rev"] == 2 and d["kit_id"] == "sampler"
    assert d["stm_proto_min"] == 0x000F
    assert d["assets_id"] == bytes(range(32)).hex()
    assert d["project_name"] == "CrossPad" and d["version"] == "1.2.3"


def test_image_without_desc(tmp_path):
    p = tmp_path / "old.bin"; p.write_bytes(image(with_desc=False))
    d = fw_image_desc.read(p)
    assert d["board_rev"] is None and d["project_name"] == "CrossPad"


def test_not_an_image(tmp_path):
    p = tmp_path / "junk.bin"; p.write_bytes(b"hello" * 100)
    assert fw_image_desc.read(p) is None
