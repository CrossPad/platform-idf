import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bump_report as br

RUN = {"id": 9, "event": "pull_request", "conclusion": "failure", "status": "completed",
       "head_branch": "auto/app-bump", "run_attempt": 1, "head_sha": "abc",
       "updated_at": "2026-10-11T10:00:00Z",
       "repository": {"full_name": "o/r"}, "head_repository": {"full_name": "o/r"},
       "pull_requests": [{"number": 7, "head": {"ref": "auto/app-bump"}}]}
PR = {"number": 7, "state": "open", "head": {"ref": "auto/app-bump", "sha": "abc", "repo": {"full_name": "o/r"}},
      "base": {"ref": "main"}, "user": {"login": "crosspad-bot[bot]"}}
JOBS = [{"name": "build (v2)", "conclusion": "failure", "html_url": "u2"},
        {"name": "build (v1)", "conclusion": "success", "html_url": "u1"},
        {"name": "build (v3)", "conclusion": "timed_out", "html_url": "u3"}]


class GH:
    """A stateful fake: posted comments are read back, so dedupe works across calls."""

    def __init__(self, comments=(), prs=(PR,), runs=(RUN,), jobs=JOBS):
        self.comments, self.prs, self.runs, self.jobs = list(comments), list(prs), list(runs), list(jobs)
        self.calls = []

    def paginate(self, path):
        assert path.startswith("/") and path.endswith("/comments"), path
        yield from list(self.comments)

    def get(self, path):
        self.calls.append(("GET", path, None))
        assert path.startswith("/"), path
        if "/pulls?" in path:
            return self.prs
        if "/pulls/" in path:
            return next(p for p in self.prs if path.endswith(f"/pulls/{p['number']}"))
        if "/actions/workflows/main.yml/runs?" in path:
            return {"total_count": len(self.runs), "workflow_runs": self.runs}
        if "/jobs?" in path:
            page = int(path.rsplit("page=", 1)[1])
            return {"total_count": len(self.jobs), "jobs": self.jobs[(page - 1) * 100:page * 100]}
        raise AssertionError(path)

    def post(self, path, body):
        self.calls.append(("POST", path, body))
        if path.endswith("/comments"):
            self.comments.append(body)

    def writes(self):
        return [c for c in self.calls if c[0] == "POST"]


@pytest.fixture
def sent(monkeypatch):
    out = []
    monkeypatch.setattr(br.ci_alert, "send", lambda t, b: out.append((t, b)))
    monkeypatch.setattr(br, "_now", lambda: br._iso("2026-10-11T11:00:00Z"))
    return out


def test_first_failure_reruns():
    assert br.action(RUN) == "rerun"


def test_second_failure_reports():
    assert br.action(dict(RUN, run_attempt=2)) == "report"


def test_timed_out_is_a_failure():
    assert br.action(dict(RUN, conclusion="timed_out")) == "rerun"
    assert br.action(dict(RUN, conclusion="timed_out", run_attempt=2)) == "report"


def test_other_runs_are_ignored():
    assert br.action(dict(RUN, head_branch="feat/x")) == "ignore"
    assert br.action(dict(RUN, conclusion="success")) == "ignore"
    assert br.action(dict(RUN, conclusion="cancelled")) == "ignore"
    assert br.action(dict(RUN, event="push")) == "ignore"


def test_fork_or_unknown_head_repo_is_ignored():
    assert br.action(dict(RUN, head_repository={"full_name": "evil/r"})) == "ignore"
    assert br.action(dict(RUN, head_repository=None)) == "ignore"
    assert br.action({k: v for k, v in RUN.items() if k != "repository"}) == "ignore"


def test_run_without_the_bump_pr_is_ignored():
    assert br.action(dict(RUN, pull_requests=[])) == "ignore"
    assert br.action(dict(RUN, pull_requests=[{"number": 8, "head": {"ref": "feat/x"}}])) == "ignore"


def test_report_labels_comments_and_alerts_once(sent):
    gh = GH()
    br.report(gh, "o/r", dict(RUN, run_attempt=2))
    assert ("POST", "/repos/o/r/issues/7/labels", {"labels": ["bump-failed"]}) in gh.calls
    body = [c for c in gh.writes() if c[1].endswith("/comments")][0][2]["body"]
    assert "build (v2)" in body and "build (v3)" in body and "build (v1)" not in body
    assert br.MARKER("abc") in body
    assert len(sent) == 1
    br.report(gh, "o/r", dict(RUN, run_attempt=3))
    assert len(sent) == 1                       # same head sha: no second alert or comment
    assert len([c for c in gh.writes() if c[1].endswith("/comments")]) == 1


