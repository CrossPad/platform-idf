#!/usr/bin/env python3
"""Version ranges as crosspad-app.json `requires` writes them, judged strictly.

The operators of the app manager's version_satisfies (crosspad-apps
crosspad_app_manager.py): >= <= > < == = ^ ~, a comma is AND, `*` is any.
Unlike the TUI, which lets an unknown version or an unreadable range pass,
anything this module cannot read is a RangeError: it gates builds and bump PRs.
Spec: docs/design/2026-10-11-deps-resolver-design.md.
"""
import re

NUM = r"(0|[1-9][0-9]*)"
VERSION = re.compile(rf"{NUM}\.{NUM}\.{NUM}", re.ASCII)
PARTIAL = re.compile(rf"{NUM}(?:\.{NUM}(?:\.{NUM})?)?", re.ASCII)
TERM = re.compile(r"(>=|<=|>|<|==|=|\^|~)?[ \t]*([^ \t]+)", re.ASCII)
MAX_SPEC = 64


class RangeError(ValueError):
    """A version or range this module will not guess about."""


def _show(text) -> str:
    return re.sub(r"[^\x20-\x7e]", "?", str(text))[:30]


def parse_version(text) -> tuple:
    if not isinstance(text, str) or not VERSION.fullmatch(text):
        raise RangeError(f"not an X.Y.Z version: {_show(text)!r}")
    return tuple(int(x) for x in text.split("."))


def fmt(v) -> str:
    return ".".join(str(x) for x in v)


def parse_range(spec) -> list:
    """[(operator, (major, minor|None, patch|None))]; [] for `*` (any version)."""
    if not isinstance(spec, str) or len(spec) > MAX_SPEC:
        raise RangeError(f"not a version range: {_show(spec)!r}")
    if spec.strip() == "*":
        return []
    terms = []
    for part in spec.split(","):
        m = TERM.fullmatch(part.strip())
        bound = PARTIAL.fullmatch(m.group(2)) if m else None
        if bound is None:
            raise RangeError(f"unreadable version range: {_show(spec)!r}")
        terms.append((m.group(1) or "==",
                      tuple(None if g is None else int(g) for g in bound.groups())))
    return terms


def _holds(have: tuple, op: str, bound: tuple) -> bool:
    want = tuple(0 if x is None else x for x in bound)
    if op == "^":
        # ^0.y.z: before 1.0 the minor is the breaking part (npm's rule).
        same = have[:2] == want[:2] if want[0] == 0 and bound[1] is not None else have[0] == want[0]
        return have >= want and same
    if op == "~":
        same = have[:2] == want[:2] if bound[1] is not None else have[0] == want[0]
        return have >= want and same
    return {">=": have >= want, "<=": have <= want, ">": have > want, "<": have < want,
            "==": have == want, "=": have == want}[op]


def satisfies(have: tuple, spec) -> bool:
    """Every term of spec holds for have. RangeError when spec cannot be read."""
    return all(_holds(have, op, bound) for op, bound in parse_range(spec))
