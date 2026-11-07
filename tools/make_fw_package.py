#!/usr/bin/env python3
"""Build a firmware package directory for /sdcard/crosspad/firmware/.

  make_fw_package.py --build build_v2 --name full-1.3.0 [--tag v1.3.0] [--tier official]
                     [--channel stable|beta] [--sign key.pem] [--profile config/profiles/full.json]
                     [--out fw_packages]

--sign writes manifest.sig: ECDSA P-256 over the exact bytes of manifest.json
(tools/fw_sign.py). The manifest already carries every file's SHA-256, so one
signature covers the package. CI signs with the FW_SIGNING_KEY secret.
--profile adds what the Firmware app shows: display_name, description, author
and apps in the manifest, and icon.bin (tools/fw_icon.py) among the hashed --
and so signed -- files.
"""
import argparse, hashlib, json, re, shutil
from pathlib import Path

import fw_icon
import fw_image_desc
import fw_sign

CHANNELS = ("stable", "beta")
TIERS = ("official", "community")
# A tag lands in the signed manifest and, on the device, in a path and a UI
# line: the device refuses anything crosspad-core fwVersionValid() refuses, so
# tag_valid() refuses it here. Its grammar is Semver::parse's.
_TAG_HEAD = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([^+]*))?(?:\+(.*))?", re.S)
# Dot-separated identifiers of [0-9A-Za-z-], none empty: a pre-release or build metadata.
_IDENTS = re.compile(r"[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*")
# A git-describe tail Semver::parse strips before it checks what is left.
_DESCRIBE = re.compile(r"(?:(.*)-)?([0-9]{1,5})-g[0-9a-f]{4,}", re.S)
_DIRTY = "-dirty"
_PART_MAX = 65535
# What a board can hold: crosspad-core kFwVersionMax (24) counts the NUL, so a
# version is at most kFwVersionMax - 1 = 23 characters, and the manifest's
# version is the image's, i.e. the tag *with* its 'v'. A longer tag would build,
# sign and publish, and every board would refuse its manifest
# (tools/tests/test_make_fw_package.py reads kFwVersionMax from core).
TAG_MAX_LEN = 23


def tag_valid(tag):
    """crosspad-core fwVersionValid() for a release tag: vMAJOR.MINOR.PATCH (each
    at most 65535), an optional -pre-release and +build of non-empty dot-separated
    [0-9A-Za-z-] identifiers, at most TAG_MAX_LEN characters. Checked against
    core's own code in tools/tests/test_make_fw_package.py."""
    m = _TAG_HEAD.fullmatch(tag)
    if len(tag) > TAG_MAX_LEN or not m or any(int(x) > _PART_MAX for x in m.group(1, 2, 3)):
        return False
    pre, build = m.group(4), m.group(5)
    if build is not None and not _IDENTS.fullmatch(build):
        return False
    if pre is None or pre == _DIRTY[1:]:
        return True
    if len(pre) > len(_DIRTY) and pre.endswith(_DIRTY):
        pre = pre[:-len(_DIRTY)]
    d = _DESCRIBE.fullmatch(pre)
    if d and int(d.group(2)) <= _PART_MAX:
        return not d.group(1) or bool(_IDENTS.fullmatch(d.group(1)))
    return bool(_IDENTS.fullmatch(pre))


def tag_channel(tag):
    """The channel a release tag stands for: beta when it has a '-' (a pre-release),
    else stable. crosspad.app and the local test server derive it the same way
    (tools/tests/fw_tag_rules.json); the board refuses a catalog channel that
    differs from the signed one."""
    return "beta" if "-" in tag else "stable"
ICON_FILE = "icon.bin"


def _entry(path):
    data = path.read_bytes()
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _profile_meta(profile):
    profile = Path(profile)
    if not profile.is_file():
        raise SystemExit(f"{profile}: no such profile")
    prof = json.loads(profile.read_text())
    if prof.get("icon") != fw_icon.profile_icon(profile.stem):
        raise SystemExit(f"{profile}: icon must be {fw_icon.profile_icon(profile.stem)!r}")
    meta = {"display_name": prof["display_name"], "description": prof["description"],
            "author": prof.get("author", ""), "apps": sorted(prof["apps"])}
    try:
        return meta, fw_icon.convert((profile.parent / prof["icon"]).read_bytes())
    except (OSError, fw_icon.IconError) as e:
        raise SystemExit(f"{profile}: icon: {e}")


def build(build_dir, out_root, name, *, tag=None, channel=None, tier="official", sign=None, profile=None):
    build_dir, out_root = Path(build_dir), Path(out_root)
    if tag is not None and not tag_valid(tag):
        raise SystemExit(f"tag {tag!r}: vMAJOR.MINOR.PATCH[-pre-release][+build] of non-empty dot-separated "
                         f"[0-9A-Za-z-] identifiers, at most {TAG_MAX_LEN} characters")
    channel = channel or (tag_channel(tag) if tag else "stable")
    if channel not in CHANNELS:
        raise SystemExit(f"channel {channel!r}: one of {', '.join(CHANNELS)}")
    if tier not in TIERS:
        raise SystemExit(f"tier {tier!r}: one of {', '.join(TIERS)}")
    image = build_dir / "CrossPad.bin"
    desc = fw_image_desc.read(image)
    if not desc or desc["board_rev"] is None:
        raise SystemExit(f"{image}: no CPFI description — rebuild the firmware")
    assets_id = (build_dir / "assets_id.txt").read_text().strip()
    if assets_id != desc["assets_id"]:
        raise SystemExit("assets.bin in this build dir is not the one the image was built with")
    out = out_root / name
    out.mkdir(parents=True, exist_ok=True)
    files = {}
    for fname in ("CrossPad.bin", "assets.bin"):
        shutil.copyfile(build_dir / fname, out / fname)
        files[fname] = _entry(out / fname)
    meta = {}
    if profile:
        meta, icon = _profile_meta(profile)
        (out / ICON_FILE).write_bytes(icon)
        files[ICON_FILE] = _entry(out / ICON_FILE)
    else:
        (out / ICON_FILE).unlink(missing_ok=True)   # a stale icon would not be in the manifest
    manifest = {"name": name, "kit": desc["kit_id"], "version": desc["version"],
                "board_rev": desc["board_rev"], "stm_proto_min": desc["stm_proto_min"],
                "assets_id": assets_id, "channel": channel, "tier": tier, "tag": tag or "",
                **meta, "files": files}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    sig = out / "manifest.sig"
    sig.unlink(missing_ok=True)   # a stale signature over an older manifest would only mislead, also if signing fails
    if sign:
        sig.write_bytes(fw_sign.sign(out / "manifest.json", Path(sign)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True, help="build directory, e.g. build_v2")
    ap.add_argument("--name", required=True, help="package name (directory on the card)")
    ap.add_argument("--out", default="fw_packages")
    ap.add_argument("--tag", default=None, help="the release tag this image was built for")
    ap.add_argument("--channel", choices=CHANNELS, default=None, help="default: beta when the tag has a '-'")
    ap.add_argument("--tier", choices=TIERS, default="official")
    ap.add_argument("--sign", default=None, help="ECDSA P-256 private key (PEM) to sign manifest.json with")
    ap.add_argument("--profile", default=None, help="config/profiles/<kit>.json: display fields and icon")
    a = ap.parse_args()
    print(build(a.build, a.out, a.name, tag=a.tag, channel=a.channel, tier=a.tier, sign=a.sign,
                profile=a.profile))


if __name__ == "__main__":
    main()
