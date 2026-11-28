"""The pad order in the LED show: the test this tool did not have, and which is the
only reason the mirrored movie would have been caught before it was photographed."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ledshow as ls  # noqa: E402

# On the board a pad index is row * 4 + column with **row 0 the bottom row** (case labels,
# the Sampler's own grid, VirtualPadGrid.cpp, mode_overlay.cpp, and -- for the last two --
# photographed on 28.11.2026: index 1 lights the square printed PAD 2, index 4 lights
# PAD 5). The bitmap in ledshow is written the way a person reads text, row 0 at the top
# of the letters, so `frame_at` is the one place that has to translate. It used not to.


def window_of(rows: list[str]) -> list[int]:
    """A 4-row bitmap as a frame, at the scroll position where the bitmap starts.

    `frame_at` pads both ends by four blank columns so the word can scroll fully
    through, so the unpadded bitmap begins at `left=4`.
    """
    cols = [r.ljust(4, ".")[:4] for r in rows]
    return ls.frame_at(cols, 4)


def test_the_top_of_the_letters_lands_on_the_top_row_of_pads():
    grid = window_of(["####", "....", "....", "...."])
    assert grid[12:16] == [1, 1, 1, 1]        # the top pad row
    assert grid[0:12] == [0] * 12             # everything below it dark


def test_the_bottom_of_the_letters_is_the_bottom_row_of_pads():
    grid = window_of(["....", "....", "....", "#.#."])
    assert grid[0:4] == [1, 0, 1, 0]          # pads 0..3 are the bottom row, left to right
    assert grid[4:16] == [0] * 12


def test_left_and_right_are_not_swapped():
    """One lit cell at the left edge, at the top of the letters: it must be pad 12, the
    top row's leftmost pad -- not pad 15, which is what a mirror would give."""
    grid = window_of(["#...", "....", "....", "...."])
    assert grid[12] == 1
    assert grid[13] == 0 and grid[14] == 0 and grid[15] == 0


def test_a_letter_with_a_left_hand_stem_stays_on_the_left():
    # `p` is drawn with its stem down the left side. Painted through the wrong mapping
    # (transposed, or mirrored about the vertical axis) the stem would sit against the
    # right edge of the pad row and the whole word would read backwards on the board.
    rows = ls.letter_rows("p")                # ["###", "#.#", "##.", "#.."]
    assert rows[0] == "###" and rows[3] == "#.."
    grid = window_of(rows)
    assert grid[12:15] == [1, 1, 1] and grid[15] == 0     # top row of the glyph
    assert grid[8] == 1 and grid[9] == 0 and grid[10] == 1 and grid[11] == 0
    assert grid[4] == 1 and grid[5] == 1 and grid[6] == 0 and grid[7] == 0
    assert grid[0] == 1 and grid[1] == 0 and grid[2] == 0 and grid[3] == 0
    # the stem column, read as pads: pads 12, 8, 4, 0 are all lit; none of 15, 11, 7, 3
    assert [grid[i] for i in (12, 8, 4, 0)] == [1, 1, 1, 1]
    assert [grid[i] for i in (15, 11, 7, 3)] == [0, 0, 0, 0]


def test_every_window_of_the_real_word_is_a_full_sixteen():
    """The scroll used to build a frame of fifteen indices labelled sixteen, which the
    board refused -- a silently unpainted frame is worse than an error."""
    cols = ls.strip_columns()
    total = len(cols[0]) + 4
    for left in range(total):
        assert len(ls.frame_at(cols, left)) == 16


def test_describe_grid_prints_the_board_as_a_person_holds_it():
    grid = [0] * 16
    grid[0] = 1                                   # bottom-left pad, index 0
    lines = ls.describe_grid(grid)
    assert len(lines) == 4
    assert "bottom row" in lines[-1] and "pads  0" in lines[-1]
    assert "top row" in lines[0]
    assert "#" not in lines[0] and "#" in lines[-1]
    try:
        ls.describe_grid([0] * 15)
    except ValueError:
        pass
    else:
        raise AssertionError("a short frame should not be accepted")


def test_the_sender_refuses_a_short_frame_before_it_reaches_the_board():
    s = ls.Sender()
    try:
        s.grid([1] * 15)
    except ValueError:
        pass
    else:
        raise AssertionError("a fifteen-index frame must not be sent as sixteen")
