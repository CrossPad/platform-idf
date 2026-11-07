#!/usr/bin/env python3
"""Build the two interleaved sine kits the kit-swap audio scenario is scored against.

`crosspad-hil run kit_swap_audio` records the board through its own UAC2
endpoint while pads fire and the kit is swapped underneath them. To tell "pad 5
is still playing kit A" from "pad 5 has been reloaded to kit B" the two kits
must differ *per pad, in the spectrum, at a frequency nothing else in the mix
occupies*. That is the whole design of these two kits:

    kit A: pad p plays 300 + 222*p Hz
    kit B: pad p plays 411 + 222*p Hz

Interleaved by 111 Hz, so A(p) and B(p) sit either side of each other and the
analyser only has to ask which of two named bands carries the energy. The
offsets 300 and 411 are chosen so that no *harmonic* of any tone in either kit
lands on any other tone either: the closest collision over the second and third
harmonics of both ladders misses by 33 Hz, outside the +-15 Hz band the scoring
measures. A pure sine has no harmonics of its own, but the codec's distortion
does, and a fixture whose distortion products sit on another pad's band would
score a pad as "still sounding" when it is not.

Peak is 0.12 (about -18 dBFS): the default stimulus round-robins 16 pads at
4 hits/s with a 2 s tone, so up to 8 tones overlap and the sum still stays
under full scale.

    python tools/make_sine_kits.py               # write + push both to /sdcard/crosspad/kits/
    python tools/make_sine_kits.py --out DIR     # only write them locally
    python tools/make_sine_kits.py --remove      # delete both from the card again

The kits are discovered at the next boot (the kit scan runs once, after mount).

`crosspad-hil run kit_swap_audio` provisions the same two kits by itself when
the card does not have them, from `crosspad_hil/fixtures.py`. That copy is the
one the scenario uses, so this tool prefers it when crosspad-hil is importable
and falls back to the local generator otherwise — the two produce byte-identical
kits, and `crosspad-hil`'s `tests/test_fixtures.py` compares them file by file
whenever both repos are checked out side by side. Change one, change the other.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from _kit_fixture import push_kit, remove_kit, sine_frames, write_wav

try:                                  # the library copy, when crosspad-hil is installed
    from crosspad_hil import fixtures as _hil_fixtures
except ImportError:                   # a bare checkout: the local generator below
    _hil_fixtures = None

KITS_ROOT = "/sdcard/crosspad/kits"
PAD_COUNT = 16
SAMPLE_RATE = 44100          # kits ship at 44.1 kHz; the engine plays files 1:1
SECONDS = 2.0                # the tone length the scenario's scoring window assumes
PEAK = 0.12                  # ~-18 dBFS; 8 overlapping tones stay under full scale
PAD_COLOR = 2162448          # the fixture colour make_tone_kit uses
PAD_VOLUME = 100

# The two frequency ladders. Same step, offsets 111 Hz apart.
STEP_HZ = 222.0
BASE_A_HZ = 300.0
BASE_B_HZ = 411.0


def freq_a(pad: int) -> float:
    """Kit A, pad p: 300 + 222*p Hz."""
    return BASE_A_HZ + STEP_HZ * pad


def freq_b(pad: int) -> float:
    """Kit B, pad p: 411 + 222*p Hz — every A tone's neighbour, 111 Hz above."""
    return BASE_B_HZ + STEP_HZ * pad


# (directory name, kit.json name, frequency formula) — everything that differs
# between the two kits, in one place.
KITS = (
    ("HIL_SINE_A", "HIL SINE A", freq_a),
    ("HIL_SINE_B", "HIL SINE B", freq_b),
)

SAMPLE_NAMES = [f"pad{p:02d}.wav" for p in range(PAD_COUNT)]


def write_kit(root: Path, kit_name: str, freq, *, seconds: float = SECONDS,
              rate: int = SAMPLE_RATE, peak: float = PEAK) -> None:
    """One kit directory: kit.json plus one stereo sine per pad.

    The local generator. `write_kits()` prefers `crosspad_hil.fixtures` when it
    is importable; this stays so the tool works in a bare platform-idf checkout,
    and so the cross-check test has two implementations to compare.
    """
    (root / "SAMPLES").mkdir(parents=True, exist_ok=True)
    pads = []
    for pad in range(PAD_COUNT):
        frames = sine_frames([freq(pad)], rate=rate, seconds=seconds, peak=peak, channels=2)
        write_wav(root / "SAMPLES" / SAMPLE_NAMES[pad], frames, channels=2, rate=rate)
        # playbackMode absent = one-shot: the tone runs to its end whatever the
        # gate does, which is what makes a fixed-length scoring window possible.
        pads.append({"color": PAD_COLOR, "color_pressed": PAD_COLOR, "choke_group": 0,
                     "cycle": False, "pitch": 0, "volume": PAD_VOLUME, "pan": 0,
                     "samples": [SAMPLE_NAMES[pad]]})
    kit = {"name": kit_name, "author": "hil",
           "description": [f"per-pad sine fixture for kit_swap_audio ({kit_name})"],
           "version": "1.0", "created_at": "2026-09-06", "pads": pads}
    (root / "kit.json").write_text(json.dumps(kit))


def write_kits(out: Path, *, seconds: float = SECONDS, peak: float = PEAK) -> None:
    """Both kits under `out`, one directory each. Uses the crosspad-hil copy when
    it is importable so the scenario and this tool cannot drift apart."""
    if _hil_fixtures is not None:
        _hil_fixtures.write_sine_kits(out, seconds=seconds, peak=peak)
        return
    for dirname, name, freq in KITS:
        write_kit(out / dirname, name, freq, seconds=seconds, peak=peak)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path,
                    help="write both kits here instead of pushing them to the card")
    ap.add_argument("--remove", action="store_true", help="delete both kits from the card")
    ap.add_argument("--seconds", type=float, default=SECONDS,
                    help="tone length; the scenario's --tone-s must match (default 2.0)")
    ap.add_argument("--peak", type=float, default=PEAK,
                    help="tone peak 0..1 (default 0.12, ~-18 dBFS)")
    ap.add_argument("--print-expected", action="store_true",
                    help="print {pad: [kit A Hz, kit B Hz]} as JSON and exit")
    a = ap.parse_args()

    if a.print_expected:
        print(json.dumps({str(p): [freq_a(p), freq_b(p)] for p in range(PAD_COUNT)}, indent=2))
        return 0

    if a.remove:
        for dirname, _name, _freq in KITS:
            remove_kit(f"{KITS_ROOT}/{dirname}", SAMPLE_NAMES)
            print(f"removed {KITS_ROOT}/{dirname}")
        print("reboot the board to drop them from the kit list")
        return 0

    if a.out:
        write_kits(a.out, seconds=a.seconds, peak=a.peak)
        for dirname, _name, _freq in KITS:
            print(f"kit written to {a.out / dirname}")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        write_kits(Path(tmp), seconds=a.seconds, peak=a.peak)
        for dirname, _name, _freq in KITS:
            push_kit(Path(tmp) / dirname, f"{KITS_ROOT}/{dirname}")
            print(f"pushed {KITS_ROOT}/{dirname}")
    print("reboot the board so the kit scan picks them up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
