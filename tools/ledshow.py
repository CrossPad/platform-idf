"""A slide show on the pad grid: the word `crosspad` written across the 4x4 pads, plus a
few animations, driven over the SysEx control surface this branch adds.

    python3 tools/ledshow.py --list              # the font and the pad rows, on paper
    python3 tools/ledshow.py --dry-run           # frames and bytes, nothing sent
    python3 tools/ledshow.py --slide 150 --loops 2
    python3 tools/ledshow.py --camera            # needs the webcam harness, see below

**Which pad is which, because this tool got it wrong before and the movie came out
mirrored.** A pad index is `row * 4 + column`, with **row 0 the BOTTOM row of the board**
and columns counted left to right, so the label printed on the case is index + 1: pads
0..3 are the bottom row, 12..15 the top. Photographed proof on a v2 board on 28.11.2026,
and the same statement the firmware makes about itself
(`crosspad-gui/src/virtual_device/VirtualPadGrid.cpp`, `mode_overlay.cpp`), the Sampler's
own on-screen grid, the printed case and both manuals: paint index 1 and index 4 and they
light the squares printed PAD 2 and PAD 5. `docs/subsystems/pad-grid-order.md` has the
whole thing, including the three commands to re-prove it. The bitmap below is written the
way a person reads it -- row 0 is the top of the letters -- and `frame_at()` is the one
place that turns that into pad order. `--list` prints both, so nobody has to trust the
board to check the layout.

Why the palette rather than one colour per pad per frame: each frame here is sixteen
palette *indices*, which is 24 bytes of SysEx for a whole grid, against 55 bytes for the
same grid as direct colour and 144 bytes for one pad per message. The tool prints the
bytes it sent, so the claim in docs/subsystems/surface-sysex.md is something you can read
off a run instead of taking on trust. (The arithmetic is also pinned by
platform-idf/main/tests/test_surface_map.cpp, so the documentation and the frame cannot
drift apart.)

It leaves the board as it found it: every frame is followed, at the end, by SEC_RESTORE
for the pads it painted and `PadMode 0` for all sixteen, so the apps' own colours come
back -- the same give-back rule the firmware enforces when a host goes quiet, applied on
purpose. The palette is left loaded: it is the host's own colour table, and a board has
no use for it that could be undone.

Nothing in here touches audio.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

# The webcam harness (cam.py, its calib.json) is not in this repository: it drags in
# numpy, scipy and an ffmpeg dshow source, none of which the rest of tools/ needs. Point
# CROSSPAD_CAM_TOOLS at a directory that holds it and --camera works; without it --camera
# says so plainly instead of reporting a check it did not do.
CAM_TOOLS = os.environ.get("CROSSPAD_CAM_TOOLS", "")
if CAM_TOOLS:
    sys.path.insert(0, CAM_TOOLS)

SEC_WRITE, SEC_RGB, PAL_LOAD, SEC_RESTORE = 0x11, 0x12, 0x13, 0x14
S_PAD_MODE, S_PAD_COLOUR, S_ANIM = 0x00, 0x01, 0x07

# A 3-wide, 4-tall font: small enough that two letters fit beside each other on the grid
# and a whole word can be scrolled past a 4-column window, chunky enough that the pads
# -- which are the display, not a LCD -- stay readable from a metre away. The rows are
# top to bottom, "#" a lit pad.
FONT = {
    "c": ["###", "#..", "#..", "###"],
    "r": ["##.", "#.#", "##.", "#.#"],
    "o": ["###", "#.#", "#.#", "###"],
    "s": [".###".replace("#", "#")[:3], "#..", ".#.", "..#"],     # 3 wide: see NOTE below
    "p": ["###", "#.#", "##.", "#.."],
    "a": ["##.", "..#", "###", "..#"],
    "d": ["..#", ".##", "#.#", "###"],
    " ": ["...", "...", "...", "..."],
}
# NOTE: `s` is the one letter a 3x4 grid cannot draw honestly, so it is drawn as the
# diagonal it becomes -- which reads as a squiggle at pad scale. The word is still
# recognisable in motion (that is what the scroll is for), and `--list` prints the strip
# so a person can see what the font actually looks like before trusting it.

WORD = "crosspad"
GAP = "."          # one blank column between letters


def letter_rows(ch: str) -> list[str]:
    rows = FONT.get(ch.lower())
    if rows is None:
        raise SystemExit(f"no glyph for {ch!r} in this font; the word shown is {WORD!r}")
    return [(rows[i] + "...")[:3] for i in range(4)]


def strip_columns() -> list[str]:
    """The word as four rows of one column each, letters separated by a blank column."""
    cols = ["", "", "", ""]
    for i, ch in enumerate(WORD):
        rows = letter_rows(ch)
        for r in range(4):
            cols[r] += rows[r]
        if i != len(WORD) - 1:
            for r in range(4):
                cols[r] += GAP
    return cols


def frame_at(cols: list[str], left: int) -> list[int]:
    """The 4x4 window starting at column `left`, as palette indices: 1 for a lit cell,
    0 for dark. The strip is padded on both ends so the word can scroll fully through."""
    # Wide enough for every window the show actually asks for: the scroll runs to
    # `total = strip + 4`, so a window can end three columns past the word plus the four
    # blank ones here. Three columns of padding on a side was one short, and the last step
    # then built a frame of fifteen indices labelled sixteen -- which the board refused
    # (`too_short`) and painted nothing for. One refused frame per run reads like noise
    # until something counts it.
    padded = ["...." + c + "...." for c in cols]
    # `cols[r]` is row r of the *letters*, r = 0 at their top. On the board, pad indices
    # 0..3 are the BOTTOM row, so letter row 0 has to land on pads 12..15: the rows are
    # fed bottom-up. This is the whole bug in one line of history -- the loop used to run
    # `for r in range(4)` and paint the word vertically flipped. The webcam saw the board
    # upside down, which is a vertical flip plus a horizontal flip, so the two vertical
    # flips cancelled and the horizontal one survived: the recorded word read mirrored.
    # Fixed at both ends (this order, and rotating the capture), and pinned by
    # tools/tests/test_ledshow_grid.py, which is written as the mirror test this lacked.
    grid = []
    for r in (3, 2, 1, 0):
        row = padded[r][left:left + 4]
        for cell in row:
            grid.append(1 if cell == "#" else 0)
    return grid


def build_palette() -> list[int]:
    """Entry 0 dark, entry 1 the word's colour, entries 2..7 the colours the colour-cycle
    animation walks. 7-bit wire values; the board doubles them into the strip's 8-bit."""
    pal = [0, 0, 0]
    pal += [0x7F, 0x2A, 0x10]          # 1: warm orange, the pad grid's usual accent
    for r, g, b in ((0x74, 0x20, 0x20), (0x74, 0x5A, 0x14), (0x2C, 0x6E, 0x20),
                    (0x18, 0x50, 0x74), (0x40, 0x24, 0x74), (0x6E, 0x22, 0x44)):
        pal += [r, g, b]
    return pal


