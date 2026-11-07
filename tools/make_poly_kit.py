#!/usr/bin/env python3
"""Build a polyphony fixture: sixteen distinct stereo notes, one per pad.

What it is for: answering "how many stereo WAVs can play at once" with a
number that can be quoted. Every pad is the same length, the same rate and
the same level, so the only variable in a sweep is how many voices sound.

Distinct pitches (a chromatic run from A3) rather than one note sixteen
times: identical files would phase-cancel and would also share a single
SampleStore slot, which is not sixteen samples being read.

    python3 tools/make_poly_kit.py              # 1.0 s a pad: fits a head, no card
    python3 tools/make_poly_kit.py --seconds 4  # longer than any head: streams
    python3 tools/make_poly_kit.py --remove
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from _kit_fixture import push_kit, remove_kit, sine_frames, write_wav

KIT_DIR = "/sdcard/crosspad/kits/POLY16"   # --name picks another
SAMPLE_RATE = 44100
PEAK = 0.25                  # sixteen at once must not clip the bus
BASE_HZ = 220.0              # A3, then a chromatic run
NAMES = [f"{i+1}.wav" for i in range(16)]


def build(root: Path, seconds: float, loop: bool = False,
          kit_name: str = "POLY16") -> None:
    (root / "SAMPLES").mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(NAMES):
        hz = BASE_HZ * (2.0 ** (i / 12.0))
        frames = sine_frames([hz], rate=SAMPLE_RATE, seconds=seconds,
                             peak=PEAK, channels=2)
        write_wav(root / "SAMPLES" / name, frames, channels=2, rate=SAMPLE_RATE)
    pads = [{"color": 2162448, "color_pressed": 2162448, "choke_group": 0,
             "cycle": False, "pitch": 0, "volume": 100, "pan": 0,
             "playbackMode": 2 if loop else 0,
             "samples": [{"sample": n, "start": 0, "end": 0}]} for n in NAMES]
    (root / "kit.json").write_text(json.dumps({
        "name": kit_name, "author": "fixture", "description":
        f"16 stereo notes, {seconds:g} s, {SAMPLE_RATE} Hz -- polyphony sweep",
        "version": "1.0", "pads": pads}))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=1.0,
                    help="per sample; short enough to be resident, or long enough to stream")
    ap.add_argument("--out", help="write here instead of pushing")
    ap.add_argument("--loop", action="store_true",
                    help="pads loop, so a voice sustains until it is stopped -- "
                         "what a polyphony sweep needs to know how many are really sounding")
    ap.add_argument("--name", default="POLY16",
                    help="kit directory and display name, for a second fixture "
                         "(a kit whose samples the player still has open cannot be "
                         "overwritten -- FS_ERR delete_13/rename_17)")
    ap.add_argument("--remove", action="store_true")
    a = ap.parse_args()
    kit_dir = f"/sdcard/crosspad/kits/{a.name}"
    if a.remove:
        remove_kit(kit_dir, NAMES)
        return 0
    if a.out:
        build(Path(a.out), a.seconds, a.loop, a.name)
        return 0
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        build(root, a.seconds, a.loop, a.name)
        total = sum(f.stat().st_size for f in (root / "SAMPLES").glob("*.wav"))
        print(f"16 x {a.seconds:g} s stereo = {total/1024:.0f} kB, pushing to {kit_dir}")
        push_kit(root, kit_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
