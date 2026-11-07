"""Shared prologue for the deprecated tools/hil_*.py shims.

The HIL suite moved to the `crosspad-hil` package (repo CrossPad/crosspad-hil):
one implementation for the CLI, the scenarios and the crosspad-mcp daemon.
These wrappers stay for one release so existing commands and docs keep working.
"""
from __future__ import annotations

import sys


def cli_main() -> object:
    """Return crosspad_hil.cli.main, or exit 2 explaining how to install it."""
    try:
        from crosspad_hil.cli import main
    except ImportError:
        sys.stderr.write(
            "crosspad-hil is not installed in this interpreter.\n"
            "  pip install -r tools/requirements-hil.txt\n"
            "  (or, from a checkout: pip install -e /path/to/crosspad-hil)\n"
        )
        raise SystemExit(2) from None
    return main


def run(argv: list[str], old_name: str, new_cmd: str) -> None:
    sys.stderr.write(f"{old_name} is deprecated: use `{new_cmd}`\n")
    raise SystemExit(cli_main()(argv + sys.argv[1:]))