def test_report_on_a_new_head_sha_alerts_again(sent):
    br.report(GH([{"body": br.MARKER("abc")}]), "o/r", dict(RUN, run_attempt=2, head_sha="def"))
    assert len(sent) == 1


def test_report_reads_every_page_of_jobs(sent):
    jobs = [{"name": f"ok {i}", "conclusion": "success", "html_url": "u"} for i in range(150)]
    gh = GH(jobs=jobs + [{"name": "late (v2)", "conclusion": "failure", "html_url": "u"}])
    br.report(gh, "o/r", dict(RUN, run_attempt=2))
    assert "late (v2)" in gh.comments[0]["body"]


def _main(monkeypatch, tmp_path, run, gh=None, repo="o/r"):
    gh = gh or GH()
    monkeypatch.setattr(br.gh_api, "from_env", lambda: gh)
    event = tmp_path / "event.json"
    event.write_text(json.dumps({"workflow_run": run, "client_payload": {"ignored": True}}))
    assert br.main(["--event", str(event), "--repo", repo]) == 0
    return gh


def test_main_reruns_the_failed_jobs_once(monkeypatch, tmp_path, sent):
    gh = _main(monkeypatch, tmp_path, RUN)
    assert gh.writes() == [("POST", "/repos/o/r/actions/runs/9/rerun-failed-jobs", {})]
    assert sent == []


def test_main_reports_the_second_failure(monkeypatch, tmp_path, sent):
    gh = _main(monkeypatch, tmp_path, dict(RUN, run_attempt=2))
    assert ("POST", "/repos/o/r/issues/7/labels", {"labels": ["bump-failed"]}) in gh.calls
    assert len(sent) == 1


@pytest.mark.parametrize("pr", [dict(PR, head=dict(PR["head"], sha="newer")), dict(PR, state="closed"),
                                dict(PR, user={"login": "someone"}),
                                dict(PR, head=dict(PR["head"], repo={"full_name": "evil/r"})),
                                dict(PR, head=dict(PR["head"], repo=None))])
def test_main_ignores_a_run_of_a_superseded_head_or_another_pr(monkeypatch, tmp_path, sent, pr):
    for run in (RUN, dict(RUN, run_attempt=2)):
        gh = _main(monkeypatch, tmp_path, run, gh=GH(prs=[pr]))
        assert gh.writes() == [] and sent == []


def test_main_ignores_a_run_of_another_repo(monkeypatch, tmp_path, sent):
    gh = _main(monkeypatch, tmp_path, RUN, repo="x/y")
    assert gh.calls == [] and sent == []


def test_main_ignores_other_runs_without_calling_github(monkeypatch, tmp_path, sent):
    gh = _main(monkeypatch, tmp_path, dict(RUN, conclusion="success"))
    assert gh.calls == [] and sent == []


# --- sweep: the schedule's safety net for a workflow_run that never fired ---

def test_sweep_reruns_a_missed_first_failure(sent):
    gh = GH()
    br.sweep(gh, "o/r")
    assert gh.writes() == [("POST", "/repos/o/r/actions/runs/9/rerun-failed-jobs", {})]
    assert ("GET", "/repos/o/r/pulls?head=o:auto/app-bump&base=main&state=open", None) in gh.calls
    assert ("GET", "/repos/o/r/actions/workflows/main.yml/runs?head_sha=abc&event=pull_request", None) in gh.calls
    assert sent == []


def test_sweep_leaves_a_fresh_first_failure_to_the_fast_path(sent):
    gh = GH(runs=[dict(RUN, updated_at="2026-10-11T10:50:00Z")])
    br.sweep(gh, "o/r")
    assert gh.writes() == [] and sent == []


def test_sweep_handles_timed_out(sent):
    gh = GH(runs=[dict(RUN, conclusion="timed_out")])
    br.sweep(gh, "o/r")
    assert gh.writes() == [("POST", "/repos/o/r/actions/runs/9/rerun-failed-jobs", {})]
    gh = GH(runs=[dict(RUN, conclusion="timed_out", run_attempt=2)])
    br.sweep(gh, "o/r")
    assert len(sent) == 1


def test_attempt_two_is_reported_once_across_fast_path_and_sweep(monkeypatch, tmp_path, sent):
    run = dict(RUN, run_attempt=2)
    gh = GH(runs=[run])
    _main(monkeypatch, tmp_path, run, gh=gh)
    br.sweep(gh, "o/r")
    br.sweep(gh, "o/r")
    assert len(sent) == 1
    assert len([c for c in gh.writes() if c[1].endswith("/comments")]) == 1
    assert not [c for c in gh.writes() if c[1].endswith("rerun-failed-jobs")]