class MidiOut:
    """One USB MIDI output to the board, matched by port name -- the same way
    tools/ota_flash.py finds its SysEx port. The ESP's own port does not have
    "MIDI+Serial" in its name; the STM bridge's does, and that one serves the TRS
    jack rather than the USB cable, so prefer the port without it.

    rtmidi is imported inside the constructor, so --list and --dry-run -- and the
    tests in tools/tests/test_ledshow_grid.py -- work on a machine with no board and
    no MIDI stack at all. That is deliberate: the part worth testing here is the
    bitmap-to-pad mapping, and it must not need hardware to be checked."""

    @staticmethod
    def port_names() -> list[str]:
        import rtmidi
        out = rtmidi.MidiOut()
        return [out.get_port_name(i) for i in range(out.get_port_count())]

    def __init__(self, name_part: str = "crosspad", index: int | None = None):
        import rtmidi
        self.rt = rtmidi
        self.out = None
        names = self.port_names()
        if index is not None:
            # Pick by the number --midi-list prints. Matching by name is how this tool
            # found a board before, and on a bench with two of them the names differ by
            # a digit and a space -- which is also exactly what a shell eats.
            if not 0 <= index < len(names):
                raise SystemExit(f"no MIDI output port {index}; this machine offers "
                                 f"0..{len(names) - 1} (see --midi-list)")
            self.index = index
            self.name = names[index]
            return
        cand = [i for i, n in enumerate(names) if name_part.lower() in n.lower()]
        esp = [i for i in cand if "midi+serial" not in names[i].lower()]
        cand = esp or cand
        if not cand:
            raise SystemExit(f"no MIDI output matching {name_part!r}; the ports this "
                             f"machine offers are: {names}")
        if len(cand) > 1:
            # Two boards on one bench, two ports whose names both contain the filter.
            # Choosing the wrong one paints a board nobody is looking at and reports
            # complete success, which is the worst kind of nothing: measured here on
            # 28.11.2026, three runs of the show over a port belonging to the other
            # board, each one printing a byte count and changing nothing visible.
            print(f"warning: {len(cand)} MIDI outputs match {name_part!r}: "
                  f"{[names[i] for i in cand]}")
            print(f"         driving {names[cand[0]]!r}; pass --midi to choose another, "
                  f"or --midi-list to see them all")
        self.index = cand[0]
        self.name = names[cand[0]]

    def open(self) -> None:
        self.out = self.rt.MidiOut()
        self.out.open_port(self.index)
        time.sleep(0.15)              # let the stack settle after opening

    def send(self, msg: list[int]) -> None:
        if self.out:
            self.out.send_message(msg)

    def close(self) -> None:
        if self.out:
            try:
                self.out.close_port()
            except Exception:
                pass                  # the port may already be gone if the board left the bus
            self.out = None


