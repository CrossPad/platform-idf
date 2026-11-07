#!/usr/bin/env python3
"""Build a one-sample fixture kit for audio measurements and push it to the SD card.

Every pad plays the same multitone (440 / 997 / 3181 Hz, the audio_loopback
reference set), so `crosspad-hil run sampler_record --kit-name "hil tone"`
records a signal the offline analyzers can judge: pitch against the file,
held/dropped frames from the second difference, THD and SNR around the tones.
A drum hit cannot answer any of those.

    python tools/make_tone_kit.py                 # write + push to /sdcard/crosspad/kits/HIL_TONE
    python tools/make_tone_kit.py --out DIR       # only write the kit locally
    python tools/make_tone_kit.py --remove        # delete it from the card again

The kit is discovered at the next boot (the kit scan runs once, after mount).
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from _kit_fixture import push_kit, remove_kit, sine_frames, write_wav

KIT_DIR = "/sdcard/crosspad/kits/HIL_TONE"
TONES_HZ = (440, 997, 3181)
SAMPLE_RATE = 44100          # the rate the kits ship at; the engine plays files 1:1
SECONDS = 3.0
PEAK = 0.5                   # sum of three tones at 1/3 each, then x0.5: -6 dBFS peak


def write_tone(path: Path) -> None:
    frames = sine_frames(TONES_HZ, rate=SAMPLE_RATE, seconds=SECONDS, peak=PEAK,
                         channels=2)
    write_wav(path, frames, channels=2, rate=SAMPLE_RATE)


def write_kit(root: Path) -> None:
    (root / "SAMPLES").mkdir(parents=True, exist_ok=True)
    write_tone(root / "SAMPLES" / "tone.wav")
    pad = {"color": 2162448, "color_pressed": 2162448, "choke_group": 0, "cycle": False,
           "pitch": 0, "volume": 100, "pan": 0, "samples": ["tone.wav"]}
    kit = {"name": "HIL TONE", "author": "hil", "description": ["multitone fixture for audio measurements"],
           "version": "1.0", "created_at": "2026-09-02", "pads": [dict(pad) for _ in range(16)]}
    (root / "kit.json").write_text(json.dumps(kit))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path, help="write the kit here instead of pushing it to the card")
    ap.add_argument("--remove", action="store_true", help="delete the kit from the card")
    a = ap.parse_args()
    if a.remove:
        remove_kit(KIT_DIR, ["tone.wav"])
        print(f"removed {KIT_DIR}; reboot the board to drop it from the kit list")
        return 0
    if a.out:
        write_kit(a.out)
        print(f"kit written to {a.out}")
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "HIL_TONE"
        write_kit(root)
        push_kit(root, KIT_DIR)
    print(f"pushed {KIT_DIR}; reboot the board so the kit scan picks it up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
