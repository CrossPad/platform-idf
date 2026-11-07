#!/usr/bin/env python3
"""Release tags as a board orders them (crosspad-core src/fw/Semver.cpp).

SemVer precedence with the board's one departure: a trailing number in a
pre-release identifier compares by value (rc9 < rc10). crosspad-hil
fw_server.version_key is the same order. Build metadata (+x) is not a release
tag here; make_fw_package.tag_valid() stays the full validity check for
signing.

  fw_semver.py check <tag>     exit 0 if <tag> is a release tag, else 1
"""
import re
import sys
from typing import NamedTuple

TAG_MAX_LEN = 23
_PART_MAX = 65535
_IDENT = r"[0-9A-Za-z][0-9A-Za-z-]*"      # never empty, never starting with '-'
_TAG = re.compile(rf"v([0-9]+)\.([0-9]+)\.([0-9]+)(?:-({_IDENT}(?:\.{_IDENT})*))?")
RC_TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+-rc[0-9]+")
RC_PRE = re.compile(r"rc([0-9]+)")


class Tag(NamedTuple):
    name: str
    major: int
    minor: int
    patch: int
    pre: str

    @property
    def stable(self) -> bool:
        return not self.pre

    @property
    def line(self) -> tuple[int, int]:
        return (self.major, self.minor)


def parse(name: str) -> Tag | None:
    m = _TAG.fullmatch(name)
    if not m or len(name) > TAG_MAX_LEN or any(int(x) > _PART_MAX for x in m.group(1, 2, 3)):
        return None
    return Tag(name, int(m[1]), int(m[2]), int(m[3]), m[4] or "")


def _ident_key(ident: str) -> tuple:
    if ident.isdigit():
        return (0, "", int(ident))
    m = re.fullmatch(r"(.*?)([0-9]*)", ident)
    return (1, m[1], int(m[2] or 0))


def key(tag: Tag) -> tuple:
    nums = (tag.major, tag.minor, tag.patch)
    if tag.stable:
        return (nums, 1, ())
    return (nums, 0, tuple(_ident_key(i) for i in tag.pre.split(".")))


def sort_tags(names) -> list[Tag]:
    return sorted((t for t in map(parse, names) if t is not None), key=key)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2 or argv[0] != "check":
        print("usage: fw_semver.py check <tag>")
        return 2
    if parse(argv[1]) is None:
        print(f"::error::{argv[1]}: not a release tag (vX.Y.Z[-pre], at most {TAG_MAX_LEN} characters)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
