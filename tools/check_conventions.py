#!/usr/bin/env python3
"""Project rules a compiler does not check (CLAUDE.md, Rules and Hardware Revisions).

  check_conventions.py [--repo DIR]     exit 0 clean, 1 with file:line per finding

  board-rev      CONFIG_BSP_BOARD_REV_* is read in the pinout selector only;
                 everything else branches on a BSP_HAS_* capability.
  constructor    Apps register through REGISTER_APP. Static constructors do not
                 run reliably on ESP-IDF, so __attribute__((constructor)) and
                 file-scope AppRegistrar objects are refused.
  parent-scope   An app's CMakeLists.txt sets its sources without PARENT_SCOPE;
                 the parent collects them.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# First-party trees. Submodules run this in their own repositories.
SOURCE_DIRS = ("main", "components/bsp", "components/crosspad-platform-idf", "components/stm32_hal")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".cmake"}

# Where a board revision may be named: the selector, and the SD header that
# pulls the same pinout in for its pin macros.
BOARD_REV_ALLOWED = {
    "components/bsp/crosspad/include/crosspad_s3.h",
    "components/bsp/crosspad/include/bsp_sdmmc.h",
}
BOARD_REV_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\bCONFIG_BSP_BOARD_REV_")
CONSTRUCTOR_RE = re.compile(r"__attribute__\s*\(\(\s*constructor")
REGISTRAR_OBJECT_RE = re.compile(r"^\s*(?:static\s+)?(?:crosspad::)?AppRegistrar\s+\w+\s*[({;=]")
PARENT_SCOPE_RE = re.compile(r"\bset\s*\([^)]*\bPARENT_SCOPE\b")


def tracked_files(repo: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(repo), "ls-files", "--", *SOURCE_DIRS],
                         check=True, capture_output=True, text=True).stdout
    return out.splitlines()


def findings(repo: Path, files: list[str] | None = None) -> list[str]:
    out: list[str] = []
    for rel in files if files is not None else tracked_files(repo):
        path = repo / rel
        name = path.name
        is_source = path.suffix in SOURCE_SUFFIXES
        is_cmake = name == "CMakeLists.txt"
        if not (is_source or is_cmake) or not path.is_file():
            continue
        is_app_cmake = is_cmake and rel.startswith("main/app/") and rel.count("/") == 3
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if is_source and rel not in BOARD_REV_ALLOWED and BOARD_REV_RE.search(line):
                out.append(f"{rel}:{n}: board-rev: branch on a BSP_HAS_* capability, not on the revision")
            if is_source and CONSTRUCTOR_RE.search(line):
                out.append(f"{rel}:{n}: constructor: use REGISTER_APP, static constructors do not run reliably here")
            if is_source and REGISTRAR_OBJECT_RE.search(line):
                out.append(f"{rel}:{n}: constructor: a file-scope AppRegistrar object; use REGISTER_APP")
            if is_app_cmake and PARENT_SCOPE_RE.search(line):
                out.append(f"{rel}:{n}: parent-scope: set the app's sources without PARENT_SCOPE")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", type=Path, default=REPO)
    args = ap.parse_args()
    found = findings(args.repo)
    for line in found:
        print(line)
    if not found:
        print("conventions hold")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