def test_sweep_reports_a_missed_second_failure(sent):
    gh = GH(runs=[dict(RUN, run_attempt=2)])
    br.sweep(gh, "o/r")
    assert len(sent) == 1 and ("POST", "/repos/o/r/issues/7/labels", {"labels": ["bump-failed"]}) in gh.calls


def test_sweep_picks_the_newest_run():
    old = dict(RUN, id=1, created_at="2026-10-11T09:00:00Z", conclusion="success")
    new = dict(RUN, id=2, created_at="2026-10-11T09:30:00Z")
    assert br._newest([old, new])["id"] == 2 and br._newest([new, old])["id"] == 2


@pytest.mark.parametrize("runs", [[dict(RUN, status="in_progress", conclusion=None)],
                                  [dict(RUN, status="queued", conclusion=None, run_attempt=2)],
                                  [dict(RUN, conclusion="success")], [dict(RUN, conclusion="cancelled")], []])
def test_sweep_does_nothing_without_a_finished_failure(sent, runs):
    gh = GH(runs=runs)
    br.sweep(gh, "o/r")
    assert gh.writes() == [] and sent == []


@pytest.mark.parametrize("prs", [[], [dict(PR, user={"login": "someone"})], [dict(PR, base={"ref": "dev"})],
                                 [dict(PR, head=dict(PR["head"], repo={"full_name": "evil/r"}))]])
def test_sweep_does_nothing_without_the_bot_pr(sent, prs):
    gh = GH(prs=prs)
    br.sweep(gh, "o/r")
    assert gh.writes() == [] and sent == []
    assert not [c for c in gh.calls if "/actions/" in c[1]]


def test_sweep_cli(monkeypatch, sent):
    gh = GH()
    monkeypatch.setattr(br.gh_api, "from_env", lambda: gh)
    assert br.main(["sweep", "--repo", "o/r"]) == 0
    assert gh.writes() == [("POST", "/repos/o/r/actions/runs/9/rerun-failed-jobs", {})]


FORK_RUN = dict(RUN, id=66, created_at="2026-10-11T09:59:00Z", run_attempt=2,
                head_repository={"full_name": "evil/r"})


@pytest.mark.parametrize("foreign", [FORK_RUN, dict(FORK_RUN, head_repository={"full_name": "o/r"}, head_branch="feat/x"),
                                     dict(FORK_RUN, head_repository={"full_name": "o/r"}, event="push"),
                                     dict(FORK_RUN, head_repository=None)])
def test_sweep_skips_foreign_runs_on_the_same_sha(sent, foreign):
    real = dict(RUN, created_at="2026-10-11T09:00:00Z")
    gh = GH(runs=[foreign, real])
    br.sweep(gh, "o/r")
    assert gh.writes() == [("POST", "/repos/o/r/actions/runs/9/rerun-failed-jobs", {})]   # the real one
    assert sent == []
    gh = GH(runs=[foreign])
    br.sweep(gh, "o/r")
    assert gh.writes() == [] and sent == []


def test_fast_path_and_sweep_share_one_run_guard():
    assert br._own_bump_run(RUN, "o/r")
    assert not br._own_bump_run(RUN, "x/y")
    assert not br._own_bump_run(FORK_RUN, "o/r")
    assert br.action(FORK_RUN) == "ignore"


HOSTILE = ("cell\x1b[31m @everyone @here [click](https://evil.example/x) <https://e.example> `c` *b* "
           "|| s || # h\n\r\t" + "a" * 300)


def test_job_names_are_sanitised(sent):
    gh = GH(jobs=[{"name": HOSTILE, "conclusion": "failure", "html_url": "https://github.com/o/r/actions/runs/9/job/1"}])
    br.report(gh, "o/r", dict(RUN, run_attempt=2))
    comment = gh.comments[0]["body"]
    (title, body), = sent
    for text in (comment, body):
        assert "\x1b" not in text and "\r" not in text and "\t" not in text
        assert "@everyone" not in text and "@here" not in text
        assert "[click](" not in text and "https://evil" not in text and "<https://e" not in text
        assert "`c`" not in text
    name = br._clean(HOSTILE)
    assert len(name) <= 2 * br.NAME_MAX and "\n" not in name
    assert comment.count("](https://github.com/o/r/actions/runs/9/job/1)") == 1   # our own link survives
    assert br._clean("build (v2, full)") == "build (v2, full)"
