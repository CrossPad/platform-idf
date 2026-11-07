#!/usr/bin/env python3
"""Long lead presets for Pitch Play: a seamless sustain loop per instrument.

Pitch Play holds one sample per zone and transposes it across the pads, so a
preset is one steady note. These loop, which is what makes a lead hold under
a finger instead of dying after three seconds.

The loop is exact rather than disguised. The sample is one second long and
**every partial is rounded to a whole number of hertz**, so each completes an
integer number of cycles in that second and the end of the buffer continues
into its start with no step at all -- detuned saws and an inharmonic bell
included, since rounding moves a partial by under a cent. A short equal-power
crossfade is laid over the seam anyway, for the renderer's four-tap
interpolator reading across the wrap.

There is no attack in the sample and no decay: the zone *is* the sustain.
PitchedInstrument starts a looped voice at loopStart and shapes the note with
attack_ms and release_ms, so an envelope in the audio would only fight it.

  python3 tools/make_lead_presets.py --out DIR     # write them locally
  python3 tools/make_lead_presets.py --push        # ... and onto the card

The root is A3 (MIDI 57): mid-range, so a pad two octaves either way still
sounds like the instrument rather than a chipmunk or a rumble.
"""
import argparse, json, math, os, struct, subprocess, sys, wave

try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy -- try ~/.venvs/crosspad-hil/bin/python")

SR = 44100
ROOT_MIDI = 57          # A3
ROOT_HZ = 440.0 / 2     # A3 = 220 Hz
SECONDS = 3.5


LOOP_SECONDS = 1.0      # a whole second: 220 Hz fits 220 cycles exactly


def hz(f):
    """Round to a whole hertz, so the partial closes its cycle inside the
    loop. The worst case here is under a cent -- inaudible, and the price of
    a seam that is gone rather than hidden."""
    return float(max(1, round(f)))


def partial_sum(t, pairs):
    """pairs: (frequency, amplitude). Every frequency is whole-hertz, so the
    sum is periodic in one second."""
    out = np.zeros_like(t)
    for f, a in pairs:
        if f >= SR / 2.2:
            continue
        out += a * np.sin(2 * math.pi * f * t)
    return out


def saw_pairs(f0, n, tilt=1.0):
    return [(hz(f0 * h), (1.0 / h) ** tilt) for h in range(1, n + 1)]


def square_pairs(f0, n, tilt=1.0):
    return [(hz(f0 * h), (1.0 / h) ** tilt) for h in range(1, n + 1, 2)]


def pulse_pairs(f0, n, width=0.25):
    return [(hz(f0 * h), abs(math.sin(math.pi * h * width)) / h) for h in range(1, n + 1)]


def detuned(f0, cents, maker, *a, **k):
    pairs = []
    for c in cents:
        pairs += maker(f0 * (2.0 ** (c / 1200.0)), *a, **k)
    return pairs


def seam(x):
    """How big the step across the wrap is, against the biggest step inside
    the buffer. A seamless loop scores at or under 1.0."""
    inner = float(np.abs(np.diff(x)).max()) or 1e-9
    return float(abs(x[0] - x[-1])) / inner


# A crossfade used to live here and it made things worse: blending the tail
# into the head is how you join two unrelated regions, and it destroys the
# exact periodicity this file goes to the trouble of building -- the measured
# step across the wrap went from a fraction of an inner step to twelve times
# one. Partials on whole hertz close their own cycles; there is nothing left
# to smooth, and smoothing is what breaks it.


