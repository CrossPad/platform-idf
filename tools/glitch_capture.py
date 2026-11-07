#!/usr/bin/env python3
"""One-shot capture harness for deep USB-audio glitch analysis.

Generates reference signals, plays them through the CrossPad UAC2 output
while recording the return path, and stores raw WAVs for offline analysis.
Assumes the device is already in USB audio mode (card "Crosspad Audio").
"""
import argparse
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np

RATE = 48000
CARD = "hw:Audio,0"


def write_wav(path: Path, data: np.ndarray):
    """data: float in [-1, 1], shape (n,) or (n, 2)."""
    if data.ndim == 1:
        data = np.column_stack([data, data])
    pcm = (np.clip(data, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def gen_sine(freq: float, seconds: float, amp: float = 0.5) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    return amp * np.sin(2 * np.pi * freq * t)


def gen_multitone(freqs, seconds: float, amp: float = 0.25) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    sig = np.zeros_like(t)
    for i, f in enumerate(freqs):
        sig += amp * np.sin(2 * np.pi * f * t + i * 1.234)
    return sig / max(1.0, np.abs(sig).max() / 0.7)


def gen_chirp(f0: float, f1: float, seconds: float, amp: float = 0.5) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    k = (f1 / f0) ** (1 / seconds)
    phase = 2 * np.pi * f0 * (k ** t - 1) / np.log(k)
    return amp * np.sin(phase)


def capture(workdir: Path, name: str, ref: np.ndarray | None, seconds: float):
    cap_path = workdir / f"cap_{name}.wav"
    rec = subprocess.Popen(
        ["arecord", "-D", CARD, "-f", "S16_LE", "-r", str(RATE), "-c", "2",
         "-d", str(int(seconds + 2)), str(cap_path)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.8)  # let capture stream settle before playback starts
    if ref is not None:
        ref_path = workdir / f"ref_{name}.wav"
        if not ref_path.exists():
            write_wav(ref_path, ref)
        play = subprocess.run(
            ["aplay", "-D", CARD, str(ref_path)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if play.returncode != 0:
            print(f"  aplay FAILED: {play.stderr.decode()}", flush=True)
    rec.wait(timeout=seconds + 15)
    err = rec.stderr.read().decode()
    ok = rec.returncode == 0
    print(f"  {name}: {'OK' if ok else 'FAIL ' + err}", flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--seconds", type=float, default=20.0)
    args = ap.parse_args()
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    s = args.seconds

    print("E1: silence x2 (noise floor / spontaneous artifacts)", flush=True)
    for i in range(2):
        capture(workdir, f"silence_{i}", None, 12)

    print("E2: sine 997 Hz x3 (determinism check)", flush=True)
    sine = gen_sine(997.0, s)
    for i in range(3):
        capture(workdir, f"sine997_{i}", sine, s)
        time.sleep(1)

    print("E3: multitone 440/997/3181", flush=True)
    capture(workdir, "multitone", gen_multitone([440, 997, 3181], s), s)

    print("E4: log chirp 100->10k", flush=True)
    capture(workdir, "chirp", gen_chirp(100, 10000, s), s)

    print("done:", workdir, flush=True)


if __name__ == "__main__":
    sys.exit(main())
