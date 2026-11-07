import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_retention as rr


def rels(*specs):
    """'v1.1.0-rc1' published, 'v1.1.0-rc2*' draft."""
    out = []
    for i, s in enumerate(specs):
        out.append({"id": i + 1, "tag_name": s.rstrip("*"), "draft": s.endswith("*")})
    return out


def tags(items):
    return [x["tag"] for x in items]


def test_no_stable_published_keeps_newest_line():
    p = rr.plan(rels("v1.1.0-rc1", "v1.1.0-rc2", "v1.1.0-fwstore.3*"))
    assert p.unpublish == []
    assert p.newest_stable is None
    assert [k for k, _ in p.keep] == ["v1.1.0-rc1", "v1.1.0-rc2"]


def test_older_line_keeps_only_its_newest_stable():
    p = rr.plan(rels("v1.0.0", "v1.0.1", "v1.1.0-rc1", "v1.1.0-rc2", "v1.1.0", "v1.1.1-rc1",
                     "v1.2.0-rc1"))
    assert tags(p.unpublish) == ["v1.0.0", "v1.1.0-rc1", "v1.1.0-rc2", "v1.1.1-rc1"]
    assert dict(p.keep)["v1.0.1"].startswith("newest stable of 1.0")
    assert dict(p.keep)["v1.1.0"].startswith("newest stable of 1.1")
    assert p.newest_stable == "v1.1.0"


def test_older_line_without_stable_keeps_its_newest_release():
    p = rr.plan(rels("v1.1.0-rc1", "v1.1.0-rc2", "v1.2.0-rc1"))
    assert tags(p.unpublish) == ["v1.1.0-rc1"]
    assert dict(p.keep)["v1.1.0-rc2"].startswith("newest release of 1.1")


def test_newest_line_keeps_everything_published_in_it():
    p = rr.plan(rels("v1.2.0-rc1", "v1.2.0-rc2", "v1.2.0", "v1.2.1-rc1"))
    assert p.unpublish == []


def test_drafts_and_non_tags_are_never_touched():
    p = rr.plan(rels("v1.0.0-rc1*", "nightly", "v1.1.0-rc1", "v1.2.0-rc1"))
    assert tags(p.unpublish) == []              # 1.1 keeps its newest release; the draft and "nightly" are ignored


def test_keep_tag_protects_a_hotfix_rc_in_an_older_line():
    p = rr.plan(rels("v1.1.0", "v1.1.1-rc1", "v1.2.0-rc1"), keep_tags=["v1.1.1-rc1"])
    assert p.unpublish == []
    p2 = rr.plan(rels("v1.1.0", "v1.1.1-rc1", "v1.2.0-rc1"))
    assert tags(p2.unpublish) == ["v1.1.1-rc1"]


def test_newest_stable_is_never_unpublished():
    p = rr.plan(rels("v1.0.0", "v2.0.0-rc1", "v2.0.0-rc2"))
    assert "v1.0.0" not in tags(p.unpublish)


def test_apply_refuses_more_than_max(capsys):
    class GH:
        patched = []

        def patch(self, path, body):
            self.patched.append(path)
    p = rr.plan(rels("v1.0.0-rc1", "v1.0.0-rc2", "v1.0.0-rc3", "v1.0.0", "v1.1.0-rc1"))
    assert len(p.unpublish) == 3
    gh = GH()
    assert rr.apply(gh, "o/r", p, max_changes=2) == 1
    assert gh.patched == []
    assert "> --max 2" in capsys.readouterr().out
    assert rr.apply(gh, "o/r", p, max_changes=10) == 0
    assert gh.patched == ["/repos/o/r/releases/1", "/repos/o/r/releases/2", "/repos/o/r/releases/3"]


def test_rc_numbers_order_by_value_not_text():
    p = rr.plan(rels("v1.1.0-rc9", "v1.1.0-rc10", "v1.2.0-rc1"))
    assert tags(p.unpublish) == ["v1.1.0-rc9"]
    assert [k for k, _ in p.keep] == ["v1.1.0-rc10", "v1.2.0-rc1"]


def test_nothing_published_plans_nothing():
    p = rr.plan(rels("v1.0.0*", "nightly"))
    assert p.keep == [] and p.unpublish == [] and p.newest_stable is None


class FakeGH:
    def __init__(self, releases):
        self.releases = releases
        self.patched = []

    def paginate(self, path):
        assert path == "/repos/o/r/releases"
        yield from self.releases

    def patch(self, path, body):
        self.patched.append((path, body))


@pytest.fixture
def gh(monkeypatch, tmp_path):
    fake = FakeGH(rels("v1.0.0", "v1.0.1", "v1.1.0-rc1"))
    monkeypatch.setattr(rr.gh_api, "from_env", lambda: fake)
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(tmp_path / "summary.md"))
    return fake


def test_main_without_apply_only_prints_the_plan(gh, tmp_path, capsys):
    assert rr.main(["--repo", "o/r"]) == 0
    assert gh.patched == []
    out = capsys.readouterr().out
    assert "dry run" in out
    assert "| v1.0.0 | unpublish to draft |" in out
    assert (tmp_path / "summary.md").read_text() == out[:-1]      # print() adds the final newline


def test_main_apply_sets_drafts_and_never_deletes(gh):
    assert rr.main(["--repo", "o/r", "--apply"]) == 0
    assert gh.patched == [("/repos/o/r/releases/1", {"draft": True})]


def test_main_apply_honours_max(gh):
    assert rr.main(["--repo", "o/r", "--apply", "--max", "0"]) == 1
    assert gh.patched == []
