import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_publish_release as cpr
import gh_api


class FakeGH:
    def __init__(self, releases):
        self.releases = releases
        self.patched = []

    def paginate(self, path):
        assert path.endswith("/releases")
        yield from self.releases

    def patch(self, path, body):
        self.patched.append((path, body))
        return {}


def out_dir(tmp_path, files):
    d = tmp_path / "out"
    d.mkdir()
    for name, size in files.items():
        (d / name).write_bytes(b"x" * size)
    return d


def rel(assets, draft=True):
    return {"id": 5, "tag_name": "v1.1.0-rc3", "draft": draft,
            "assets": [{"name": n, "size": s, "state": st} for n, s, st in assets]}


def test_find_release_finds_a_draft():
    gh = FakeGH([{"id": 1, "tag_name": "v1.1.0-rc2", "draft": False},
                 {"id": 2, "tag_name": "v1.1.0-rc3", "draft": True}])
    assert cpr.find_release(gh, "o/r", "v1.1.0-rc3")["id"] == 2
    assert cpr.find_release(gh, "o/r", "v9.9.9") is None


def test_verify_accepts_an_exact_upload(tmp_path):
    d = out_dir(tmp_path, {"a.bin": 3, "SHA256SUMS.txt": 2})
    assert cpr.verify(rel([("a.bin", 3, "uploaded"), ("SHA256SUMS.txt", 2, "uploaded")]), d) == []


def test_verify_names_missing_wrong_size_unfinished_and_extra(tmp_path):
    d = out_dir(tmp_path, {"a.bin": 3, "b.bin": 4, "c.bin": 1})
    problems = cpr.verify(rel([("a.bin", 2, "uploaded"), ("b.bin", 4, "starter"), ("z.bin", 1, "uploaded")]), d)
    assert "a.bin: 2 B on the release, 3 B here" in problems
    assert "b.bin: state starter" in problems
    assert "c.bin: not on the release" in problems
    assert "z.bin: on the release, not built by this run" in problems


def test_verify_requires_a_draft(tmp_path):
    d = out_dir(tmp_path, {"a.bin": 1})
    assert "the release is not a draft" in cpr.verify(rel([("a.bin", 1, "uploaded")], draft=False), d)


def test_publish_decision():
    assert cpr.publish_decision("v1.1.0-rc3", "on") == (True, "rc tag")
    assert cpr.publish_decision("v1.1.0-rc3", "")[0] is False
    assert cpr.publish_decision("v1.1.0", "on")[0] is False
    assert cpr.publish_decision("v1.2.0-test.1", "on")[0] is False


def test_publish_patches_draft_false_and_writes_output(tmp_path, monkeypatch):
    out = tmp_path / "out.txt"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    monkeypatch.setenv("AUTO_PUBLISH", "on")
    d = out_dir(tmp_path, {"a.bin": 3})
    gh = FakeGH([rel([("a.bin", 3, "uploaded")]) | {"id": 2}])
    assert cpr.publish(gh, "o/r", "v1.1.0-rc3", d) == 0
    assert gh.patched == [("/repos/o/r/releases/2", {"draft": False})]
    assert "published=true" in out.read_text()


def test_publish_leaves_stable_as_draft(tmp_path, monkeypatch):
    out = tmp_path / "out.txt"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    monkeypatch.setenv("AUTO_PUBLISH", "on")
    gh = FakeGH([{"id": 2, "tag_name": "v1.1.0", "draft": True}])
    assert cpr.publish(gh, "o/r", "v1.1.0", tmp_path) == 0
    assert gh.patched == []
    assert "published=false" in out.read_text()


@pytest.mark.parametrize("auto,tag", [("", "v1.1.0-rc3"), ("on", "v1.1.0")])
def test_declined_publish_tells_the_job_summary_how_to_publish(tmp_path, monkeypatch, auto, tag):
    summary = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    monkeypatch.setenv("AUTO_PUBLISH", auto)
    assert cpr.publish(FakeGH([]), "o/r", tag, tmp_path) == 0
    text = summary.read_text()
    assert "draft" in text and f"gh release edit {tag} -R o/r --draft=false" in text


def test_declined_publish_survives_an_unwritable_summary(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(tmp_path / "no" / "such" / "dir" / "s.md"))
    monkeypatch.setenv("AUTO_PUBLISH", "")
    assert cpr.publish(FakeGH([]), "o/r", "v1.1.0-rc3", tmp_path) == 0
    assert "not published" in capsys.readouterr().out


def test_verify_fails_when_nothing_was_built(tmp_path):
    d = out_dir(tmp_path, {})
    assert cpr.verify(rel([]), d) == ["no built assets to verify against"]


def _publish_env(tmp_path, monkeypatch):
    monkeypatch.setenv("GITHUB_OUTPUT", str(tmp_path / "out.txt"))
    monkeypatch.setenv("AUTO_PUBLISH", "on")
    return out_dir(tmp_path, {"a.bin": 3})


def test_publish_refuses_a_release_that_is_not_a_draft(tmp_path, monkeypatch):
    d = _publish_env(tmp_path, monkeypatch)
    gh = FakeGH([rel([("a.bin", 3, "uploaded")], draft=False)])
    assert cpr.publish(gh, "o/r", "v1.1.0-rc3", d) == 1
    assert gh.patched == []


def test_publish_fails_on_a_missing_tag(tmp_path, monkeypatch):
    d = _publish_env(tmp_path, monkeypatch)
    gh = FakeGH([{"id": 1, "tag_name": "v1.1.0-rc2", "draft": True}])
    assert cpr.publish(gh, "o/r", "v1.1.0-rc3", d) == 1
    assert gh.patched == []


@pytest.mark.parametrize("assets", [[], [("a.bin", 2, "uploaded")]])
def test_publish_does_not_patch_when_verify_finds_a_problem(tmp_path, monkeypatch, assets):
    d = _publish_env(tmp_path, monkeypatch)
    gh = FakeGH([rel(assets)])
    assert cpr.publish(gh, "o/r", "v1.1.0-rc3", d) == 1
    assert gh.patched == []


def test_main_verify_exits_1_on_a_mismatch(tmp_path, monkeypatch):
    d = out_dir(tmp_path, {"a.bin": 3})
    monkeypatch.setattr(gh_api, "from_env", lambda: FakeGH([rel([("a.bin", 2, "uploaded")])]))
    assert cpr.main(["verify", "--repo", "o/r", "--tag", "v1.1.0-rc3", "--out", str(d)]) == 1


def test_main_publish_exits_1_on_a_mismatch(tmp_path, monkeypatch):
    d = _publish_env(tmp_path, monkeypatch)
    gh = FakeGH([rel([("a.bin", 2, "uploaded")])])
    monkeypatch.setattr(gh_api, "from_env", lambda: gh)
    assert cpr.main(["publish", "--repo", "o/r", "--tag", "v1.1.0-rc3", "--out", str(d)]) == 1
    assert gh.patched == []


@pytest.mark.parametrize("cmd", ["verify", "publish"])
def test_main_without_out_is_a_usage_error(cmd, capsys):
    with pytest.raises(SystemExit) as e:
        cpr.main([cmd, "--repo", "o/r", "--tag", "v1.1.0-rc3"])
    assert e.value.code == 2
    assert "--out is required" in capsys.readouterr().err
