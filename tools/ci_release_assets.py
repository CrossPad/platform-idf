#!/usr/bin/env python3
"""Turn the build job's artifacts into a release's files.

  ci_release_assets.py --tag v1.3.0 --dist dist --out out --matrix '<ci_matrix json>' --key key.pem

dist/build-<rev>-<kit>/ is one matrix cell (CrossPad.bin, assets.bin,
assets_id.txt, components.txt, bootloader/, partition_table/, CrossPad.elf). Every cell
becomes a signed package, published flat:
  CrossPad-<tag>-<rev>-<kit>.bin, -assets.bin, -icon.bin, -manifest.json,
  -manifest.sig
The default kit's cells also keep the names the companion and CP Tools read:
  CrossPad-<tag>-<rev>.bin, -assets.bin, -bootloader.bin, -partition-table.bin, .elf
CrossPad-<tag>-kits.json lists the kits with their tier and display fields for
crosspad.app's /api/device/catalog. CrossPad-<tag>-components.json is the commit
of every submodule the default kit was built from: its cells' components.txt, the
build job's `git submodule status` after the kit's profile was applied -- a
branch-tracked app is past the tag's gitlinks by then. Every signature is checked
against main/fw_sign_pubkey.h first: a release whose firmware could not verify its
own packages fails here.
"""
import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

import ci_matrix
import fw_sign
import make_fw_package

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "main" / "fw_sign_pubkey.h"
PROFILE_DIR = REPO / "config" / "profiles"
DEFAULT_KIT = "full"
PACKAGE_FILES = (("CrossPad.bin", "{b}.bin"), ("assets.bin", "{b}-assets.bin"),
                 ("manifest.json", "{b}-manifest.json"), ("manifest.sig", "{b}-manifest.sig"))
ICON_FILE = (make_fw_package.ICON_FILE, "{b}-icon.bin")
LEGACY_FILES = (("CrossPad.bin", "{p}.bin"), ("assets.bin", "{p}-assets.bin"),
                ("bootloader/bootloader.bin", "{p}-bootloader.bin"),
                ("partition_table/partition-table.bin", "{p}-partition-table.bin"),
                ("CrossPad.elf", "{p}.elf"))
META_KEYS = ("display_name", "description", "author", "apps")


def _validate(tag: str, cells: list[dict]) -> None:
    # Everything below becomes a path or a file name, and the tag lands in a
    # signed manifest: refuse a bad one before any file is touched.
    if not make_fw_package.tag_valid(tag):
        raise SystemExit(f"tag {tag!r}: vMAJOR.MINOR.PATCH[-pre-release][+build] of non-empty dot-separated "
                         f"[0-9A-Za-z-] identifiers, at most {make_fw_package.TAG_MAX_LEN} characters")
    for cell in cells:
        if not ci_matrix.REV_LIKE_RE.fullmatch(cell["rev"]):
            raise SystemExit(f"rev {cell['rev']!r}: must be v<digits>")
        if not ci_matrix.KIT_RE.match(cell["kit"]) or ci_matrix.REV_LIKE_RE.match(cell["kit"]):
            raise SystemExit(f"kit {cell['kit']!r}: must match {ci_matrix.KIT_RE.pattern} and not look like a revision")


def _parse_components(text: str) -> dict:
    # `git submodule status`: "[ +-U]<sha> <path> (<describe>)". The describe
    # depends on which refs a runner has fetched, never on what was built.
    comps = {}
    for line in text.splitlines():
        parts = line.strip().lstrip("-+U").split()
        if len(parts) >= 2:
            comps[parts[1]] = parts[0]
    return comps


def _components(tag: str, cells: list[Path]) -> dict:
    lists = []
    for src in cells:
        f = src / "components.txt"
        if not f.is_file():
            raise SystemExit(f"{f}: missing -- the build job records the components it built for a tag")
        lists.append(_parse_components(f.read_text()))
    # CP Tools flashes the release image only when a checkout matches this
    # list exactly: one list must describe every default-kit image.
    if any(comps != lists[0] for comps in lists):
        raise SystemExit(f"the {DEFAULT_KIT} kit's cells were built from different component commits")
    return {"tag": tag, "components": lists[0]}


