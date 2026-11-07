import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import next_rc_tag as n


def test_after_stable_starts_the_next_patch():
    assert n.next_rc(["v1.0.0", "v1.1.0"]) == "v1.1.1-rc1"


def test_rc_counts_up_numerically():
    assert n.next_rc(["v1.1.0-rc1", "v1.1.0-rc2"]) == "v1.1.0-rc3"
    assert n.next_rc(["v1.1.0-rc9"]) == "v1.1.0-rc10"


def test_other_label_below_rc_moves_to_rc1():
    assert n.next_rc(["v1.1.0-fwstore.5"]) == "v1.1.0-rc1"


def test_today():
    assert n.next_rc(["v1.0.0", "v1.0.1", "v1.1.0-rc1", "v1.1.0-fwstore.5", "v1.1.0-rc2"]) == "v1.1.0-rc3"


def test_label_above_rc_needs_a_human():
    with pytest.raises(n.NoCandidate, match="does not sort above"):
        n.next_rc(["v1.2.0-test.1"])


def test_rc_with_more_identifiers_needs_a_human():
    with pytest.raises(n.NoCandidate):
        n.next_rc(["v1.1.0-rc2.1"])


def test_length_limit():
    assert n.next_rc(["v10000.10000.10-rc9999"]) == "v10000.10000.10-rc10000"
    with pytest.raises(n.NoCandidate, match="not a valid tag"):
        n.next_rc(["v10000.10000.10-rc99999"])


def test_no_tags():
    with pytest.raises(n.NoCandidate):
        n.next_rc(["nightly"])
