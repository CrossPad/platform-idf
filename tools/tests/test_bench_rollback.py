import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402


def test_rollback_supported():
    assert bench.fw_rollback_state("FWSLOT: rollback=1") is True


def test_rollback_not_supported():
    assert bench.fw_rollback_state("FWSLOT: rollback=0") is False


def test_rollback_line_missing_is_unknown():
    # Older firmware without the trailer row, or a board that never answered.
    assert bench.fw_rollback_state("") is None
    assert bench.fw_rollback_state(None) is None
    assert bench.fw_rollback_state("FWSLOT: slot=A running=1 state=confirmed") is None


def test_rollback_line_among_others():
    text = "FWSLOT: slot=A running=1 state=confirmed\nFWSLOT: rollback=1\n"
    assert bench.fw_rollback_state(text) is True


def test_rollback_advisory_supported():
    ok, detail = bench.rollback_advisory(True)
    assert ok is True
    assert detail == "supported"


def test_rollback_advisory_not_supported():
    ok, detail = bench.rollback_advisory(False)
    assert ok is None  # advisory, not a failure
    assert detail == "not supported — flashed bootloader lacks rollback"


def test_running_slot_state_reads_the_running_row():
    text = ("FWSLOT: slot=A running=0 state=installed pkg=test-v2\n"
            "FWSLOT: slot=B running=1 state=trial pkg=-\n"
            "FWSLOT: rollback=1\n")
    assert bench.fw_running_slot_state(text) == "trial"


def test_running_slot_state_without_a_running_row_is_unknown():
    assert bench.fw_running_slot_state("FWSLOT: rollback=1") is None
    assert bench.fw_running_slot_state("") is None
    assert bench.fw_running_slot_state(None) is None


class FakeBoard:
    """The board seen through await_confirm's two callables: it confirms
    itself after `confirms_after` polls, or never (then FW_CONFIRM does it)."""

    def __init__(self, states):
        self.states = list(states)
        self.reads = 0
        self.confirms = 0
        self.slept = 0.0

    def read(self):
        state = self.states[min(self.reads, len(self.states) - 1)]
        self.reads += 1
        return state

    def confirm(self):
        self.confirms += 1
        # Whatever it reads next, it reads confirmed.
        self.states = self.states + ["confirmed"]
        self.states[self.reads:] = ["confirmed"] * (len(self.states) - self.reads)

    def sleep(self, seconds):
        self.slept += seconds


def test_await_confirm_returns_at_once_when_already_confirmed():
    b = FakeBoard(["confirmed"])
    state, waited, how = bench.await_confirm(b.read, b.confirm, b.sleep)
    assert (state, waited, how) == ("confirmed", 0.0, "none")
    assert b.confirms == 0
    assert b.slept == 0.0


def test_await_confirm_waits_for_the_boards_own_timer():
    b = FakeBoard(["trial", "trial", "confirmed"])
    state, waited, how = bench.await_confirm(b.read, b.confirm, b.sleep,
                                             wait_s=25.0, poll_s=2.0)
    assert (state, how) == ("confirmed", "timer")
    assert waited == 4.0            # two polls, not the whole bound
    assert b.confirms == 0


def test_await_confirm_falls_back_to_fw_confirm_when_the_bound_expires():
    b = FakeBoard(["trial"])        # never confirms on its own
    state, waited, how = bench.await_confirm(b.read, b.confirm, b.sleep,
                                             wait_s=6.0, poll_s=2.0)
    assert (state, how) == ("confirmed", "FW_CONFIRM")
    assert b.confirms == 1
    assert waited == 8.0            # the bound, plus the one poll after asking


def test_await_confirm_is_a_noop_on_firmware_without_fw_slots():
    b = FakeBoard([None])
    state, waited, how = bench.await_confirm(b.read, b.confirm, b.sleep)
    assert (state, waited, how) == (None, 0.0, "none")
    assert b.confirms == 0
    assert b.slept == 0.0


def test_await_confirm_reports_a_slot_that_never_confirms():
    b = FakeBoard(["trial"])
    b.confirm = lambda: None        # FW_CONFIRM accepted but nothing changed
    state, _, how = bench.await_confirm(b.read, b.confirm, b.sleep,
                                        wait_s=4.0, poll_s=2.0)
    assert (state, how) == ("trial", "FW_CONFIRM")


def test_app_sha_line():
    digest = "1f" * 32
    assert bench.app_sha_line(f"APPSHA: {digest}") == digest
    assert bench.app_sha_line("OK") is None
    assert bench.app_sha_line(None) is None


def test_rollback_advisory_no_answer_is_distinct_from_not_supported():
    # The Task 11 bug: a timed-out CDC read (no trailer at all) must not read
    # the same as a confirmed rollback=0.
    ok, detail = bench.rollback_advisory(None)
    assert ok is None
    assert detail == "no answer (retry tools/bench.py check)"
    assert detail != bench.rollback_advisory(False)[1]
