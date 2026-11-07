import struct, sys, zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fw_icon

COLOR_PLANE = 48 * 48 * 2
HEADER = 12


def solid(w, h, rgba):
    return fw_icon.write_png(w, h, [rgba] * (w * h))


def encode_rgb(w, h, rows, filters):
    """An RGB PNG whose rows use the given filter types in turn (what image editors write)."""
    bpp, out, prev = 3, b"", bytes(w * 3)
    for y, row in enumerate(rows):
        f = filters[y % len(filters)]
        line = bytearray()
        for i in range(len(row)):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) >> 1
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            line.append((row[i] - pred) & 0xFF)
        out += bytes([f]) + bytes(line)
        prev = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return fw_icon.PNG_SIG + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(out)) + chunk(b"IEND", b"")


def test_converts_to_lvgl_rgb565a8():
    out = fw_icon.convert(solid(64, 64, (255, 0, 0, 255)))
    assert len(out) == fw_icon.ICON_BYTES == 6924
    assert struct.unpack("<BBHHHHH", out[:HEADER]) == (0x19, 0x14, 0, 48, 48, 96, 0)
    assert out[HEADER:HEADER + 2] == struct.pack("<H", 0xF800)
    assert out[HEADER + COLOR_PLANE] == 255


def test_alpha_is_area_averaged_and_colour_premultiplied():
    # Transparent black left of x=31, opaque white from it: output column 23
    # covers a third of source 30 and all of source 31.
    px = [(0, 0, 0, 0) if x < 31 else (255, 255, 255, 255) for y in range(64) for x in range(64)]
    out = fw_icon.convert(fw_icon.write_png(64, 64, px))
    alpha = out[HEADER + COLOR_PLANE:]
    assert alpha[0] == 0 and alpha[47] == 255
    assert alpha[23] == 191                                   # 3/4 covered
    colour23 = struct.unpack("<H", out[HEADER + 23 * 2:HEADER + 23 * 2 + 2])[0]
    assert colour23 == 0xFFFF                                 # not darkened by the transparent black


def test_reads_rgb_with_every_filter():
    w = h = fw_icon.SRC_SIZE
    rows = [bytes((x * 7 + y * 13 + c * 29) & 0xFF for x in range(w) for c in range(3)) for y in range(h)]
    width, height, px = fw_icon.read_png(encode_rgb(w, h, rows, [0, 1, 2, 3, 4]))
    assert (width, height) == (w, h)
    for y in range(h):
        for x in range(w):
            r, g, b = rows[y][x * 3:x * 3 + 3]
            assert px[y * w + x] == (r, g, b, 255)


def test_refuses_what_it_cannot_convert():
    with pytest.raises(fw_icon.IconError, match="64x64"):
        fw_icon.convert(solid(32, 32, (0, 0, 0, 255)))
    with pytest.raises(fw_icon.IconError, match="not a PNG"):
        fw_icon.convert(b"GIF89a....")
    assert fw_icon.check_bytes(solid(64, 64, (1, 2, 3, 255))) == []
    assert fw_icon.check_bytes(b"x")


def test_same_png_same_bytes():
    png = fw_icon.grid_png(4, (255, 136, 0))
    assert fw_icon.convert(png) == fw_icon.convert(png)
    assert fw_icon.check_bytes(png) == []


def chunk(kind, body, crc=None):
    crc = zlib.crc32(kind + body) & 0xFFFFFFFF if crc is None else crc
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)


def png_of(raw, w=64, h=64, depth=8, color=6, interlace=0, ihdr=None):
    ihdr = struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, interlace) if ihdr is None else ihdr
    return fw_icon.PNG_SIG + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


GOOD_RAW = (b"\x00" + bytes(64 * 4)) * 64


def test_a_small_zlib_bomb_is_refused_without_inflating_it():
    bomb = png_of(bytes(300 * 1024 * 1024))             # ~300 kB file, 300 MB inflated
    assert len(bomb) < 1024 * 1024
    with pytest.raises(fw_icon.IconError, match="size"):
        fw_icon.read_png(bomb)


def test_dimensions_are_refused_before_any_decode():
    with pytest.raises(fw_icon.IconError, match="64x64"):
        fw_icon.read_png(png_of(b"", w=60000, h=60000))


def test_short_data_is_refused():
    with pytest.raises(fw_icon.IconError, match="size"):
        fw_icon.read_png(png_of(GOOD_RAW[:-1]))


@pytest.mark.parametrize("name, make", [
    ("signature", lambda: b"\x89PNG\r\n\x1a\x0b" + png_of(GOOD_RAW)[8:]),
    ("short IHDR", lambda: png_of(GOOD_RAW, ihdr=b"\x00" * 5)),
    ("chunk past the end", lambda: png_of(GOOD_RAW)[:-20]),
    ("oversized length", lambda: fw_icon.PNG_SIG + struct.pack(">I", 0x7FFFFFFF) + b"IHDR" + b"\x00" * 20),
    ("CRC", lambda: fw_icon.PNG_SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0), crc=1)
     + chunk(b"IEND", b"")),
    ("no IEND", lambda: png_of(GOOD_RAW)[:-12]),
    ("no IHDR", lambda: fw_icon.PNG_SIG + chunk(b"IEND", b"")),
    ("depth", lambda: png_of(GOOD_RAW, depth=16)),
    ("colour type", lambda: png_of(GOOD_RAW, color=3)),
    ("interlace", lambda: png_of(GOOD_RAW, interlace=1)),
    ("not zlib", lambda: fw_icon.PNG_SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0))
     + chunk(b"IDAT", b"garbage") + chunk(b"IEND", b"")),
])
def test_malformed_png_raises_icon_error(name, make):
    with pytest.raises(fw_icon.IconError):
        fw_icon.read_png(make())
    assert fw_icon.check_bytes(make())


def test_well_formed_synthetic_png_converts():
    assert len(fw_icon.convert(png_of(GOOD_RAW))) == fw_icon.ICON_BYTES
