#!/usr/bin/env python3
"""Build a kit's waveform.cache off the board, so it ships inside the kit.

Without one the board reads every sample end to end after the kit is first
picked -- the waveform loader walks the whole file for each layer, beside the
streamer. A kit that carries the file is drawn at once.

    python3 tools/make_waveform_cache.py KIT_DIR [KIT_DIR ...]   # write KIT_DIR/waveform.cache
    python3 tools/make_waveform_cache.py --all KITS_ROOT          # every kit under a kits folder
    python3 tools/make_waveform_cache.py --check KIT_DIR          # compare with the file there

Entry names are relative to the kit's folder ("SAMPLES/1.wav"), which the
firmware resolves against wherever the kit is installed; a sample named with an
absolute path in kit.json is outside the kit and is left to the board.

Mirrors, byte for byte: crosspad-sampler waveform_cache_save_to_disk (format v3),
waveform_loader_preload_kit (which samples, in which order) and
platform_wf_getWaveformSamples in main/gui/gui.cpp (the peak per bin).
"""
from __future__ import annotations

import argparse
import array
import json
import struct
import sys
from pathlib import Path

CACHE_MAGIC = b"WFCA"
CACHE_VERSION = 3
CACHE_NAME = "waveform.cache"
TARGET_CACHE_SAMPLES = 600       # SamplerConfig.hpp
WAVEFORM_CACHE_SIZE = 64         # SamplerConfig.hpp: entries the board keeps
KIT_PADS = 16
MAX_NAME_BYTES = 127             # the reader stops at a longer name
MAX_CACHE_BYTES = 256 * 1024     # the reader reads no more than this


def parse_wav(path: Path) -> dict | None:
    """parseWavHeader(): first fmt chunk, data chunk, sampleCount from its size."""
    data = path.read_bytes()
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos, fmt, data_off, data_size = 12, None, None, 0
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        start = pos + 8
        if cid == b"fmt " and size >= 16:
            _, channels, rate, _, block_align, bits = struct.unpack_from("<HHIIHH", data, start)
            fmt = dict(channels=channels, rate=rate, block_align=block_align, bits=bits)
        elif cid == b"data":
            data_off, data_size = start, size
            break
        pos = start + size + (size & 1)
    if fmt is None or data_off is None or fmt["block_align"] == 0:
        return None
    return dict(fmt, frames=data_size // fmt["block_align"],
                pcm=memoryview(data)[data_off:data_off + data_size])


def first_channel(wav: dict) -> list[int]:
    """The int16 the board takes from each frame: channel 0, top 16 bits of 24."""
    pcm, ba, bits, n = wav["pcm"], wav["block_align"], wav["bits"], wav["frames"]
    if bits == 16:
        lo, hi = pcm[0:n * ba:ba], pcm[1:n * ba:ba]
    elif bits == 24:
        lo, hi = pcm[1:n * ba:ba], pcm[2:n * ba:ba]
    else:
        return [(b - 128) << 8 for b in pcm[0:n * ba:ba]]
    words = array.array("h", bytes(x for pair in zip(lo, hi) for x in pair))
    if sys.byteorder == "big":
        words.byteswap()
    return words.tolist()


def peaks(wav: dict, out_samples: int) -> array.array | None:
    """One signed peak per bin, the first sample of the largest magnitude."""
    frames = wav["frames"]
    if frames == 0 or out_samples == 0 or len(wav["pcm"]) < frames * wav["block_align"]:
        return None                      # the board fails a short read too
    samples = first_channel(wav)
    out = array.array("h")
    for i in range(out_samples):
        lo = i * frames // out_samples
        hi = min((i + 1) * frames // out_samples, frames)
        out.append(max(samples[lo:hi], key=abs) if hi > lo else 0)
    return out


def resolve(kit_dir: Path, name: str) -> str | None:
    """resolveKitSamplePath(): flat beside kit.json first, then SAMPLES/."""
    for rel in (name, "SAMPLES/" + name):
        if (kit_dir / rel).is_file():
            return rel
    return None


def kit_samples(kit_dir: Path, warn) -> list[str]:
    """Every layer of the first 16 pads, in preload order, without repeats."""
    doc = json.loads((kit_dir / "kit.json").read_text(encoding="utf-8-sig"))
    names: list[str] = []
    for i, pad in enumerate((doc.get("pads") or [])[:KIT_PADS]):
        for s in pad.get("samples") or []:
            name = s if isinstance(s, str) else (s.get("sample") if isinstance(s, dict) else None)
            if not name:
                continue
            if name.startswith("/"):
                warn(f"pad {i}: {name} is outside the kit, left to the board")
                continue
            rel = resolve(kit_dir, name)
            if rel is None:
                warn(f"pad {i}: {name} not found")
            elif rel not in names:
                names.append(rel)
    return names


def build(kit_dir: Path, warn) -> bytes:
    entries = []
    for rel in kit_samples(kit_dir, warn):
        if len(entries) == WAVEFORM_CACHE_SIZE:
            warn(f"more than {WAVEFORM_CACHE_SIZE} samples, the rest are left to the board")
            break
        name = rel.encode("utf-8")
        if len(name) > MAX_NAME_BYTES:
            warn(f"{rel}: name longer than {MAX_NAME_BYTES} bytes, skipped")
            continue
        wav = parse_wav(kit_dir / rel)
        pk = peaks(wav, min(wav["frames"], TARGET_CACHE_SAMPLES)) if wav else None
        if pk is None:
            warn(f"{rel}: not a readable wav, skipped")
            continue
        if sys.byteorder == "big":
            pk.byteswap()
        entries.append(bytes([len(name)]) + name
                       + struct.pack("<IIIBB", len(pk), wav["frames"], wav["rate"],
                                     wav["channels"] & 0xFF, wav["bits"] & 0xFF)
                       + pk.tobytes())
    blob = CACHE_MAGIC + bytes([CACHE_VERSION, len(entries)]) + b"\0\0" + b"".join(entries)
    if len(blob) > MAX_CACHE_BYTES:
        raise SystemExit(f"{kit_dir}: cache is {len(blob)} B, the board reads {MAX_CACHE_BYTES}")
    return blob if entries else b""


def find_kits(root: Path) -> list[Path]:
    return sorted(p.parent for p in root.rglob("kit.json"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=Path, help="kit folders (with kit.json), or kits roots with --all")
    ap.add_argument("--all", action="store_true", help="every kit.json under the given folders")
    ap.add_argument("--check", action="store_true", help="compare with the existing cache, write nothing")
    args = ap.parse_args()

    kits = [k for p in args.paths for k in find_kits(p)] if args.all else args.paths
    failed = 0
    for kit in kits:
        if not (kit / "kit.json").is_file():
            print(f"{kit}: no kit.json")
            failed += 1
            continue
        warnings: list[str] = []
        blob = build(kit, warnings.append)
        target = kit / CACHE_NAME
        if args.check:
            same = target.is_file() and target.read_bytes() == blob
            print(f"{kit}: {'identical' if same else 'DIFFERENT'}")
            failed += not same
        elif blob:
            target.write_bytes(blob)
            print(f"{kit}: {blob[5]} samples, {len(blob)} B")
        else:
            print(f"{kit}: nothing to cache")
        for w in warnings:
            print(f"    {w}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