class Sender:
    """The frames, either onto the wire or onto stdout for --dry-run."""

    def __init__(self, midi=None):
        self.midi = midi
        self.bytes_sent = 0
        self.frames_sent = 0
        self.messages = 0

    def syx(self, sub: int, payload: list[int]) -> None:
        frame = [0xF0, 0x7D, 0x1D, sub] + [b & 0x7F for b in payload] + [0xF7]
        self.bytes_sent += len(frame)
        self.messages += 1
        if self.midi:
            self.midi.send(frame)

    def grid(self, indices: list[int]) -> None:
        # The board refuses a block whose count does not match its payload; a tool that
        # sends one has painted nothing for that frame and will not notice in the dark.
        # Say it here, where the person running the tool is looking, rather than leaving it
        # in the board's `refused=` counter for someone else to chase.
        if len(indices) != 16:
            raise ValueError(f"a grid frame needs 16 indices, this one has {len(indices)}: "
                             f"{indices}")
        self.syx(SEC_WRITE, [S_PAD_COLOUR, 0, 16] + indices)
        self.frames_sent += 1

    def palette(self, triples: list[int]) -> None:
        self.syx(PAL_LOAD, [0, len(triples) // 3] + triples)

    def claim_pads(self, first: int = 0, count: int = 16) -> None:
        """Take host mode on the pads this run is about to paint.

        The surface map's pad mode is per pad: 0 = the board paints its own colours
        again, 1 = the host holds this pad (docs/subsystems/surface-sysex.md). A parked
        colour is not ownership: write colours without taking the mode and every frame
        is accepted, parked and repainted by the app in the same beat. On current
        firmware that is a run which appears to do nothing and reports no error
        anywhere -- measured on a bench, twice, before this existed. `give_back()`
        puts mode 0 back, so the board gets its own colours at the end either way."""
        self.syx(SEC_WRITE, [S_PAD_MODE, first, count] + [1] * count)

    def anim(self, which: int, period_ms: int) -> None:
        self.syx(SEC_WRITE, [S_ANIM, 0, 2, which, max(1, period_ms // 10)])

    def give_back(self) -> None:
        self.syx(SEC_WRITE, [S_PAD_MODE, 0, 16] + [0] * 16)
        self.syx(SEC_RESTORE, [S_PAD_COLOUR, 0, 16])

    def report(self) -> str:
        return (f"{self.frames_sent} grid frames, {self.messages} SysEx messages, "
                f"{self.bytes_sent} bytes on the wire")


def describe_grid(indices: list[int]) -> list[str]:
    """A 16-index frame, printed the way the board is held: top row of pads first
    (that is indices 12..15), each cell marked, each row labelled with the pad numbers
    a person can find on the case. `--list` uses this so the layout can be checked on
    paper instead of by trusting a run on real pads."""
    if len(indices) != 16:
        raise ValueError(f"a frame needs 16 indices, this one has {len(indices)}")
    out = []
    for row in (3, 2, 1, 0):                       # the top pad row is index row 3
        cells = indices[row * 4: row * 4 + 4]
        mark = " ".join("#" if c else "\u00b7" for c in cells)
        nums = " ".join(f"{row * 4 + c:>2d}" for c in range(4))
        where = "bottom row" if row == 0 else ("top row" if row == 3 else f"row {row + 1}")
        out.append(f"{mark}    pads {nums}   ({where})")
    return out


def hold(s: Sender, indices: list[int], ms: int) -> None:
    s.grid(indices)
    time.sleep(ms / 1000.0)


def camera_check(calib, expected_lit: set[int]) -> tuple[float, list[int]]:
    """What the webcam sees versus what the frame asked for: the score is the fraction of
    the sixteen pads whose lit/unlit state matches the bitmap."""
    if not CAM_TOOLS:
        raise SystemExit("--camera wants the webcam harness: set CROSSPAD_CAM_TOOLS to the "
                         "directory holding cam.py (it is not in this repository yet). "
                         "The show itself needs no camera.")
    import cam
    img = cam.grab_frame()
    mismatches = []
    for p in range(16):
        own = cam.sample_pad(img, calib, p)
        lit = max(own) > 25            # the pad's own light, not the room's
        if lit != (p in expected_lit):
            mismatches.append(p)
    return (16 - len(mismatches)) / 16.0, mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="print the strip the font produces")
    ap.add_argument("--dry-run", action="store_true", help="count frames and bytes, send nothing")
    ap.add_argument("--slide", type=int, default=170, help="milliseconds per scroll step")
    ap.add_argument("--hold", type=int, default=900, help="milliseconds a whole word sits still")
    ap.add_argument("--loops", type=int, default=1, help="how many times to scroll the word")
    ap.add_argument("--colour-cycle", action="store_true", help="walk the palette while scrolling")
    ap.add_argument("--board-anim", action="store_true",
                    help="finish with the board's own chase and ring animations, asked for by SysEx")
    ap.add_argument("--camera", action="store_true",
                    help="freeze on one slide and let the webcam say whether the pads are lit "
                         "the way the bitmap asked")
    ap.add_argument("--freeze", type=int, default=None, metavar="COLUMN",
                    help="paint one window of the strip and hold it still: the frame a "
                         "photograph, a camera check or a retake of the movie needs. A "
                         "window is 4 columns and a glyph is 3 plus a blank column, so one "
                         "frame holds exactly one letter -- the word is legible in motion, "
                         "not in a still. --list prints one line per letter, with the column to pass")
    ap.add_argument("--freeze-for", type=float, default=6.0, metavar="SECONDS",
                    help="how long to hold the frozen window (default 6 s, enough to "
                         "photograph; the pads are handed back when it ends)")

    ap.add_argument("--midi", default=None, help="MIDI port name to match (default: the ESP's own)")
    ap.add_argument("--midi-index", type=int, default=None, metavar="N",
                    help="drive MIDI output port N, the number --midi-list prints; with two "
                         "boards on the bench this is the only way to be sure which one is "
                         "getting the frames")
    ap.add_argument("--midi-list", action="store_true",
                    help="print every MIDI output this machine offers and exit -- with two "
                         "boards attached the names are nearly identical and guessing which "
                         "one is the board in front of the camera is not a strategy")
    a = ap.parse_args()

    if a.midi_list:
        # Deliberately before anything else: this is the question "what is plugged in",
        # and it should answer it without needing --list, a board, or a hub lease.
        names = MidiOut.port_names()
        for i, n in enumerate(names):
            print(f"  {i:>2}  {n}")
        return 0

    cols = strip_columns()
    if a.list:
        print(f"`{WORD}` as {len(cols[0])} columns of a 3x4 font, scrolled through a 4x4 window:")
        print("  letter rows, top of the letters first -- and the pads each one lands on:")
        for r in range(4):
            pads = [(3 - r) * 4 + c for c in range(4)]
            shown = " ".join(f"pad {n:<2d}" for n in pads)
            print("  " + cols[r].replace(".", "\u00b7") + "   ->   " + shown)
        ruler = "".join(str(c % 10) for c in range(len(cols[0])))
        print("  column ruler for --freeze: " + ruler)
        for i, ch in enumerate(WORD):
            c = i * 4
            top = cols[0][c:c + 4].replace(".", "\u00b7")
            lit = sum(1 for r in range(4) for cell in cols[r][c:c + 4] if cell == "#")
            print(f"    --freeze {c:<2d}  letter {ch!r}: top row {top}, {lit} pads lit")
        print("  (pads 0..3 are the BOTTOM row of the board; the printed label on the case "
              "is index + 1)")
        print("  the same strip, laid out as the board is held (a window at column 6):")
        for line in describe_grid(frame_at(cols, 6)):
            print("  " + line)
        print(f"one window frame = 16 palette indices = 24 bytes of SysEx")
        return 0

    total = len(cols[0]) + 4          # the strip plus the padding on both ends
    if a.dry_run:
        s = Sender()
        s.palette(build_palette())
        s.claim_pads()
        for step in range(total):
            s.grid(frame_at(cols, step))
        s.give_back()
        print(f"{total} scroll steps at {a.slide} ms -> {s.report()}")
        print(f"that is {s.bytes_sent // max(1, s.frames_sent)} bytes per grid frame on average")
        return 0

    # The check has to happen before anything is opened: no harness, no camera run, and
    # the board keeps its own pad colours.
    if a.camera and not CAM_TOOLS:
        raise SystemExit("--camera wants the webcam harness: set CROSSPAD_CAM_TOOLS to the "
                         "directory holding cam.py (it is not in this repository yet). "
                         "The show itself needs no camera.")

    from crosspad_hil import hub_link

    with hub_link.exclusive("ledshow: pad slide show over the SysEx surface"):
        midi = MidiOut(name_part=a.midi or "crosspad", index=a.midi_index)
        midi.open()
        # Two boards on one bench means two ports whose names both contain
        # "crosspad"; say which one is being driven.
        print(f"midi out: {midi.name}")
        # There used to be a CDC session here as well, opened for --camera and closed at
        # the end without ever being asked anything: the camera check reads the pads from
        # a frame, not from the board, so the session bought nothing and cost a port.
        s = Sender(midi=midi)
        calib = None
        if a.camera:
            import cam                          # guarded by CAM_TOOLS, checked above
            calib = cam.load_calib(cam.CALIB)
        try:
            s.palette(build_palette())
            s.claim_pads()
            time.sleep(0.06)
            if a.freeze is not None:
                # One window, held, and nothing else: the show is 35 frames a second for
                # the eye, which is exactly what a camera cannot read. Returning from here
                # still runs the finally below, so the pads go back to the board.
                # `--list` numbers the strip the person is reading; `frame_at` indexes the
                # copy padded with four blank columns on each side. The gap between the two
                # is four, and without adding it `--freeze 20` paints the window that
                # `--list` calls 16 -- an off-by-four that only shows up when somebody
                # photographs the wrong letter and wonders why.
                col = max(0, min(total - 4, a.freeze + 4))
                frame = frame_at(cols, col)
                s.grid(frame)
                print(f"frozen window at strip column {col - 4} (held {a.freeze_for:g} s) -- "
                      f"pads as the board is held (top row first):")
                for line in describe_grid(frame):
                    print("   " + line)
                print(f"lit pads: {[i for i, v in enumerate(frame) if v]}")
                time.sleep(a.freeze_for)
                return 0
            for loop in range(max(1, a.loops)):
                # The scroll: one 24-byte frame per step, the word walking across the grid.
                for step in range(total):
                    idx = frame_at(cols, step)
                    if len(idx) != 16:      # belt and braces: the strip padding decides it
                        raise ValueError(f"window at column {step} is short: {len(idx)} "
                                         f"indices, strip is {len(cols[0])} wide")
                    if a.colour_cycle:
                        # A palette index change costs nothing extra on the wire: the
                        # grid frame already carries sixteen of them, so the colour walks
                        # with the same bytes.
                        accent = 2 + (step // 3) % 6
                        idx = [accent if v else 0 for v in idx]
                    hold(s, idx, a.slide)
                # The word sits still for a beat, so a person can read it before it moves.
                still = frame_at(cols, (total - 4))
                hold(s, still, a.hold // 4)
                # A wipe-in: the same bitmap, revealed column by column, no extra frames
                # beyond the ones the reveal needs -- and it fades by writing colour
                # directly, which is what SEC_RGB is for.
                base = frame_at(cols, total - 5)
                for revealed in range(1, 5):
                    grad = []
                    for p, on in enumerate(base):
                        col = p % 4
                        grad.append(1 if (on and col < revealed) else 0)
                    hold(s, grad, 90)
                for level in (0x18, 0x38, 0x58, 0x78):
                    # Direct colour, four steps of a fade: this is what SEC_RGB is for --
                    # a colour the palette does not hold, without reloading the palette
                    # first. 7-bit on the wire, doubled by the board, so a fade here is
                    # 128 steps rather than the pad's 256.
                    triples = []
                    for v in base:
                        triples += [(level if v else 0), (level // 3 if v else 0),
                                    (level // 6 if v else 0)]
                    s.syx(SEC_RGB, [0, 16] + triples)
                    time.sleep(0.10)
                print(f"loop {loop + 1}: {s.report()}")
            if a.camera:
                # Freeze on a readable window and ask the webcam what it sees. The bitmap
                # says which pads should be lit; the camera is an outside witness to
                # whether they are.
                pick = next(k for k in range(total) if frame_at(cols, k).count(1) >= 5)
                lit = {p for p, v in enumerate(frame_at(cols, pick)) if v}
                hold(s, [v or 0 for v in frame_at(cols, pick)], 400)
                score, mism = camera_check(calib, lit)
                print(f"camera: window at column {pick}, asked for pads {sorted(lit)}")
                print(f"camera: {score * 16:.0f}/16 pads agree, mismatches {mism}")
            if a.board_anim:
                # The board's own animations, asked for by SysEx: two bytes for `which`
                # and the period, and the stepping happens on the board.
                s.anim(1, 130)
                time.sleep(1.6)
                s.anim(2, 150)
                time.sleep(1.6)
                s.anim(0, 120)
                time.sleep(0.2)
        finally:
            s.give_back()
            time.sleep(0.25)
            print(f"total: {s.report()}")
            midi.close()
    print("the pads were given back to the board's own colours")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
