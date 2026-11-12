#!/usr/bin/env python3
"""Hold sdkconfig.defaults and sdkconfig.v* to config/sdkconfig_invariants.json.

A Kconfig value that a measured budget depends on is listed there with its
reason. A regenerated sdkconfig that loses one fails here, before a board shows
it as lost memory or audio glitches.

  check_sdkconfig.py [--repo DIR]      exit 0 clean, 1 with one line per problem
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INVARIANTS = Path("config") / "sdkconfig_invariants.json"
LINE_RE = re.compile(r"^(?:# )?(CONFIG_[A-Za-z0-9_]+)(?:=.*| is not set)$")


def symbol_of(line: str) -> str:
    m = LINE_RE.match(line)
    if not m:
        raise ValueError(f"not a Kconfig line: {line!r}")
    return m.group(1)


def state_of(text: str, symbol: str) -> str | None:
    """The line that sets or unsets `symbol`, or None when the file is silent."""
    for line in text.splitlines():
        m = LINE_RE.match(line.strip())
        if m and m.group(1) == symbol:
            return line.strip()
    return None


def problems(repo: Path) -> list[str]:
    spec = json.loads((repo / INVARIANTS).read_text())
    out: list[str] = []
    defaults = (repo / "sdkconfig.defaults").read_text()
    rev_files = {rev: repo / f"sdkconfig.{rev}" for rev in spec["revisions"]}

    def hold(name: str, text: str, entry: dict, silent_ok: bool) -> None:
        want = entry["line"]
        have = state_of(text, symbol_of(want))
        if have == want or (have is None and silent_ok):
            return
        out.append(f"{name}: expected `{want}`, found `{have or 'nothing'}` -- {entry['why']}")

    for entry in spec["required"]:
        # sdkconfig.defaults may stay silent; when it speaks it must agree,
        # because it wins over sdkconfig.v* for a choice.
        hold("sdkconfig.defaults", defaults, entry, silent_ok=True)
        for rev, path in rev_files.items():
            hold(path.name, path.read_text(), entry, silent_ok=False)
    for entry in spec["required_in_revisions"]:
        for rev, path in rev_files.items():
            hold(path.name, path.read_text(), entry, silent_ok=False)
    for entry in spec["forbidden_in_defaults"]:
        have = state_of(defaults, entry["symbol"])
        if have is not None and not have.startswith("# "):
            out.append(f"sdkconfig.defaults: `{have}` must not be set here -- {entry['why']}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", type=Path, default=REPO)
    args = ap.parse_args()
    found = problems(args.repo)
    for line in found:
        print(line)
    if not found:
        print("sdkconfig invariants hold")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
