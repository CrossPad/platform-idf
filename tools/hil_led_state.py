#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run led_state`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "led_state"], "hil_led_state.py", "crosspad-hil run led_state")
