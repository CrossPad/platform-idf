#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil run kit_churn`. Implementation lives in the crosspad-hil package."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["run", "kit_churn"], "hil_kit_churn.py", "crosspad-hil run kit_churn")
