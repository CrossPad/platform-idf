#!/usr/bin/env python3
"""Build a pitched-instrument fixture kit and push it to the SD card.

Companion to `make_tone_kit.py`, but for the second sample engine (spec:
docs/design/2026-09-03-pitched-instrument-design.md): 1, 2 or 4
recorded zones held whole in PSRAM, every pad playing them transposed to its
own MIDI note. `--zones 2` (the default) writes one explicit root and one
`"root": "auto"` zone, so `crosspad-hil run pitched_kit` exercises both the
manual and the load-time pitch-detector paths in one kit.

    python tools/make_pitched_kit.py                    # write + push to /sdcard/crosspad/kits/HIL_PITCHED
    python tools/make_pitched_kit.py --zones 4 --rate 48000   # 4 zones, resampled at load time
    python tools/make_pitched_kit.py --out DIR           # only write the kit locally
    python tools/make_pitched_kit.py --remove             # delete it from the card again
    python tools/make_pitched_kit.py --print-expected     # pad -> expected Hz, as JSON

The kit is discovered at the next boot (the kit scan runs once, after mount).
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from _kit_fixture import push_kit, remove_kit, sine_frames, write_wav

KIT_DIR = "/sdcard/crosspad/kits/HIL_PITCHED"

# Zone roots (MIDI note numbers) per --zones, from the design spec's fixture
# kit: A3/A4/A5/A6 == 220/440/880/1760 Hz.
ZONE_ROOTS: dict[int, list[int]] = {
    1: [69],
    2: [57, 69],
    4: [57, 69, 81, 93],
}

SAMPLE_RATE = 44100          # kits ship at 44.1 kHz; --rate exercises the load-time resampler
SECONDS = 1.0
# 0.5 s per zone keeps a four-zone fixture at 176 kB — quick to push over CDC,
# and it is what the four-zone HIL run in docs/subsystems/pitched-instrument.md
# was measured on. Longer zones fit fine since the shared engine arena raised
# the bank budget (2026-09-03); --seconds overrides either way.
SECONDS_BY_ZONES = {1: 1.0, 2: 1.0, 4: 0.5}
PEAK = 0.5                   # -6 dBFS
PAD_COUNT = 16
BASE_NOTE = 60                # pad_notes[0]; pad_notes run base_note..base_note+15
RELEASE_MS = 100
ATTACK_MS = 2
MAX_VOICES = 24
# Sixteen -6 dBFS sines at full pad volume sum to ~+9 dBFS and clip the chord
# test (measured 2026-09-03: peak 0.0 dBFS, 4 of 16 peaks lost). At 20 the
# worst-case sum is ~1.1 and a typical chord peaks around -2 dBFS.
PAD_VOLUME = 20
PAD_COLOR = 2162448            # same fixture color make_tone_kit uses


def note_to_hz(note: float) -> float:
    """MIDI note -> Hz, A4 (69) = 440 Hz — the one formula every part of this tool shares."""
    return 440.0 * (2.0 ** ((note - 69.0) / 12.0))


def expected_hz(pad: int, base_note: int = BASE_NOTE) -> float:
    """Expected pitch of `pad` on this fixture kit: pad_notes[pad] = base_note + pad."""
    return note_to_hz(base_note + pad)


def write_zone_wav(path: Path, root: int, *, rate: int, seconds: float) -> None:
    frames = sine_frames([note_to_hz(root)], rate=rate, seconds=seconds, peak=PEAK,
                         channels=1)
    write_wav(path, frames, channels=1, rate=rate)


def write_kit(root_dir: Path, *, zones: int, rate: int, seconds: float,
              pad_volume: int = PAD_VOLUME) -> None:
    if zones not in ZONE_ROOTS:
        raise ValueError(f"--zones must be one of {sorted(ZONE_ROOTS)}, got {zones}")
    roots = ZONE_ROOTS[zones]
    samples_dir = root_dir / "SAMPLES"
    samples_dir.mkdir(parents=True, exist_ok=True)

    zone_specs = []
    for i, root in enumerate(roots):
        fname = f"z{root}.wav"
        write_zone_wav(samples_dir / fname, root, rate=rate, seconds=seconds)
        # Even index -> explicit root; odd index -> "auto" (both paths exercised
        # by one kit). A single zone stays explicit (index 0 is even).
        zone_specs.append({"sample": fname, "root": root if i % 2 == 0 else "auto"})

    pad_notes = [BASE_NOTE + i for i in range(PAD_COUNT)]
    pads = [
        {
            "note": i,
            "color": PAD_COLOR,
            "color_pressed": PAD_COLOR,
            "playbackMode": 1,
            "volume": pad_volume,
            "pan": 0,
            "samples": [],
        }
        for i in range(PAD_COUNT)
    ]
    kit = {
        "name": "HIL PITCHED",
        "author": "hil",
        "engine": "pitched",
        "type": 0,
        "instrument": {
            "zones": zone_specs,
            "pad_notes": pad_notes,
            "base_note": BASE_NOTE,
            "release_ms": RELEASE_MS,
            "attack_ms": ATTACK_MS,
            "max_voices": MAX_VOICES,
        },
        "pads": pads,
    }
    (root_dir / "kit.json").write_text(json.dumps(kit, indent=2))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--zones", type=int, default=2, choices=sorted(ZONE_ROOTS),
                     help="number of zones (roots: 1->[69] 2->[57,69] 4->[57,69,81,93])")
    ap.add_argument("--rate", type=int, default=SAMPLE_RATE,
                     help="zone WAV sample rate (default 44100; e.g. 48000 to exercise "
                          "the load-time FFT resampler)")
    ap.add_argument("--seconds", type=float, default=None,
                    help="zone length in seconds (default 1.0, or 0.5 with --zones 4)")
    ap.add_argument("--pad-volume", type=int, default=PAD_VOLUME,
                    help="pads[].volume 0..100; the default keeps a 16-pad chord of -6 dBFS sines under full scale")
    ap.add_argument("--out", type=Path, help="write the kit here instead of pushing it to the card")
    ap.add_argument("--remove", action="store_true", help="delete the kit from the card")
    ap.add_argument("--print-expected", action="store_true",
                     help="print {pad: expected_hz} for pad_notes 60..75 and exit")
    a = ap.parse_args()
    if a.seconds is None:
        a.seconds = SECONDS_BY_ZONES[a.zones]

    if a.print_expected:
        print(json.dumps({str(p): round(expected_hz(p), 4) for p in range(PAD_COUNT)}, indent=2))
        return 0

    if a.remove:
        remove_kit(KIT_DIR, [f"z{root}.wav" for root in ZONE_ROOTS[a.zones]])
        print(f"removed {KIT_DIR}; reboot the board to drop it from the kit list")
        return 0

    if a.out:
        write_kit(a.out, zones=a.zones, rate=a.rate, seconds=a.seconds, pad_volume=a.pad_volume)
        print(f"kit written to {a.out}")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "HIL_PITCHED"
        write_kit(root, zones=a.zones, rate=a.rate, seconds=a.seconds, pad_volume=a.pad_volume)
        push_kit(root, KIT_DIR)
    print(f"pushed {KIT_DIR}; reboot the board so the kit scan picks it up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
