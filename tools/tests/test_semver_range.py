import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import semver_range as sr


def test_parse_version_strict():
    assert sr.parse_version("1.53.0") == (1, 53, 0)
    for bad in ["1.53", "v1.53.0", "01.2.3", "1.2.3-rc1", "1.2.3 ", "١.2.3", "", None, 7]:
        with pytest.raises(sr.RangeError):
            sr.parse_version(bad)


def test_the_forms_the_app_manager_accepts():  # crosspad-apps tests/test_versions.py vectors
    assert sr.satisfies((1, 20, 0), ">=0.3.0")
    assert not sr.satisfies((0, 2, 9), ">=0.3.0")
    assert sr.satisfies((1, 4, 2), "^1.4") and not sr.satisfies((2, 0, 0), "^1.4")
    assert sr.satisfies((1, 4, 9), "~1.4.1") and not sr.satisfies((1, 5, 0), "~1.4.1")
    assert sr.satisfies((1, 2, 3), "1.2.3") and sr.satisfies((9, 9, 9), "*")
    assert not sr.satisfies((1, 2, 0), ">=1.0.0, <1.2.0")


def test_every_operator():
    v = (1, 5, 0)
    assert sr.satisfies(v, ">1.4.9") and not sr.satisfies(v, ">1.5.0")
    assert sr.satisfies(v, "<=1.5.0") and not sr.satisfies(v, "<1.5.0")
    assert sr.satisfies(v, "==1.5.0") and sr.satisfies(v, "=1.5.0") and sr.satisfies(v, ">= 1.5.0")


def test_caret_on_zero_major_holds_the_minor():
    assert sr.satisfies((0, 3, 7), "^0.3.1") and not sr.satisfies((0, 4, 0), "^0.3.1")
    assert sr.satisfies((1, 60, 0), "^1.53.0") and not sr.satisfies((1, 52, 9), "^1.53.0")


def test_tilde_with_only_a_major_holds_the_major():
    assert sr.satisfies((1, 9, 0), "~1") and not sr.satisfies((2, 0, 0), "~1")


@pytest.mark.parametrize("spec", ["", "   ", "latest", ">=1.2.x", "=>1.2.0", ">=1.2.0,", ",>=1.0.0",
                                  ">=v1.2.0", ">=1.2.3-rc1", "!=1.0.0", None, 3, "x" * 65])
def test_unreadable_ranges_are_errors(spec):
    with pytest.raises(sr.RangeError):
        sr.satisfies((1, 0, 0), spec)


def test_space_separated_and_is_an_error():
    with pytest.raises(sr.RangeError):
        sr.satisfies((1, 5, 0), ">=1.2.0 <2.0.0")


def test_parse_range_validates_without_evaluating():
    assert sr.parse_range(">=1.2, <2") == [(">=", (1, 2, None)), ("<", (2, None, None))]
    assert sr.parse_range("*") == []
    with pytest.raises(sr.RangeError):
        sr.parse_range("nope")


def test_fmt():
    assert sr.fmt((1, 53, 0)) == "1.53.0"
