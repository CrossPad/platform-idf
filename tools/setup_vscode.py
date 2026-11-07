#!/usr/bin/env python3
"""Generate .vscode/settings.json from .vscode/settings.template.json.

Substitutes @@IDF_PYTHON_ENV@@ with detected ESP-IDF python venv folder
(e.g. idf5.5_py3.12_env) found under ~/.espressif/python_env/.

Idempotent — does nothing if settings.json already exists. Run again with
--force to regenerate.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = REPO_ROOT / ".vscode" / "settings.template.json"
OUT = REPO_ROOT / ".vscode" / "settings.json"


def detect_python_env() -> str | None:
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        name = Path(env).name
        if name:
            return name

    tools_path = Path(os.environ.get("IDF_TOOLS_PATH") or Path.home() / ".espressif")
    venv_root = tools_path / "python_env"
    if not venv_root.is_dir():
        return None

    candidates = sorted(
        (p.name for p in venv_root.iterdir()
         if p.is_dir() and re.match(r"idf[\d.]+_py[\d.]+_env$", p.name)),
        reverse=True,
    )
    return candidates[0] if candidates else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--force", action="store_true",
                    help="overwrite existing settings.json")
    args = ap.parse_args()

    if not TEMPLATE.is_file():
        print(f"[setup_vscode] template missing: {TEMPLATE}", file=sys.stderr)
        return 1

    if OUT.exists() and not args.force:
        return 0

    venv = detect_python_env()
    if venv is None:
        print("[setup_vscode] no ESP-IDF python venv found under "
              "~/.espressif/python_env/. Run ESP-IDF install.sh first, "
              "then re-run this script.", file=sys.stderr)
        return 2

    content = TEMPLATE.read_text(encoding="utf-8")
    content = content.replace("@@IDF_PYTHON_ENV@@", venv)
    OUT.write_text(content, encoding="utf-8")
    print(f"[setup_vscode] wrote {OUT.relative_to(REPO_ROOT)} "
          f"(IDF python env: {venv})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
