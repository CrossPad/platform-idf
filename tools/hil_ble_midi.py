#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run ble_midi`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "ble_midi"], "hil_ble_midi.py", "crosspad-hil run ble_midi")