def build_release(tag: str, dist: Path, out: Path, cells: list[dict], key: Path, header: Path = HEADER,
                  profiles: Path = PROFILE_DIR) -> dict:
    dist, out, profiles = Path(dist), Path(out), Path(profiles)
    _validate(tag, cells)
    version = tag.removeprefix("v")
    kits: dict[str, dict] = {}
    default_cells: list[Path] = []
    # Staged, then moved: a later cell that fails must not leave the earlier
    # cells' files in `out` for a release step to pick up.
    with tempfile.TemporaryDirectory() as tmp:
        staged = Path(tmp) / "staged"
        staged.mkdir()
        for cell in cells:
            rev, kit, tier = cell["rev"], cell["kit"], cell["tier"]
            src = dist / f"build-{rev}-{kit}"
            if not src.is_dir():
                raise SystemExit(f"{src}: missing -- the build job did not upload this cell")
            pkg = make_fw_package.build(src, Path(tmp) / rev, f"{kit}-{version}", tag=tag, tier=tier, sign=key,
                                        profile=profiles / f"{cell['profile']}.json")
            manifest = json.loads((pkg / "manifest.json").read_text())
            # A board refuses a manifest of another revision, so an image in the
            # wrong cell would publish a package no board of that cell can install.
            if manifest["board_rev"] != int(rev.removeprefix("v")):
                raise SystemExit(f"{src}: the image is for board v{manifest['board_rev']}, the cell is {rev!r}")
            if manifest["kit"] != kit:
                raise SystemExit(f"{src}: the image carries kit {manifest['kit']!r}, the cell is {kit!r}")
            # PROJECT_VER=<tag> pins it; anything else (a "-dirty" describe
            # string) would put a version the catalog does not name on the card.
            if manifest["version"].removeprefix("v") != version:
                raise SystemExit(f"{kit} {rev}: image version {manifest['version']!r} is not the tag {tag!r}")
            if not fw_sign.verify_with_header(header, pkg / "manifest.json", (pkg / "manifest.sig").read_bytes()):
                raise SystemExit(f"{kit} {rev}: no key in {header.name} (main/fw_sign_pubkey.h) verifies this "
                                 "release's signature -- the firmware could not install its own packages")
            base = f"CrossPad-{tag}-{rev}-{kit}"
            for name, pattern in (*PACKAGE_FILES, ICON_FILE):
                shutil.copyfile(pkg / name, staged / pattern.format(b=base))
            if kit == DEFAULT_KIT:
                default_cells.append(src)
                for name, pattern in LEGACY_FILES:
                    shutil.copyfile(src / name, staged / pattern.format(p=f"CrossPad-{tag}-{rev}"))
            entry = kits.setdefault(kit, {"kit": kit, "tier": tier, "revs": [],
                                          **{k: manifest[k] for k in META_KEYS}})
            entry["revs"].append(rev)
        doc = {"v": 1, "tag": tag, "kits": sorted(kits.values(), key=lambda k: k["kit"])}
        (staged / f"CrossPad-{tag}-kits.json").write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
        if default_cells:
            (staged / f"CrossPad-{tag}-components.json").write_text(
                json.dumps(_components(tag, default_cells), indent=2) + "\n")
        out.mkdir(parents=True, exist_ok=True)
        for f in staged.iterdir():
            shutil.move(f, out / f.name)
    return doc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dist", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--matrix", required=True, help="tools/ci_matrix.py output")
    ap.add_argument("--key", required=True, help="the FW_SIGNING_KEY secret, written to a file")
    ap.add_argument("--header", default=str(HEADER))
    ap.add_argument("--profiles", default=str(PROFILE_DIR))
    a = ap.parse_args()
    doc = build_release(a.tag, Path(a.dist), Path(a.out), json.loads(a.matrix)["include"], Path(a.key),
                        Path(a.header), Path(a.profiles))
    print(json.dumps(doc, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
