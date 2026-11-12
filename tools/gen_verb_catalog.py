#!/usr/bin/env python3
"""The CDC verb catalog, read from the firmware's own dispatch table.

k_verbs in main/hil_control.cpp is the order a line is offered to the verbs;
each verb_<name>() in main/hil_verbs_<domain>.cpp starts with the condition
that takes the line. This prints one row per verb, in dispatch order:

  order  function  file  match  literal  companion

  order      position in k_verbs; "9.3" is the third verb the function of row
             9 matches (a handler in another file, or a function that takes
             several verbs), in the order it tries them
  match      exact (strcmp), prefix (strncmp), scan (sscanf format), word (the
             whole first word), handler (a function in another file)
  literal    the text matched, as written in the source
  companion  yes when the companion allow-list (BLE, SysEx, the remote
             session) names the verb, some when it names only some forms of it
             ("ASONG PLAY"), no otherwise

Not listed: what cdc_rx.cpp offers a line to before this table -- the
bootloader request, ota_cdc.cpp (OTA_*), file_transfer.cpp (FS_*, ASSETS_*) and
audio_route_control.cpp (AUDIO_*) -- and the forms a verb tells apart by its
arguments (TWIN HELLO, ASONG PLAY).

  gen_verb_catalog.py            print the catalog
  gen_verb_catalog.py --write    write docs/dev/verbs.tsv
  gen_verb_catalog.py --check    exit 1 when docs/dev/verbs.tsv is out of date
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN = REPO / "main"
OUT = REPO / "docs" / "dev" / "verbs.tsv"
HEADER = "order\tfunction\tfile\tmatch\tliteral\tcompanion"

COND_RE = re.compile(
    r'\b(?:(strcmp)\(cmd, "((?:[^"\\]|\\.)*)"\)\s*==\s*0'
    r'|(strncmp)\(cmd, "((?:[^"\\]|\\.)*)",'
    r'|(sscanf)\(cmd(?: \+ \d+)?, "((?:[^"\\]|\\.)*)")')
DELEGATE_RE = re.compile(r"\bif \((?:\w+ && )?(\w+)\(cmd, cdc_reply\)\)")
SUB_RE = re.compile(
    r'\b(?:(args_of|args_of_trimmed|is_verb)\(cmd, "((?:[^"\\]|\\.)*)"\)'
    r'|(strcmp)\(cmd, "((?:[^"\\]|\\.)*)"\)\s*==\s*0'
    r'|(strncmp)\(cmd, "((?:[^"\\]|\\.)*)",'
    r'|(sscanf)\(cmd, "((?:[^"\\]|\\.)*)")')


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def table_order(control: str) -> list[str]:
    m = re.search(r"k_verbs\[\]\)\(VerbLine &\) = \{(.*?)\n\};", control, re.S)
    if not m:
        raise SystemExit("k_verbs not found in main/hil_control.cpp")
    return re.findall(r"\bverb_\w+", strip_comments(m.group(1)))


def bodies() -> dict[str, tuple[str, str]]:
    """verb function name -> (file name, body text without comments)."""
    out: dict[str, tuple[str, str]] = {}
    for path in sorted(MAIN.glob("hil_*.cpp")):
        text = path.read_text()
        for m in re.finditer(r"^(?:static )?bool (verb_\w+)\(VerbLine &l\)\n\{", text, re.M):
            end = text.find("\n}\n", m.end())
            out[m.group(1)] = (path.name, strip_comments(text[m.end():end]))
    return out


def companion_names(control: str) -> set[str]:
    m = re.search(r"const bool allowed = (.*?);\n", control, re.S)
    if not m:
        raise SystemExit("the companion allow-list was not found in main/hil_control.cpp")
    return {lit.strip() for lit in re.findall(r'\(command, "([^"]+)"', strip_comments(m.group(1)))}


def first_word(literal: str) -> str:
    return re.split(r"[ %\\]", literal, maxsplit=1)[0]


def companion_of(literal: str, allowed: set[str]) -> str:
    word = first_word(literal)
    if not word:
        return "no"
    if word in allowed:
        return "yes"
    if any(a.startswith(word + " ") or (a.endswith("_") and word.startswith(a)) for a in allowed):
        return "some"
    return "no"


def handler_verbs(fn: str) -> list[tuple[str, str, str]]:
    """(file, match, literal) for each verb a handler in another file matches."""
    for path in sorted(MAIN.glob("*.cpp")):
        text = path.read_text()
        m = re.search(r"^(?:extern \"C\" )?bool " + fn + r"\(const char ?\*\s*cmd\b[^;{]*\{", text, re.M)
        if not m:
            continue
        end = text.find("\n}\n", m.end())
        body = strip_comments(text[m.end():end])
        out = []
        for s in SUB_RE.finditer(body):
            if s.group(1):
                out.append((path.name, "exact" if s.group(1) == "is_verb" else "word", s.group(2)))
            elif s.group(3):
                out.append((path.name, "exact", s.group(4)))
            elif s.group(5):
                out.append((path.name, "prefix", s.group(6)))
            else:
                out.append((path.name, "scan", s.group(8)))
        seen, uniq = set(), []
        for row in out:
            if first_word(row[2]) not in seen:
                seen.add(first_word(row[2])); uniq.append(row)
        return uniq
    return []


def rows() -> list[str]:
    control = (MAIN / "hil_control.cpp").read_text()
    funcs = bodies()
    allowed = companion_names(control)
    out = []
    for n, name in enumerate(table_order(control), 1):
        if name not in funcs:
            raise SystemExit(f"{name} is in k_verbs and defined nowhere")
        file, body = funcs[name]
        m = COND_RE.search(body)
        d = DELEGATE_RE.search(body)
        if m and (not d or m.start() < d.start()):
            kind = {"strcmp": "exact", "strncmp": "prefix", "sscanf": "scan"}[m.group(1) or m.group(3) or m.group(5)]
            literal = m.group(2) or m.group(4) or m.group(6) or ""
        elif d:
            kind, literal = "handler", d.group(1) + "()"
        else:
            kind, literal = "handler", ""
        out.append(f"{n}\t{name}\t{file}\t{kind}\t{literal}\t{companion_of(literal, allowed)}")
        if kind != "handler":
            # a function that takes several verbs: one row for each further one
            seen, k = {first_word(literal)}, 0
            for extra in COND_RE.finditer(body, m.end()):
                ek = {"strcmp": "exact", "strncmp": "prefix", "sscanf": "scan"}[extra.group(1) or extra.group(3) or extra.group(5)]
                el = extra.group(2) or extra.group(4) or extra.group(6) or ""
                if first_word(el) and first_word(el) not in seen:
                    seen.add(first_word(el)); k += 1
                    out.append(f"{n}.{k}\t{name}\t{file}\t{ek}\t{el}\t{companion_of(el, allowed)}")
        if kind == "handler" and d:
            for k, (sub_file, sub_kind, sub_literal) in enumerate(handler_verbs(d.group(1)), 1):
                out.append(f"{n}.{k}\t{d.group(1)}\t{sub_file}\t{sub_kind}\t{sub_literal}\t{companion_of(sub_literal, allowed)}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    args = ap.parse_args()
    text = HEADER + "\n" + "\n".join(rows()) + "\n"
    if args.write:
        OUT.write_text(text)
        print(f"{OUT.relative_to(REPO)}: {text.count(chr(10)) - 1} verbs")
        return 0
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"{OUT.relative_to(REPO)} is out of date: run python3 tools/gen_verb_catalog.py --write")
            return 1
        print("verb catalog is up to date")
        return 0
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
