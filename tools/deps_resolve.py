#!/usr/bin/env python3
"""The firmware's apps, crosspad-core and crosspad-gui as one consistent set.

Apps declare ranges in crosspad-app.json `requires`; gui declares the core it
needs in library.json `dependencies`. solve() is the planner app_bump.py
reconcile uses: newest apps, the lowest core/gui that satisfies them, MAJOR
never moves. check reads the checkout (CI, after `profile apply`, offline).
Spec: docs/design/2026-10-11-deps-resolver-design.md.

  deps_resolve.py check --profile <name> [--root DIR]
"""
import argparse
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple

import semver_range as sr

CORE, GUI = "crosspad-core", "crosspad-gui"
LIBS = {CORE: "components/crosspad-core", GUI: "components/crosspad-gui"}
SHORT = {CORE: "core", GUI: "gui"}
HEADERS = {CORE: ("include/crosspad/CrosspadCoreVersion.hpp", "CROSSPAD_CORE_VERSION"),
           GUI: ("include/crosspad-gui/CrosspadGuiVersion.hpp", "CROSSPAD_GUI_VERSION")}
PROFILE_NAME = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*", re.ASCII)
REPO = Path(__file__).resolve().parent.parent


class Release(NamedTuple):
    version: tuple
    commit: str          # "" when read from the checkout
    tag: str | None
    requires: dict       # crosspad-core / crosspad-gui -> range text


class Options(NamedTuple):
    pinned: Release
    newer: list          # Release, ascending, every one above pinned


class Solution(NamedTuple):
    picks: dict          # app id or library name -> Release
    held: list           # "<app> <newest>: <unmet range>", one per app left below its newest


class Unsatisfiable(Exception):
    """The pinned set itself breaks a range: nothing to plan from."""


def header_version(text: str, prefix: str) -> tuple:
    out = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"^[ \t]*#define[ \t]+{prefix}_{part}[ \t]+(0|[1-9][0-9]*)[ \t]*$", text, re.M | re.ASCII)
        if m is None:
            raise sr.RangeError(f"{prefix}_{part} not found")
        out.append(int(m.group(1)))
    return tuple(out)


def manifest_requires(doc) -> dict:
    """crosspad-app.json `requires`, validated: only core/gui, every range readable."""
    req = doc.get("requires", {}) if isinstance(doc, dict) else None
    if not isinstance(req, dict):
        raise sr.RangeError("requires must be an object of crosspad-core / crosspad-gui ranges")
    for dep, spec in req.items():
        if dep not in LIBS:
            raise sr.RangeError(f"unknown dependency {sr._show(dep)!r}")
        sr.parse_range(spec)
    return dict(req)


def gui_requires(doc) -> dict:
    """crosspad-gui library.json dependencies.crosspad-core: the core range gui was built for."""
    deps = doc.get("dependencies") if isinstance(doc, dict) else None
    spec = deps.get(CORE) if isinstance(deps, dict) else None
    if spec is None:
        raise sr.RangeError("crosspad-gui library.json has no dependencies.crosspad-core")
    sr.parse_range(spec)
    return {CORE: spec}


def unmet(requires: dict, have: dict) -> list:
    return [f"needs {dep} {spec}, has {sr.fmt(have[dep])}" for dep, spec in sorted(requires.items())
            if not sr.satisfies(have[dep], spec)]


def _lib_ladder(o: Options) -> list:
    return [o.pinned] + [r for r in o.newer if r.version[0] == o.pinned.version[0]]


def _newest(o: Options) -> Release:
    return o.newer[-1] if o.newer else o.pinned


def solve(apps: dict, libs: dict) -> Solution:
    best = None
    for core in _lib_ladder(libs[CORE]):
        for gui in _lib_ladder(libs[GUI]):
            if unmet(gui.requires, {CORE: core.version}):
                continue
            have = {CORE: core.version, GUI: gui.version}
            picks = {}
            for app_id, o in sorted(apps.items()):
                fit = next((r for r in reversed([o.pinned] + o.newer) if not unmet(r.requires, have)), None)
                if fit is None:
                    break
                picks[app_id] = fit
            else:
                score = sum(1 for app_id, o in apps.items() if picks[app_id] == _newest(o))
                if best is None or score > best[0]:   # strict: a tie keeps the lower pair
                    best = (score, {**picks, CORE: core, GUI: gui})
    if best is None:
        have = {CORE: libs[CORE].pinned.version, GUI: libs[GUI].pinned.version}
        why = [f"{GUI} {sr.fmt(libs[GUI].pinned.version)}: {u}" for u in unmet(libs[GUI].pinned.requires, have)]
        why += [f"{a} {sr.fmt(o.pinned.version)}: {u}" for a, o in sorted(apps.items())
                for u in unmet(o.pinned.requires, have)]
        raise Unsatisfiable("; ".join(why) or "no core/gui pair satisfies the pinned set")
    picks = best[1]
    have = {CORE: picks[CORE].version, GUI: picks[GUI].version}
    held = [f"{a} {sr.fmt(_newest(o).version)}: " + "; ".join(unmet(_newest(o).requires, have))
            for a, o in sorted(apps.items()) if picks[a] != _newest(o)]
    return Solution(picks, held)


def read_tree(root: Path, app_ids) -> tuple:
    """(apps, libs) as Release from the checkout: name -> Release."""
    libs = {}
    for lib, path in LIBS.items():
        rel, prefix = HEADERS[lib]
        version = header_version((root / path / rel).read_text(), prefix)
        req = gui_requires(json.loads((root / path / "library.json").read_text())) if lib == GUI else {}
        libs[lib] = Release(version, "", None, req)
    apps = {}
    for app_id in app_ids:
        try:
            doc = json.loads((root / "components" / f"crosspad-{app_id}" / "crosspad-app.json").read_text())
            apps[app_id] = Release(sr.parse_version(doc.get("version")), "", None, manifest_requires(doc))
        except (OSError, ValueError) as e:
            raise ValueError(f"{app_id}: {e}") from e
    return apps, libs


def check_tree(root: Path, app_ids) -> list:
    try:
        apps, libs = read_tree(root, app_ids)
    except (OSError, ValueError) as e:
        return [f"cannot read the pinned versions: {e}"]
    have = {lib: r.version for lib, r in libs.items()}
    problems = [f"{GUI} {sr.fmt(have[GUI])}: {u}" for u in unmet(libs[GUI].requires, have)]
    problems += [f"{a} {sr.fmt(r.version)}: {u}" for a, r in sorted(apps.items()) for u in unmet(r.requires, have)]
    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--profile", required=True)
    c.add_argument("--root", default=str(REPO))
    ns = ap.parse_args(argv)
    root = Path(ns.root)
    if not PROFILE_NAME.fullmatch(ns.profile):
        print(f"::error::not a profile name: {sr._show(ns.profile)!r}")
        return 1
    try:
        app_ids = sorted(json.loads((root / "config" / "profiles" / f"{ns.profile}.json").read_text())["apps"])
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f"::error::{ns.profile}: cannot read the profile: {e}")
        return 1
    problems = check_tree(root, app_ids)
    for p in problems:
        print(f"::error::{ns.profile}: " + re.sub(r"[\x00-\x1f\x7f]", "?", p))
    if not problems:
        _, libs = read_tree(root, app_ids)
        print(f"{ns.profile}: {len(app_ids)} app(s), {CORE} {sr.fmt(libs[CORE].version)}, "
              f"{GUI} {sr.fmt(libs[GUI].version)}: every range holds")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
