#!/usr/bin/env python3
"""Check the on-device Help page against the firmware it ships with.

main/docs/instructions.md (MODE pad 14, Help) is written by hand and drifted
three times in one month: a kit tab that no longer existed, an empty pad that
had become More, four Arrange tracks instead of eight, 4 s / 7 s power holds
after the code went to 2.4 s / 4.2 s. This reads the facts the page states
from the sources that decide them and fails on any difference:

- MODE map: every pad 1..16 listed once, with the label of crosspad-gui
  mode_overlay.cpp kDefaultMap; a pad missing from the map is "empty".
- Arrange track count (any "<n> tracks"): crosspad-core AudioSong::kTracks.
- Kit browser tabs (the "- NAME:" bullets of "## Kits"), in order:
  crosspad-sampler kit_selector.cpp kTypeName; each "+ NEW ..." key named
  must be a string in the kit selector.
- Power-off hold: core Stm32Settings kLongPressDefaultMs, equal to gui's
  default and one of the Settings presets (kLongPressOpts).
- Ship, switch-on and 0.5 s holds: the STM firmware (CrossPad_STM32_r2
  power_btn.c / power_btn.h) when --stm names a checkout -- CI checks out
  the tag of the bundled stm32_fw.bin (--stm-tag); without it only that the
  ship hold lies above the longest power-off hold the STM accepts.
- Dim and sleep defaults: core Stm32Settings dimTimeoutS / sleepTimeoutS,
  each one of the Settings presets (kTimeoutSecs).
- Every "Settings > A > B" path: each step is a string in crosspad-gui's
  sources (a renamed row or category fails).

Prose is not checked. Usage: python3 tools/help_facts.py [--stm DIR]
Exit 0 = the page matches, 1 = it does not, 2 = a source could not be read.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HELP = "main/docs/instructions.md"
MODE_MAP = "components/crosspad-gui/src/components/mode_overlay.cpp"
SETTINGS_SYSTEM = "components/crosspad-gui/src/components/settings_ui_system.cpp"
GUI_SRC = "components/crosspad-gui/src"
KIT_SELECTOR = "components/crosspad-sampler/src/ui/kit_selector.cpp"
STM32_SETTINGS = "components/crosspad-core/include/crosspad/settings/Stm32Settings.hpp"
AUDIO_SONG = "components/crosspad-core/include/crosspad/song/AudioSong.hpp"
STM_BTN_C = "app/src/power_btn.c"
STM_BTN_H = "app/inc/power_btn.h"
STM_BUNDLE = "main/stm32_fw/stm32_fw.bin"

NUMBER_WORDS = {w: i for i, w in enumerate(
    "zero one two three four five six seven eight nine ten eleven twelve "
    "thirteen fourteen fifteen sixteen".split())}


class SourceError(Exception):
    """A fact could not be read from its source: the parser needs updating."""


def _need(pattern: str, text: str, what: str, flags: int = 0) -> re.Match:
    m = re.search(pattern, text, flags)
    if not m:
        raise SourceError(f"cannot find {what}")
    return m


def _secs(ms: int) -> str:
    return f"{ms / 1000:g} s"


# ---- sources --------------------------------------------------------------

def mode_map(src: str) -> dict[int, str]:
    body = _need(r"kDefaultMap\[\]\s*=\s*\{(.*?)\n\};", src, "kDefaultMap", re.S).group(1)
    entries = {int(p): label for p, label in re.findall(r'\{\s*(\d+)\s*,\s*"([^"]*)"', body)}
    if not entries:
        raise SourceError("kDefaultMap has no entries")
    return entries


def kit_tabs(src: str) -> list[str]:
    body = _need(r"kTypeName\[[^\]]*\]\s*=\s*\{([^}]*)\}", src, "kTypeName").group(1)
    return re.findall(r'"([^"]*)"', body)


def int_const(src: str, name: str) -> int:
    return int(_need(rf"\b{name}\s*=\s*(\d+)", src, name).group(1))


def define_ms(src: str, name: str) -> int:
    return int(_need(rf"#define\s+{name}\s+(\d+)U?\b", src, name).group(1))


def u16_list(src: str, name: str) -> list[int]:
    body = _need(rf"\b{name}\[\]\s*=\s*\{{([^}}]*)\}}", src, name).group(1)
    return [int(v) for v in re.findall(r"\d+", body)]


def opts(src: str, name: str) -> list[str]:
    return _need(rf'\b{name}\s*=\s*"([^"]*)"', src, name).group(1).split("\\n")


def gui_strings(gui_dir: Path) -> set[str]:
    out: set[str] = set()
    for p in gui_dir.rglob("*"):
        if p.suffix in (".cpp", ".c", ".h", ".hpp"):
            out.update(re.findall(r'"([^"\\\n]*)"', p.read_text(errors="replace")))
    return out


# ---- the Help page ----------------------------------------------------------

def sections(md: str) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    cur = ""
    for line in md.splitlines():
        if line.startswith("## "):
            cur = line[3:].strip()
            out[cur] = []
        else:
            out.setdefault(cur, []).append(line)
    return out


def _seconds(text: str, pattern: str, what: str, problems: list[str]) -> float | None:
    m = re.search(pattern, text)
    if not m:
        problems.append(f"Help: no {what} found (expected text like {pattern!r})")
        return None
    return float(m.group(1))


def check(md: str, facts: dict) -> list[str]:
    """Return one line per difference between the Help page and the facts."""
    problems: list[str] = []
    sec = sections(md)

    # MODE map
    listed: dict[int, str] = {}
    for line in sec.get("MODE map", []):
        m = re.match(r"- (\d+)(?: ([^:]+))?:", line)
        if not m:
            continue
        pad, label = int(m.group(1)), (m.group(2) or "").strip()
        if pad in listed:
            problems.append(f"MODE map: pad {pad} listed twice")
        listed[pad] = label
    for pad in range(1, 17):
        want = facts["mode_map"].get(pad)
        if pad not in listed:
            problems.append(f"MODE map: pad {pad} missing (firmware: {want or 'empty'})")
        elif want is None and listed[pad]:
            problems.append(f"MODE map: pad {pad} is empty in the firmware, Help says {listed[pad]!r}")
        elif want is not None and listed[pad] != want:
            problems.append(f"MODE map: pad {pad} is {want!r} in the firmware, Help says "
                            f"{listed[pad] or 'empty'!r}")

    # Arrange tracks
    for word in re.findall(r"\b(\w+) tracks\b", md, re.I):
        n = int(word) if word.isdigit() else NUMBER_WORDS.get(word.lower())
        if n is not None and n != facts["tracks"]:
            problems.append(f"Help says {word} tracks, AudioSong::kTracks = {facts['tracks']}")

    # Kit browser tabs and + NEW keys
    kits = "\n".join(sec.get("Kits", []))
    tabs = re.findall(r"^- ([A-Z][A-Z0-9]+):", kits, re.M)
    if tabs != facts["kit_tabs"]:
        problems.append(f"Kits: Help lists tabs {' / '.join(tabs) or 'none'}, "
                        f"the browser has {' / '.join(facts['kit_tabs'])}")
    for key in re.findall(r"\+ NEW [A-Z]+", md):
        if key not in facts["kit_keys"]:
            problems.append(f"Kits: Help names {key!r}, the kit selector has no such key")

    # Power button
    power = "\n".join(sec.get("Power button", []))
    off = _seconds(power, r"Switch off: hold (\d+(?:\.\d+)?) s", "power-off hold", problems)
    if off is not None:
        if round(off * 1000) != facts["off_ms"]:
            problems.append(f"Power button: Help says off after {off:g} s, "
                            f"default is {_secs(facts['off_ms'])}")
        if f"{off:g}s" not in facts["off_opts"]:
            problems.append(f"Power button: {off:g} s is not a Power Long-Press choice "
                            f"({', '.join(facts['off_opts'])})")
    ship = _seconds(power, r"a (\d+(?:\.\d+)?) s hold cuts the battery", "ship-mode hold", problems)
    if ship is not None:
        if facts.get("ship_ms") is not None and round(ship * 1000) != facts["ship_ms"]:
            problems.append(f"Power button: Help says ship after {ship:g} s, "
                            f"the STM ships at {_secs(facts['ship_ms'])}")
        if round(ship * 1000) <= facts["off_max_ms"]:
            problems.append(f"Power button: ship hold {ship:g} s is not above the longest "
                            f"power-off hold ({_secs(facts['off_max_ms'])})")
    if facts.get("wake_ms") is not None:
        on = _seconds(power, r"Switch on: hold (\d+(?:\.\d+)?) s", "switch-on hold", problems)
        if on is not None and round(on * 1000) != facts["wake_ms"]:
            problems.append(f"Power button: Help says on after {on:g} s, "
                            f"the STM wakes at {_secs(facts['wake_ms'])}")
    if facts.get("short_hold_ms") is not None:
        hold = _seconds(power, r"Hold about (\d+(?:\.\d+)?) s", "short hold", problems)
        if hold is not None and round(hold * 1000) != facts["short_hold_ms"]:
            problems.append(f"Power button: Help says hold {hold:g} s for the transport bar, "
                            f"the STM's hold is {_secs(facts['short_hold_ms'])}")

    # Power saving
    saving = "\n".join(sec.get("Power saving", []))
    for what, pattern, key in (("dim", r"dims after (\d+) minutes?", "dim_s"),
                               ("sleep", r"switches off after (\d+) minutes?", "sleep_s")):
        m = re.search(pattern, saving)
        if not m:
            problems.append(f"Power saving: no {what} time found")
            continue
        said = int(m.group(1)) * 60
        if said != facts[key]:
            problems.append(f"Power saving: Help says {what} after {m.group(1)} min, "
                            f"default is {facts[key] // 60} min ({facts[key]} s)")
        if facts[key] not in facts["timeout_presets"]:
            problems.append(f"Power saving: default {what} {facts[key]} s is not a Settings choice")

    # Settings paths
    for steps in re.findall(r"Settings((?: > [A-Z][\w-]*(?: [A-Z][\w-]*)*)+)", md):
        path = "Settings" + steps
        for step in [s.strip() for s in steps.split(">")[1:]]:
            if step not in facts["gui_strings"]:
                problems.append(f"Help names {path!r}: no {step!r} in crosspad-gui")
    return problems


def read_facts(root: Path, stm: Path | None) -> dict:
    def rd(base: Path, rel: str) -> str:
        p = base / rel
        if not p.is_file():
            raise SourceError(f"{p} not found (submodule not checked out?)")
        return p.read_text(errors="replace")

    core = rd(root, STM32_SETTINGS)
    gui_sys = rd(root, SETTINGS_SYSTEM)
    selector = rd(root, KIT_SELECTOR)
    facts = {
        "mode_map": mode_map(rd(root, MODE_MAP)),
        "tracks": int_const(rd(root, AUDIO_SONG), "kTracks"),
        "kit_tabs": kit_tabs(selector),
        "kit_keys": set(re.findall(r'"(\+ NEW [A-Z]+)"', selector)),
        "off_ms": int_const(core, "kLongPressDefaultMs"),
        "off_max_ms": int_const(core, "kLongPressMaxMs"),
        "off_opts": opts(gui_sys, "kLongPressOpts"),
        "dim_s": int_const(core, "dimTimeoutS"),
        "sleep_s": int_const(core, "sleepTimeoutS"),
        "timeout_presets": u16_list(gui_sys, "kTimeoutSecs"),
        "gui_strings": gui_strings(root / GUI_SRC),
        "ship_ms": None, "wake_ms": None, "short_hold_ms": None,
    }
    gui_default = int_const(gui_sys, "kLongPressDefaultMs")
    if gui_default != facts["off_ms"]:
        raise SourceError(f"power-off default differs: core {facts['off_ms']} ms, "
                          f"gui {gui_default} ms")
    if stm is not None:
        btn = rd(stm, STM_BTN_C)
        facts["ship_ms"] = define_ms(btn, "LONG_PRESS_SHIP_MS")
        facts["short_hold_ms"] = define_ms(btn, "LONG_PRESS_SLEEP_MS")
        facts["wake_ms"] = define_ms(rd(stm, STM_BTN_H), "POWER_BTN_WAKE_HOLD_MS")
    return facts


def bundled_stm_tag(root: Path) -> str:
    """The CrossPad_STM32_r2 tag of the STM image this firmware bundles (CPFW at 0x100)."""
    head = (root / STM_BUNDLE).read_bytes()[0x100:0x106]
    if head[:4] != b"CPFW":
        raise SourceError(f"{STM_BUNDLE}: no CPFW descriptor at 0x100")
    return f"v{head[4]}.{head[5]}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=ROOT, help="platform-idf checkout")
    ap.add_argument("--stm", type=Path, help="CrossPad_STM32_r2 checkout (ship/on/0.5 s holds)")
    ap.add_argument("--stm-tag", action="store_true",
                    help="print the STM tag of the bundled stm32_fw.bin and exit")
    args = ap.parse_args(argv)
    try:
        if args.stm_tag:
            print(bundled_stm_tag(args.root))
            return 0
        facts = read_facts(args.root, args.stm)
        md = (args.root / HELP).read_text()
    except (SourceError, OSError) as e:
        print(f"help_facts: {e}", file=sys.stderr)
        return 2
    problems = check(md, facts)
    for p in problems:
        print(f"::error file={HELP}::{p}" if os.environ.get("GITHUB_ACTIONS") else p)
    if not args.stm:
        print("help_facts: no --stm checkout; ship/on/0.5 s holds checked only for order")
    if problems:
        print(f"help_facts: {len(problems)} difference(s) between {HELP} and the firmware")
        return 1
    print(f"help_facts: {HELP} matches the MODE map, kit tabs, holds and timeouts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
