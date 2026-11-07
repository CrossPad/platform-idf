#!/usr/bin/env python3
"""The next beta tag: one -rcN above the highest release tag in the repo.

  highest vX.Y.Z      -> vX.Y.(Z+1)-rc1
  highest vX.Y.Z-rcN  -> vX.Y.Z-rc(N+1)
  highest vX.Y.Z-<l>  -> vX.Y.Z-rc1 if that sorts above it, else a human tags
Never a stable tag; at most 23 characters; the device's order (fw_semver).
"""
import subprocess
import sys

import fw_semver


class NoCandidate(Exception):
    pass


def next_rc(tag_names) -> str:
    tags = fw_semver.sort_tags(tag_names)
    if not tags:
        raise NoCandidate("no release tag to continue from")
    h = tags[-1]
    if h.stable:
        cand = f"v{h.major}.{h.minor}.{h.patch + 1}-rc1"
    else:
        m = fw_semver.RC_PRE.fullmatch(h.pre)
        cand = f"v{h.major}.{h.minor}.{h.patch}-rc{int(m[1]) + 1 if m else 1}"
    t = fw_semver.parse(cand)
    if t is None:
        raise NoCandidate(f"{cand}: not a valid tag (longer than {fw_semver.TAG_MAX_LEN}?)")
    if fw_semver.key(t) <= fw_semver.key(h):
        raise NoCandidate(f"{cand} does not sort above {h.name}; a human tags the next one")
    return cand


def main() -> int:
    names = subprocess.run(["git", "tag", "-l", "v*"], check=True, capture_output=True, text=True).stdout.split()
    try:
        print(next_rc(names))
    except NoCandidate as e:
        print(f"::error::{e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