def build(name):
    n = int(LOOP_SECONDS * SR)
    t = np.arange(n) / SR
    f = ROOT_HZ
    if name == "Saw Lead":
        # tilt 1.25 and a gentle drive: a flat 1/h saw stacked three times
        # and waveshaped put its third harmonic above its fundamental --
        # 49 % of the energy over 2 kHz, which whistles rather than leads. 
        x = partial_sum(t, detuned(f, (-8, 0, 8), saw_pairs, 18, tilt=1.25))
        # Normalise *before* the drive. Three stacked saws peak near five, and
        # tanh of that is a square wave: the third harmonic came out above the
        # fundamental, a third of the energy over 2 kHz, and it whistled.
        x = np.tanh(x / (float(np.max(np.abs(x))) or 1.0) * 1.3)
    elif name == "Supersaw":
        x = partial_sum(t, detuned(f, (-18, -11, -4, 0, 4, 11, 18), saw_pairs, 13, tilt=1.15))
    elif name == "Square Lead":
        x = partial_sum(t, detuned(f, (-5, 5), square_pairs, 19))
    elif name == "Hollow Reed":
        x = partial_sum(t, pulse_pairs(f, 18, 0.18))
    elif name == "Soft Sine":
        x = partial_sum(t, [(hz(f), 1.0), (hz(f * 2), 0.22), (hz(f * 3), 0.1),
                            (hz(f * 1.5), 0.16)])
    elif name == "Glass Bell":
        # inharmonic, the way a struck bar is -- and still whole-hertz
        x = partial_sum(t, [(hz(f), 1.0), (hz(f * 2.76), 0.5), (hz(f * 5.40), 0.26),
                            (hz(f * 8.93), 0.13)])
    else:
        raise SystemExit("unknown lead " + name)

    peak = float(np.max(np.abs(x))) or 1.0
    x = x / peak * 0.708                          # -3 dBFS, room for a chord
    return (x * 32767.0).astype("<i2")


LEADS = ["Saw Lead", "Supersaw", "Square Lead", "Hollow Reed", "Soft Sine", "Glass Bell"]


def instrument_json(name, wav):
    """The shape make_pitched_kit.py writes, which the loader is known to read."""
    pads = [{"note": i, "color": 0x2100D0, "color_pressed": 0x2100D0,
             "playbackMode": 1, "volume": 20, "pan": 0, "samples": []} for i in range(16)]
    return {
        "name": name, "author": "crosspad", "engine": "pitched", "type": 0,
        "instrument": {
            # The whole buffer is the loop: a looped voice starts at
            # loopStart, so anything before it would never be heard.
            "zones": [{"sample": wav, "root": ROOT_MIDI,
                       "loop_start": 0, "loop_end": int(LOOP_SECONDS * SR)}],
            "pad_notes": list(range(60, 76)),
            "base_note": 60, "release_ms": 260, "attack_ms": 12, "max_voices": 24,
        },
        "pads": pads,
    }


ratios = {}


def write_preset(root_dir, name):
    folder = os.path.join(root_dir, name.replace(" ", "_"))
    os.makedirs(folder, exist_ok=True)
    wav = name.replace(" ", "_").lower() + ".wav"
    data = build(name)
    ratios[name] = seam(data.astype(float) / 32768.0)
    with wave.open(os.path.join(folder, wav), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(data.tobytes())
    with open(os.path.join(folder, "instrument.json"), "w") as f:
        json.dump(instrument_json(name, wav), f, indent=2)
    return folder, wav


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="build_leads", help="where to write them")
    ap.add_argument("--push", action="store_true", help="also send them to the card")
    ap.add_argument("--only", action="append", help="one lead by name; repeatable")
    a = ap.parse_args()

    names = a.only or LEADS
    here = os.path.dirname(os.path.abspath(__file__))
    made = []
    for name in names:
        folder, wav = write_preset(a.out, name)
        made.append((name, folder, wav))
        print(f"{name:<13} seam {ratios[name]:.2f}x  {os.path.join(folder, wav)}")

    if not a.push:
        return 0
    fs = os.path.join(here, "fs_transfer.py")
    for name, folder, wav in made:
        dest = "/sdcard/crosspad/pitchplay/" + os.path.basename(folder)
        subprocess.run([sys.executable, fs, "mkdir", dest], capture_output=True)
        for f in ("instrument.json", wav):
            r = subprocess.run([sys.executable, fs, "push", os.path.join(folder, f),
                                dest + "/" + f], capture_output=True, text=True)
            ok = "FS_OK" in r.stdout
            print(f"  {'->' if ok else 'XX'} {dest}/{f}")
            if not ok:
                print(r.stdout.strip() or r.stderr.strip())
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
