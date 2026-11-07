#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run midi_bench`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "midi_bench"], "hil_midi_bench.py", "crosspad-hil run midi_bench")
