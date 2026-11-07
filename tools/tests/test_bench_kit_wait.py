import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402

NOT_YET = "error: BAD_ARGS: KIT_LOAD 0: device replied ERR bad kit id"


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def scripted(replies):
    calls = []

    def hil_fn(*args, timeout=0.0):
        calls.append(args)
        return replies.pop(0) if len(replies) > 1 else replies[0]
    return hil_fn, calls


def test_kit_load_waits_for_the_card_after_a_reset():
    """~15 s after a reset the card is not mounted: "bad kit id" until it is."""
    clock = Clock()
    hil_fn, calls = scripted([(2, NOT_YET, None)] * 4 + [(0, "kit 0 loaded", None)])
    notes = []
    rc = bench.load_home_kit(notes.append, hil_fn=hil_fn, sleep=clock.sleep, clock=clock)
    assert rc == bench.EXIT_OK
    assert len(calls) == 5 and all(c[:3] == ("kit", "load", "0") for c in calls)
    assert "not listed yet" in notes[0]


def test_a_card_that_never_comes_is_a_bench_error_with_the_boards_words(capsys):
    clock = Clock()
    hil_fn, _ = scripted([(2, NOT_YET, None)])
    rc = bench.load_home_kit(lambda m: None, hil_fn=hil_fn, sleep=clock.sleep, clock=clock,
                             wait_s=10.0)
    assert rc == bench.EXIT_BENCH
    out = capsys.readouterr().out
    assert "bad kit id" in out and "never offered kit 0" in out


def test_any_other_failure_is_not_retried():
    clock = Clock()
    hil_fn, calls = scripted([(2, "error: TIMEOUT: no reply", None)])
    rc = bench.load_home_kit(lambda m: None, hil_fn=hil_fn, sleep=clock.sleep, clock=clock)
    assert rc == bench.EXIT_BENCH and len(calls) == 1


def test_flash_does_not_need_a_kit(monkeypatch):
    """The ticket: a flash right after a reset ended at "KIT_LOAD bad kit id"
    and flashed nothing. The flash now asks ensure() for no kit at all."""
    seen = {}

    def fake_ensure(verbose=True, kit=True):
        seen["kit"] = kit
        return bench.EXIT_BENCH        # stop right after: only the call matters

    monkeypatch.setattr(hardware, "ensure", fake_ensure)
    assert bench.cmd_flash(type("A", (), {"assets": False})()) == bench.EXIT_BENCH
    assert seen == {"kit": False}
