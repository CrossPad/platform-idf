#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run usb_mode_cycle`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "usb_mode_cycle"], "hil_usb_mode_cycle.py", "crosspad-hil run usb_mode_cycle")
