#!/usr/bin/env python3
"""Deprecated shim: `crosspad-hil analyze`. Implementation lives in the crosspad-hil package.

The measurements this script made are now analysis kinds: `analyze sine WAV` for the
phase/sample-slip/glitch-periodicity report (add `--ref` and `--opt compare='[…]'` for
alignment and cross-run determinism), `analyze silence WAV` for the capture path alone
and `analyze psd WAV` for sidebands and harmonics. One WAV per invocation, one verdict
out — there is no walk-the-workdir mode.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _hil_shim import run  # noqa: E402

run(["analyze"], "glitch_analyze.py", "crosspad-hil analyze sine|silence|psd WAV")
