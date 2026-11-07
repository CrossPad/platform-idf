import json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fw_semver

RULES = json.loads((Path(__file__).parent / "fw_tag_rules.json").read_text())

ORDER = ["v1.0.0", "v1.0.1", "v1.1.0-fwstore.5", "v1.1.0-rc1", "v1.1.0-rc2", "v1.1.0-rc9",
         "v1.1.0-rc10", "v1.1.0", "v1.1.1-rc1", "v1.2.0-alpha", "v1.2.0-alpha.1", "v1.2.0-beta",
         "v1.2.0", "v10.0.0"]


def test_device_order():
    shuffled = ORDER[::-1][3:] + ORDER[::-1][:3]
    assert [t.name for t in fw_semver.sort_tags(shuffled)] == ORDER


def test_accepted_tags_without_build_metadata_parse():
    for tag in RULES["accepted"]:
        if "+" not in tag:
            assert fw_semver.parse(tag) is not None, tag


def test_refused_tags_do_not_parse():
    for tag in RULES["refused"]:
        assert fw_semver.parse(tag) is None, tag


def test_build_metadata_is_not_a_release_tag_here():
    assert fw_semver.parse("v1.3.0-rc1+b.7") is None


def test_length_limit():
    assert fw_semver.parse("v1.0.0-abcdefghijklmnop") is not None       # 23
    assert fw_semver.parse("v1.0.0-abcdefghijklmnopq") is None          # 24


def test_fields_and_line():
    t = fw_semver.parse("v1.2.3-rc4")
    assert (t.major, t.minor, t.patch, t.pre, t.stable, t.line) == (1, 2, 3, "rc4", False, (1, 2))
    assert fw_semver.parse("v1.2.3").stable


def test_rc_tag_regex():
    assert fw_semver.RC_TAG.fullmatch("v1.1.0-rc12")
    assert not fw_semver.RC_TAG.fullmatch("v1.1.0-fwstore.5")
    assert not fw_semver.RC_TAG.fullmatch("v1.1.0")
    assert not fw_semver.RC_TAG.fullmatch("v1.1.0-rc1.1")


def test_non_ascii_digits_are_refused():
    for name in ["v1.0.0-rc\u0661", "v\u0661.0.0", "v1.0.0-rc\uff12"]:
        assert fw_semver.parse(name) is None, name
        assert not fw_semver.RC_TAG.fullmatch(name), name
        assert fw_semver.main(["check", name]) == 1, name
    assert not fw_semver.RC_PRE.fullmatch("rc\u0661")


def test_non_tags_are_dropped_by_sort():
    assert [t.name for t in fw_semver.sort_tags(["v1.0.0", "nightly", "v1.3"])] == ["v1.0.0"]


def test_cli_check(capsys):
    assert fw_semver.main(["check", "v1.1.0-rc3"]) == 0
    assert fw_semver.main(["check", "v1.1.0-"]) == 1
