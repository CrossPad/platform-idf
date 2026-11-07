import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import factory_provision as fp  # noqa: E402


def test_rollback_ok():
    assert fp.rollback_supported("FWSLOT: rollback=1") is True


def test_rollback_missing_is_a_failure():
    assert fp.rollback_supported("FWSLOT: rollback=0") is False


def test_rollback_no_reply_is_a_failure():
    # No line, a timeout, or firmware old enough to not send the trailer at
    # all — a unit must not leave the factory without confirmed proof either way.
    assert fp.rollback_supported("") is False
    assert fp.rollback_supported(None) is False
    assert fp.rollback_supported("crosspad-hil timed out: cdc FW_SLOTS") is False
