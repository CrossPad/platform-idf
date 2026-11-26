#!/usr/bin/env python3
"""Find duplicate #include lines in a tree -- the ones that are certainly dead.

Why a script and not a grep: `grep -c` finds every repeated header, and the
repeated ones that live inside two different `#if` branches are not repeats at
all -- one of each branch is needed. So the checker tracks the conditional
context of every include (the stack of `#if*`/`#else`/`#elif` a line sits in) and
splits the answer into two lists:

    definite   the same header, the same context twice  -> the second is dead
    possible   the same header, different contexts      -> a person decides

A definite duplicate is not a style thing in this tree: `main/midiCallbacks.cpp`
carried `#include <atomic>` twice, once at line 43 and once at line 163, and that
was worth a PR (CrossPad/platform-idf #35) -- the file's includes had grown in
several blocks as code was moved around it, and the repeat is where the drift
shows. `main/hil_control.cpp` has the same shape twice more (`esp_attr.h`,
`CrosspadPlatform.hpp`) and `main/sample_player_port.cpp` grew a late include
pair at line 1286 -- which is what a checker like this is for: cheap to run,
and it says *where* to look instead of asking someone to read a 5 800-line file.

Run it over the whole tree, or one file:

    python3 tools/include_check.py                  # the repo, defaults below
    python3 tools/include_check.py main/hil_control.cpp
    python3 tools/include_check.py --json           # for CI

Exit code: 0 clean, 1 definite duplicates (possible ones never fail the run),
2 the path is not a tree or a file.

It does not remove anything. Deleting an include is a judgement call -- a header
can be relied on transitively today and needed directly tomorrow, and this repo's
convention is to keep what a file actually uses. What the checker buys is the
list, and the certainty that a listed line is redundant rather than conditional.
"""

import argparse
import fnmatch
import json
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]')
COND_OPEN_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b(.*)$')
COND_MID_RE = re.compile(r'^\s*#\s*(else|elif)\b(.*)$')
COND_END_RE = re.compile(r'^\s*#\s*endif\b')

# Vendored or generated trees: repeats inside them belong to their upstream.
DEFAULT_EXCLUDES = (
    "build", "build_*/", ".git", "managed_components", "docs",
    "components/crosspad-core", "components/crosspad-gui",
    "components/arduino_midi_library", "main/app", ".crosspad", "recordings",
    "factory", "tools/webflasher",
)
SOURCE_EXT = (".c", ".cpp", ".cxx", ".cc", ".h", ".hpp", ".hxx")


def strip_comment(line):
    """Drop a trailing // comment so `#include <x>  // for y` still parses."""
    idx = line.find("//")
    return line[:idx] if idx >= 0 else line


def context_label(frame_stack):
    """A stable string for the conditional position of a line."""
    return "|".join(frame_stack) if frame_stack else "-"


def scan_text(text):
    """-> (definite, possible): lists of dicts about repeated includes."""
    seen = {}          # header -> list of (line_no, context)
    frames = []        # open conditional frames
    for no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        opened = COND_OPEN_RE.match(line)
        if opened:
            frames.append(opened.group(1) + " " + opened.group(2).strip())
            continue
        mid = COND_MID_RE.match(line)
        if mid and frames:
            # The other arm of the same conditional: a different place to be, so
            # an include here is not a repeat of one in the first arm.
            frames[-1] = frames[-1] + " / " + mid.group(1).strip() + " " + mid.group(2).strip()
            continue
        if COND_END_RE.match(line):
            if frames:
                frames.pop()
            continue
        inc = INCLUDE_RE.match(line)
        if not inc:
            continue
        header = inc.group(2).strip()
        seen.setdefault(header, []).append((no, context_label(frames)))

    definite, possible = [], []
    for header, places in sorted(seen.items()):
        if len(places) < 2:
            continue
        first_line, first_ctx = places[0]
        for line_no, ctx in places[1:]:
            entry = {"header": header, "first_line": first_line, "line": line_no,
                     "context": ctx, "first_context": first_ctx}
            (definite if ctx == first_ctx else possible).append(entry)
    return definite, possible


