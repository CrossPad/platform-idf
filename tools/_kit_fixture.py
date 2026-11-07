"""Shared plumbing for the fixture-kit builders (`make_tone_kit`, `make_pitched_kit`).

Both write a kit directory into a temp dir and push it to the card with
`fs_transfer.py`; both synthesise their samples from sine tones with a short
fade so the ends are not a step. Only the tone content and the kit.json shape
differ, and those stay in the respective tools.
"""
from __future__ import annotations

import math
import struct
import subprocess
import sys
import wave
from pathlib import Path
from typing import Iterable, Sequence

HERE = Path(__file__).resolve().parent
FS_TRANSFER = HERE / "fs_transfer.py"

FADE_S = 0.005          # 5 ms fade in and out, on every fixture sample
INT16_FULL_SCALE = 32767


def fade_gain(i: int, n: int, fade: int) -> float:
    """Linear fade-in/out envelope for sample `i` of `n`, `fade` samples deep."""
    if i < fade:
        return i / fade
    if i >= n - fade:
        return (n - 1 - i) / fade
    return 1.0


def quantize(v: float) -> int:
    """Clip to full scale and quantize one float sample to int16."""
    return int(max(-1.0, min(1.0, v)) * INT16_FULL_SCALE)


def write_wav(path: Path, samples: Iterable[Sequence[int]], *, channels: int,
              rate: int) -> None:
    """Write 16-bit PCM frames (one tuple of `channels` ints per frame)."""
    frames = bytearray()
    fmt = "<" + "h" * channels
    for frame in samples:
        frames += struct.pack(fmt, *frame)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def sine_frames(freqs: Sequence[float], *, rate: int, seconds: float, peak: float,
                channels: int) -> list[tuple[int, ...]]:
    """Faded sum of `freqs`, the same signal on every channel."""
    n = int(rate * seconds)
    fade = max(1, int(rate * FADE_S))
    out = []
    for i in range(n):
        t = i / rate
        v = sum(math.sin(2 * math.pi * f * t) for f in freqs) / len(freqs) * peak
        s = quantize(v * fade_gain(i, n, fade))
        out.append((s,) * channels)
    return out


def fs(*args: str) -> None:
    """Run fs_transfer.py, raising if the device refused."""
    subprocess.run([sys.executable, str(FS_TRANSFER), *args], check=True)


def fs_mkdir(path: str) -> None:
    """mkdir that tolerates an existing directory (device answers FS_ERR mkdir_17, EEXIST)."""
    r = subprocess.run([sys.executable, str(FS_TRANSFER), "mkdir", path],
                       capture_output=True, text=True)
    if r.returncode != 0 and "mkdir_17" not in (r.stdout + r.stderr):
        sys.stderr.write(r.stdout + r.stderr)
        raise subprocess.CalledProcessError(r.returncode, r.args)


def push_kit(local_root: Path, kit_dir: str) -> None:
    """Push kit.json plus every SAMPLES/*.wav under `local_root` to `kit_dir`."""
    fs_mkdir(kit_dir)
    fs_mkdir(f"{kit_dir}/SAMPLES")
    fs("push", str(local_root / "kit.json"), f"{kit_dir}/kit.json")
    for wav in sorted((local_root / "SAMPLES").glob("*.wav")):
        fs("push", str(wav), f"{kit_dir}/SAMPLES/{wav.name}")


def remove_kit(kit_dir: str, sample_names: Sequence[str]) -> None:
    """Delete a pushed kit, innermost entry first (the device has no rm -r)."""
    for name in sample_names:
        fs("rm", f"{kit_dir}/SAMPLES/{name}")
    fs("rm", f"{kit_dir}/kit.json")
    fs("rm", f"{kit_dir}/SAMPLES")
    fs("rm", kit_dir)
