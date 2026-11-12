#!/usr/bin/env python3
"""Compare a built image with config/image_budget.json.

  check_image_budget.py --rev v2 [--elf build_v2/CrossPad.elf] [--size-tool xtensa-esp32s3-elf-size]

Prints the PSRAM pages the image takes and how far .flash.text is from its next
page (crossing it moves .flash.rodata a whole page). Exit 1 when the image
needs more pages than the budget, 2 when the ELF or the size tool is missing,
0 otherwise (also for a revision the budget does not list).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUDGET = REPO / "config" / "image_budget.json"
SECTIONS = (".ext_ram.dummy", ".flash.text", ".flash.rodata", ".iram0.text", ".dram0.bss")


def parse_size_output(text: str) -> dict[str, tuple[int, int]]:
    """`size -A` rows as {section: (bytes, address)} for the sections we track."""
    out: dict[str, tuple[int, int]] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] in SECTIONS:
            out[parts[0]] = (int(parts[1]), int(parts[2]))
    return out


def report(sections: dict[str, tuple[int, int]], page: int, budget_pages: int | None) -> tuple[list[str], bool]:
    ext, _ = sections[".ext_ram.dummy"]
    text_size, text_addr = sections[".flash.text"]
    pages = -(-ext // page)
    text_end_in_page = (text_addr + text_size) % page
    headroom = (page - text_end_in_page) % page
    lines = [f"{name:16s} {sections[name][0]:>9d} B" for name in SECTIONS if name in sections]
    lines.append(f"PSRAM pages: {pages}" + (f" (budget {budget_pages})" if budget_pages is not None else " (no budget for this revision)"))
    lines.append(f".flash.text headroom to its next page: {headroom} B")
    over = budget_pages is not None and pages > budget_pages
    if over:
        lines.append(f"OVER BUDGET: the image needs {pages - budget_pages} more 64 kB page(s) of PSRAM than config/image_budget.json allows")
    return lines, over


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rev", required=True)
    ap.add_argument("--elf", type=Path)
    ap.add_argument("--size-tool", default="xtensa-esp32s3-elf-size")
    args = ap.parse_args()
    elf = args.elf or REPO / f"build_{args.rev}" / "CrossPad.elf"
    if not elf.exists():
        print(f"{elf}: no such file")
        return 2
    try:
        text = subprocess.run([args.size_tool, "-A", str(elf)], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"{args.size_tool}: {e}")
        return 2
    sections = parse_size_output(text)
    if ".ext_ram.dummy" not in sections or ".flash.text" not in sections:
        print(f"{elf}: no .ext_ram.dummy / .flash.text section")
        return 2
    budget = json.loads(BUDGET.read_text())
    rev = budget["revisions"].get(args.rev)
    lines, over = report(sections, budget["page_bytes"], rev["ext_ram_pages"] if rev else None)
    print("\n".join(lines))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
