#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run stability`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "stability"], "hil_stability.py", "crosspad-hil run stability")