def is_excluded(rel_parts, excludes):
    """A path is excluded when any of its segments matches a pattern, or when
    the path starts with a slash-separated pattern -- so `build_*/` catches
    `build_v2_pc/...` at any depth and `components/crosspad-core` catches that
    submodule's own tree, not only a directory called that at the root."""
    for pat in excludes:
        base = pat.rstrip("/")
        for seg in rel_parts:
            if fnmatch.fnmatchcase(seg, base) or fnmatch.fnmatchcase(seg, pat):
                return True
        for k in range(1, len(rel_parts) + 1):
            joined = "/".join(rel_parts[:k])
            if "/" in base and fnmatch.fnmatchcase(joined, base):
                return True
    return False


def submodule_paths(root):
    """Paths registered as git submodules in `.gitmodules`.

    Apps and the two shared libraries are separate repos: their include hygiene
    is their own branch to open, and a report that mixes them in makes the
    firmware's own list unreadable.
    """
    if not os.path.isdir(root):
        return []
    gm = os.path.join(root, ".gitmodules")
    if not os.path.exists(gm):
        return []
    found = []
    with open(gm, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            s = line.strip()
            if s.startswith("path") and "=" in s:
                found.append(s.split("=", 1)[1].strip())
    return found


def walk(root, excludes):
    if os.path.isfile(root):
        yield root
        return
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        parts = [] if rel_dir == "." else rel_dir.replace(os.sep, "/").split("/")
        dirnames[:] = [d for d in dirnames if not is_excluded(parts + [d], excludes)]
        for name in filenames:
            if not name.endswith(SOURCE_EXT):
                continue
            if is_excluded(parts + [name], excludes):
                continue
            yield os.path.join(dirpath, name)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path", nargs="?", default=".", help="a tree or one file")
    ap.add_argument("--exclude", action="append", default=None,
                    help="repeatable; replaces the default vendored list")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--show-possible", action="store_true",
                    help="list the conditional ones too (default: count only)")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.path)
    if not os.path.exists(root):
        print("not found: " + root, file=sys.stderr)
        return 2
    excludes = (args.exclude if args.exclude is not None else list(DEFAULT_EXCLUDES))
    excludes = list(excludes) + submodule_paths(root)

    definite, possible, files_scanned = [], [], 0
    for path in walk(root, excludes):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            print("unreadable %s: %s" % (path, exc), file=sys.stderr)
            continue
        files_scanned += 1
        rel = os.path.relpath(path, root if os.path.isdir(root) else os.path.dirname(root))
        d, p = scan_text(text)
        for entry in d:
            entry["file"] = rel.replace(os.sep, "/")
        for entry in p:
            entry["file"] = rel.replace(os.sep, "/")
        definite += d
        possible += p

    if args.json:
        print(json.dumps({"files": files_scanned, "definite": definite,
                          "possible": possible}, indent=2))
    else:
        print("scanned %d C/C++ files under %s" % (files_scanned, root))
        print("definite duplicates (same header, same conditional context): %d" % len(definite))
        for e in definite:
            print("  %s:%d repeats %s (first at :%d)" % (e["file"], e["line"], e["header"], e["first_line"]))
        print("possible duplicates (different #if arms -- a person decides): %d" % len(possible))
        if args.show_possible:
            for e in possible:
                print("  %s:%d %s  [%s] vs :%d [%s]"
                      % (e["file"], e["line"], e["header"], e["context"],
                         e["first_line"], e["first_context"]))
        if definite:
            print("a listed line is redundant, not conditional; removing it is still a "
                  "judgement call (see the module docstring)")
    return 1 if definite else 0


if __name__ == "__main__":
    sys.exit(main())
