"""The other half of the pad show: owning a pad is not the same as writing its colour.

Measured on a bench, 28.11.2026: three runs of the slide show over a MIDI port, each one
printing a byte count and changing nothing that could be seen on the board. The frames
were arriving -- `SURFACE STATUS` says `writes=` climbed and `claims=0xFFFF` -- but the
surface map's per-pad *mode* was still 0, so the app put its own colours straight back on
top. These tests pin the frames that make the difference, so a future edit cannot drop
them by accident and turn the tool back into a machine that reports success and does
nothing.

The captures here spy on `Sender.syx`, which is what every other method goes through, so
the frame layout stays described in exactly one place -- the production code.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ledshow as ls  # noqa: E402


def spy(s: ls.Sender) -> list[tuple[int, list[int]]]:
    """Record (sub, payload) pairs instead of sending them."""
    got: list[tuple[int, list[int]]] = []

    def fake(sub: int, payload: list[int]) -> None:
        got.append((sub, list(payload)))

    s.syx = fake
    return got


def test_claiming_takes_host_mode_on_all_sixteen_pads():
    s = ls.Sender()
    got = spy(s)
    s.claim_pads()
    assert got == [(ls.SEC_WRITE, [ls.S_PAD_MODE, 0, 16] + [1] * 16)]


def test_a_narrower_claim_carries_its_own_count():
    """The surface refuses a block whose count does not match its payload, so a partial
    claim cannot reuse the sixteen-entry shape."""
    s = ls.Sender()
    got = spy(s)
    s.claim_pads(first=4, count=4)
    assert got == [(ls.SEC_WRITE, [ls.S_PAD_MODE, 4, 4, 1, 1, 1, 1])]


def test_a_run_gives_the_mode_back_as_well_as_the_colours():
    """`give_back` already reset the colours. If it left mode 1 claimed, the board would
    go on showing a host's colours after the host had gone."""
    s = ls.Sender()
    got = spy(s)
    s.give_back()
    assert got == [(ls.SEC_WRITE, [ls.S_PAD_MODE, 0, 16] + [0] * 16),
                   (ls.SEC_RESTORE, [ls.S_PAD_COLOUR, 0, 16])]


def test_the_claim_comes_before_the_first_grid_frame():
    """Order is the whole point: colour first and mode second leaves a beat in which the
    app's colour is still the one on the pad, which on a running board is exactly what
    the app uses."""
    s = ls.Sender()
    got = spy(s)
    s.palette(ls.build_palette())
    s.claim_pads()
    s.grid([0] * 16)
    assert [sub for sub, _ in got] == [ls.PAL_LOAD, ls.SEC_WRITE, ls.SEC_WRITE]
    assert [p[0] for sub, p in got if sub == ls.SEC_WRITE] == [ls.S_PAD_MODE, ls.S_PAD_COLOUR]


def test_the_claim_costs_exactly_one_frame_on_the_wire():
    """The byte report is a claim about the wire, so the frame this tool forgot for a
    week has to show up in it: one more message, 24 more bytes (F0 7D 1D sub +
    S_PAD_MODE first count + 16 values + F7)."""
    plain = ls.Sender()
    plain.palette(ls.build_palette())
    for _ in range(35):
        plain.grid([1] * 16)
    plain.give_back()
    claimed = ls.Sender()
    claimed.palette(ls.build_palette())
    claimed.claim_pads()
    for _ in range(35):
        claimed.grid([1] * 16)
    claimed.give_back()
    assert claimed.messages == plain.messages + 1
    assert claimed.bytes_sent == plain.bytes_sent + 24
