#!/usr/bin/env python3
"""Arrange by finger: what a player does on the timeline with one finger,
done through the panel's read path (TOUCH_TAP / TOUCH_DRAG) and checked by
what the board then says.

On LIVE_ARM (a looped 4-bar song with kit blocks from bar 0, on the board's
card), stopped, zoomed to a bar per 48 px (Arrange's default):

  ruler     a tap on bar 3 of the ruler: the top bar's position reads 3.1
  region    the ruler held on bar 1 and dragged to bar 3: ASONG rgn=0-3, looping
  undo      EDIT held: the region is gone again (one undo step)
  drag      a block held and dragged a bar right: its label moved 48 px;
            EDIT held puts it back

Every step undoes itself, so the song is left as it was. Exit 0 all of it,
1 a step did not do what it should, 2 the test setup.

  python3 tools/hil_arrange_touch.py
"""
from __future__ import annotations

import re
import sys
import time

from hil_perf_song import BenchError, Board, asong, fields, wait_song_ready

SONG = "LIVE_ARM"

PX_PER_BAR = 48          # kZoomPx[2], Arrange's zoom as it opens
LANES_X = 90             # the timeline's content starts here (88 + 2 of padding)
RULER_Y = 76
EDIT = (145, 53)         # the context row's EDIT key


def ui(b: Board) -> list[dict]:
    """UI_TREE rows as dicts (text, x, y, w, h)."""
    assert b.s
    b.s.reset_input_buffer()
    b.s.write(b"UI_TREE\r\n")
    buf, end = b"", time.time() + 4
    while time.time() < end and b"UITREE: end" not in buf:
        buf += b.s.read(65536)
    rows = []
    for line in buf.decode(errors="replace").splitlines():
        if not line.startswith("UITREE: i="):
            continue
        f = dict(re.findall(r"(\w+)=(\S*)", line.split(" text=")[0]))
        f["text"] = line.split(" text=", 1)[1] if " text=" in line else ""
        rows.append(f)
    return rows


def position(b: Board) -> str | None:
    """The top bar's position, "bar.beat"."""
    for r in ui(b):
        # The second key of the bar: BPM's reading is a number with a point too.
        if int(r.get("y", 999)) < 40 and 44 <= int(r.get("x", -1)) < 84 and \
                re.fullmatch(r"\d+\.\d", r["text"].strip()):
            return r["text"].strip()
    return None


def block_labels(b: Board) -> list[tuple[int, int, str]]:
    """(x, y, text) of every block label on the lanes."""
    out = []
    for r in ui(b):
        x, y = int(r.get("x", -1)), int(r.get("y", -1))
        if LANES_X <= x < 300 and 85 <= y < 200 and re.fullmatch(r"[A-Z]\d*|Seq \d+", r["text"].strip()):
            out.append((x, y, r["text"].strip()))
    return out


def main() -> int:
    fails: list[str] = []

    def check(ok: bool, what: str) -> None:
        print(f"  {'ok  ' if ok else 'FAIL'} {what}", flush=True)
        if not ok:
            fails.append(what)

    try:
        b = Board()
        b.cmd("APP_START Arrange", "OK")
        time.sleep(2)
        b.cmd(f"ASONG OPEN {SONG}", "ASONG:")
        wait_song_ready(b, SONG, 30)
        b.cmd("ASONG STOP", "ASONG:")
        b.cmd("ASONG STOP", "ASONG:")
        time.sleep(1)
        rgn0 = fields(b.cmd("ASONG", "ASONG:")).get("rgn")

        b.cmd(f"TOUCH_TAP {LANES_X + 2 * PX_PER_BAR + 10} {RULER_Y}", "OK")
        time.sleep(0.8)
        pos = position(b)
        check(pos == "3.1", f"ruler tap on bar 3: position {pos}")

        x1, x3 = LANES_X + 10, LANES_X + 2 * PX_PER_BAR + 10
        b.cmd(f"TOUCH_DRAG {x1} {RULER_Y} {x3} {RULER_Y} 400 150 700", "OK")
        time.sleep(1.6)
        st = fields(b.cmd("ASONG", "ASONG:"))
        check(st.get("rgn") == "0-3", f"ruler held and dragged over bars 1-3: rgn={st.get('rgn')}")

        b.cmd(f"TOUCH_TAP {EDIT[0]} {EDIT[1]} 900", "OK")
        time.sleep(1.5)
        st = fields(b.cmd("ASONG", "ASONG:"))
        check(st.get("rgn") == rgn0, f"EDIT held undid it: rgn={st.get('rgn')} (was {rgn0})")

        before = block_labels(b)
        if not before:
            check(False, "a block on the lanes to drag")
        else:
            x, y, text = min(before)
            b.cmd(f"TOUCH_DRAG {x + 6} {y + 12} {x + 6 + PX_PER_BAR} {y + 12} 400 150 700", "OK")
            time.sleep(1.6)
            after = block_labels(b)
            moved = any(t == text and ay == y and ax == x + PX_PER_BAR for ax, ay, t in after)
            check(moved, f"block {text} at x={x} held and dragged a bar right: now {sorted(after)[:4]}")
            b.cmd(f"TOUCH_TAP {EDIT[0]} {EDIT[1]} 900", "OK")
            time.sleep(1.5)
            back = block_labels(b)
            check(sorted(back) == sorted(before), "EDIT held put the block back")
    except BenchError as e:
        print(f"BENCH: {e}")
        return 2
    print(f"ARRANGE_TOUCH {'clean' if not fails else f'{len(fails)} failed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
