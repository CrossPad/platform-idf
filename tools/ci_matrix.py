#!/usr/bin/env python3
"""The CI build matrix: every firmware kit for the board revisions asked for.

A kit is a profile -- config/profiles/*.json (tier official) and
config/profiles/community/*.json (tier community, submitted by PR). Its file
name is the kit id baked into the image (-DCROSSPAD_FW_KIT).

  ci_matrix.py [--all] [--revs v1,v2] [--github-output]
      prints {"include": [{"rev", "kit", "profile", "tier"}, ...]}

Without --all only the default kit is built (a branch push does not pay for
every kit). Without --revs only v2: nobody runs v1 boards any more, and its
cells cost a runner's disk and a release's space. A manual run can still ask
for v1. Every profile is validated either way, so a bad community PR fails
here, before anything compiles: exit 1 with one line per problem.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

import fw_icon
import fw_text

REPO = Path(__file__).resolve().parent.parent
PROFILE_DIR = REPO / "config" / "profiles"
COMMUNITY_DIR = PROFILE_DIR / "community"
REVS = ("v1", "v2")          # what --revs may name
DEFAULT_REVS = ("v2",)
DEFAULT_KIT = "full"
# CPFI kit_id is char[16]; crosspad.app reads CrossPad-<tag>-<rev>-<kit>.bin,
# so a kit must not look like a revision either.
KIT_RE = re.compile(r"^[a-z][a-z0-9_]{0,14}$")
SDKCONFIG_LINE_RE = re.compile(r"^(# )?CONFIG_[A-Z0-9_]+(=[^;\n]+| is not set)$")
REV_LIKE_RE = re.compile(r"^v\d+$")
# Device buffers (crosspad/fw/FwStore.hpp) less the NUL, and what fits a tile.
DISPLAY_NAME_CHARS, DISPLAY_NAME_BYTES = 24, 31
DESCRIPTION_CHARS, DESCRIPTION_BYTES = 160, 191
AUTHOR_CHARS, AUTHOR_BYTES = 31, 31
APP_ID_BYTES = 23
APPS_MAX = 16


def _text_field(path: Path, prof: dict, key: str, max_chars: int, max_bytes: int, required: bool,
                errors: list[str]) -> None:
    value = prof.get(key)
    if value is None:
        if required:
            errors.append(f"{path}: {key} is required")
        return
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{path}: {key} must be a non-empty string")
        return
    if len(value) > max_chars or len(value.encode()) > max_bytes:
        errors.append(f"{path}: {key} is longer than {max_chars} characters or {max_bytes} bytes")
    missing = fw_text.missing_glyphs(value)
    if missing:
        errors.append(f"{path}: {key} has characters the CrossPad cannot draw: {missing!r}")


def validate_profile(path: Path) -> list[str]:
    try:
        prof = json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        return [f"{path}: unreadable ({e})"]
    errors = []
    name = prof.get("name")
    if name != path.stem:
        errors.append(f"{path}: name {name!r} must equal the file name {path.stem!r}")
    if not isinstance(name, str) or not KIT_RE.match(name):
        errors.append(f"{path}: kit {name!r} must match {KIT_RE.pattern}")
    elif REV_LIKE_RE.match(name):
        errors.append(f"{path}: kit {name!r} looks like a board revision")
    if prof.get("platform") != "esp-idf":
        errors.append(f"{path}: platform must be esp-idf")
    if not isinstance(prof.get("apps"), dict) or not prof["apps"]:
        errors.append(f"{path}: no apps")
    community = Path(path).parent.name == "community"
    _text_field(path, prof, "display_name", DISPLAY_NAME_CHARS, DISPLAY_NAME_BYTES, True, errors)
    _text_field(path, prof, "description", DESCRIPTION_CHARS, DESCRIPTION_BYTES, True, errors)
    _text_field(path, prof, "author", AUTHOR_CHARS, AUTHOR_BYTES, community, errors)
    expected_icon = fw_icon.profile_icon(path.stem)
    if prof.get("icon") != expected_icon:
        errors.append(f"{path}: icon must be {expected_icon!r} (a 64x64 PNG next to the profile)")
    else:
        errors += fw_icon.check(Path(path).parent / expected_icon)
    # The image's own shape (CMakeLists.txt reads these before project()).
    if "standalone" in prof and (not isinstance(prof["standalone"], str) or not prof["standalone"]):
        errors.append(f"{path}: standalone must name the app the kit starts into")
    for key in ("top_bar", "sample_engine", "radio", "audio"):
        if key in prof and not isinstance(prof[key], bool):
            errors.append(f"{path}: {key} must be true or false")
    if "sdkconfig" in prof:
        lines = prof["sdkconfig"]
        if community:
            errors.append(f"{path}: sdkconfig is for official kits only")
        elif not isinstance(lines, list) or not all(isinstance(l, str) and SDKCONFIG_LINE_RE.match(l) for l in lines):
            errors.append(f"{path}: sdkconfig must list lines like CONFIG_X=value or # CONFIG_X is not set")
    apps = prof.get("apps")
    if isinstance(apps, dict):
        if len(apps) > APPS_MAX:
            errors.append(f"{path}: more than {APPS_MAX} apps")
        errors += [f"{path}: app id {a!r} is longer than {APP_ID_BYTES} bytes"
                   for a in apps if len(a.encode()) > APP_ID_BYTES]
    return errors


def _kits() -> list[dict]:
    found = []
    for tier, directory, prefix in (("official", PROFILE_DIR, ""), ("community", COMMUNITY_DIR, "community/")):
        if not directory.is_dir():
            continue
        for p in sorted(directory.glob("*.json")):
            found.append({"kit": p.stem, "profile": prefix + p.stem, "tier": tier, "path": p})
    return found


def matrix(all_kits: bool, revs: tuple[str, ...] = DEFAULT_REVS) -> tuple[dict, list[str]]:
    errors, seen, include = [], set(), []
    errors += [f"--revs: {r!r} is not one of {', '.join(REVS)}" for r in revs if r not in REVS]
    for k in _kits():
        errors += validate_profile(k["path"])
        if k["kit"] in seen:
            errors.append(f"{k['path']}: kit {k['kit']!r} defined twice")
        seen.add(k["kit"])
        if not all_kits and k["kit"] != DEFAULT_KIT:
            continue
        for rev in revs:
            include.append({"rev": rev, "kit": k["kit"], "profile": k["profile"], "tier": k["tier"]})
    if DEFAULT_KIT not in seen:
        errors.append(f"no {DEFAULT_KIT}.json in {PROFILE_DIR}")
    return {"include": include}, errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="every kit, not only the default one")
    ap.add_argument("--revs", default=",".join(DEFAULT_REVS),
                    help=f"comma-separated board revisions (default {','.join(DEFAULT_REVS)}; of {','.join(REVS)})")
    ap.add_argument("--github-output", action="store_true", help="also append matrix=<json> to $GITHUB_OUTPUT")
    a = ap.parse_args()
    revs = tuple(r.strip() for r in a.revs.split(",") if r.strip())
    m, errors = matrix(a.all, revs)
    for e in errors:
        print(f"::error::{e}", file=sys.stderr)
    if errors:
        return 1
    text = json.dumps(m, separators=(",", ":"))
    print(text)
    if a.github_output:
        with open(os.environ["GITHUB_OUTPUT"], "a") as f:
            f.write(f"matrix={text}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
