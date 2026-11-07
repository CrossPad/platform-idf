"""Read what a CrossPad firmware image says about itself.

esp_app_desc_t sits at 0x20 of every ESP-IDF app image; the CrossPad block
("CPFI") follows it at 0x120. Images built before the block existed have the
first and not the second.
"""
import struct
from pathlib import Path

APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
FW_DESC_OFFSET = 0x120
FW_DESC_MAGIC = 0x49465043
_FW_DESC = struct.Struct("<IBBH16s32s")


def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def read(path):
    data = Path(path).read_bytes()[:FW_DESC_OFFSET + _FW_DESC.size]
    if len(data) < APP_DESC_OFFSET + 0x100:
        return None
    if struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        return None
    out = {"version": _cstr(data[APP_DESC_OFFSET + 0x10:APP_DESC_OFFSET + 0x30]),
           "project_name": _cstr(data[APP_DESC_OFFSET + 0x30:APP_DESC_OFFSET + 0x50]),
           "board_rev": None, "stm_proto_min": None, "kit_id": None, "assets_id": None}
    if len(data) >= FW_DESC_OFFSET + _FW_DESC.size:
        magic, ver, rev, proto, kit, assets = _FW_DESC.unpack_from(data, FW_DESC_OFFSET)
        if magic == FW_DESC_MAGIC and ver == 1:
            out.update(board_rev=rev, stm_proto_min=proto, kit_id=_cstr(kit),
                       assets_id=assets.hex())
    return out
