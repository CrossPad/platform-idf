"""auto_tag.py: the bump-merge and green-run rules, the tag it creates, its input
validation; auto-tag.yml: the hardening of a job that holds the bot's private key."""
import json
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import auto_tag as at
import next_rc_tag

SHA = "a" * 40
BUMP = {"merged_at": "2026-10-11T10:00:00Z", "head": {"ref": "auto/app-bump"},
        "user": {"login": "crosspad-bot[bot]"}, "labels": [{"name": "auto-bump"}],
        "title": "chore(deps): core 1.53.0→1.60.0, slicer 0.3.1→0.4.0"}
RUN = {"event": "push", "head_branch": "main", "conclusion": "success"}


def test_bump_merge_detection():
    assert at.bump_merge([BUMP]) is BUMP
    assert at.bump_merge([dict(BUMP, merged_at=None)]) is None
    assert at.bump_merge([dict(BUMP, labels=[])]) is None
    assert at.bump_merge([dict(BUMP, user={"login": "matixan"})]) is None
    assert at.bump_merge([dict(BUMP, head={"ref": "feat"})]) is None
    assert at.bump_merge([]) is None


def test_should_tag_only_green_pushes_to_main():
    assert at.should_tag(RUN, manual=False)[0] is True
    assert at.should_tag(dict(RUN, conclusion="failure"), manual=False)[0] is False
    assert at.should_tag(dict(RUN, event="pull_request"), manual=False)[0] is False
    assert at.should_tag(dict(RUN, head_branch="feat"), manual=False)[0] is False
    assert at.should_tag(None, manual=True)[0] is True


def test_should_tag_without_a_run_is_a_no_not_a_crash():
    ok, why = at.should_tag(None, manual=False)
    assert ok is False and why


def test_tag_message_drops_the_prefix():
    assert at.tag_message(BUMP["title"]) == "CrossPad beta: core 1.53.0→1.60.0, slicer 0.3.1→0.4.0"


def test_tag_message_strips_control_characters():
    msg = at.tag_message("chore(deps): core 1.0.0→1.1.0\n\x1b[31mtagger x\x00\r\ty")
    assert msg == "CrossPad beta: core 1.0.0→1.1.0 [31mtagger x y"
    assert not any(ord(c) < 32 or 127 <= ord(c) < 160 for c in msg)


def test_create_tag_makes_an_annotated_tag_then_the_ref():
    calls = []

    class GH:
        def post(self, path, body):
            calls.append((path, body))
            return {"sha": "TAGOBJ"} if path.endswith("/git/tags") else {}
    at.create_tag(GH(), "o/r", "v1.1.0-rc3", "S" * 40, "msg")
    assert calls[0] == ("/repos/o/r/git/tags", {"tag": "v1.1.0-rc3", "message": "msg",
                                                "object": "S" * 40, "type": "commit"})
    assert calls[1] == ("/repos/o/r/git/refs", {"ref": "refs/tags/v1.1.0-rc3", "sha": "TAGOBJ"})


# --- main() ---------------------------------------------------------------

class FakeGH:
    def __init__(self, prs):
        self.prs, self.posts = prs, []

    def get(self, path):
        assert path == f"/repos/o/r/commits/{SHA}/pulls"
        return self.prs

    def post(self, path, body):
        self.posts.append((path, body))
        return {"sha": "TAGOBJ"}


@pytest.fixture
def env(tmp_path, monkeypatch):
    """AUTO_TAG on, a green push event on disk, git and the API faked."""
    monkeypatch.setenv("AUTO_TAG", "on")
    event = tmp_path / "event.json"
    event.write_text(json.dumps({"workflow_run": RUN}))
    gh = FakeGH([BUMP])
    tags = {"contains": "", "list": "v1.0.0\nv1.1.0-rc2\n"}

    def fake_git(*args):
        return tags["contains"] if args[:2] == ("tag", "--contains") else tags["list"]
    monkeypatch.setattr(at, "git", fake_git)
    monkeypatch.setattr(at.gh_api, "from_env", lambda name: gh)
    monkeypatch.setattr(at, "git_rc", lambda *a: subprocess.CompletedProcess(a, 0, "", ""))
    alerts = []
    monkeypatch.setattr(at.ci_alert, "send", lambda title, body: alerts.append((title, body)))
    return type("Env", (), {"event": str(event), "gh": gh, "tags": tags, "alerts": alerts})


def run_main(env, *extra):
    return at.main(["--repo", "o/r", "--sha", SHA, "--event", env.event, *extra])


@pytest.mark.parametrize("sha", ["", "abc", "A" * 40, "g" * 40, "a" * 39, "a" * 41, SHA + "\n", SHA + " x"])
def test_sha_must_be_forty_lowercase_hex(sha, capsys):
    assert at.main(["--repo", "o/r", "--sha", sha, "--manual"]) == 1
    assert capsys.readouterr().out.startswith("::error::")


def test_auto_tag_off_does_nothing(env, monkeypatch, capsys):
    monkeypatch.delenv("AUTO_TAG")
    assert run_main(env) == 0 and env.gh.posts == []
    assert "AUTO_TAG is not on" in capsys.readouterr().out


def test_tags_the_next_rc_with_the_bump_title(env, capsys):
    assert run_main(env) == 0
    assert env.gh.posts[0] == ("/repos/o/r/git/tags", {
        "tag": "v1.1.0-rc3", "message": "CrossPad beta: core 1.53.0→1.60.0, slicer 0.3.1→0.4.0",
        "object": SHA, "type": "commit"})
    assert env.gh.posts[1] == ("/repos/o/r/git/refs", {"ref": "refs/tags/v1.1.0-rc3", "sha": "TAGOBJ"})
    assert f"tagged v1.1.0-rc3 at {SHA}" in capsys.readouterr().out


def test_a_human_merge_is_not_tagged(env, capsys):
    env.gh.prs = [dict(BUMP, user={"login": "matixan"})]
    assert run_main(env) == 0 and env.gh.posts == []
    assert "not a bump merge" in capsys.readouterr().out


def test_a_red_run_is_not_tagged(env):
    Path(env.event).write_text(json.dumps({"workflow_run": dict(RUN, conclusion="failure")}))
    assert run_main(env) == 0 and env.gh.posts == []


def test_an_already_tagged_commit_is_skipped(env, capsys):
    env.tags["contains"] = "v1.1.0-rc2\n"
    assert run_main(env) == 0 and env.gh.posts == []
    assert "already points at this commit" in capsys.readouterr().out


def test_manual_skips_the_bump_rule(env):
    env.gh.prs = []
    assert at.main(["--repo", "o/r", "--sha", SHA, "--manual"]) == 0
    assert env.gh.posts[0][1]["message"] == f"CrossPad (manual auto-tag of {SHA[:12]})"


@pytest.mark.parametrize("extra", [(), ("--manual",)], ids=["workflow_run", "manual"])
def test_a_commit_off_main_is_refused_before_any_api_call(env, monkeypatch, capsys, extra):
    seen = []

    def not_ancestor(*a):
        seen.append(a)
        return subprocess.CompletedProcess(a, 1, "", "")
    monkeypatch.setattr(at, "git_rc", not_ancestor)
    monkeypatch.setattr(env.gh, "get", lambda path: pytest.fail("API called"))
    assert run_main(env, *extra) == 1 and env.gh.posts == []
    assert seen == [("merge-base", "--is-ancestor", SHA, "HEAD")]
    assert f"::error::{SHA} is not on main" in capsys.readouterr().out


def test_an_unknown_object_is_refused_too(env, monkeypatch, capsys):
    monkeypatch.setattr(at, "git_rc", lambda *a: subprocess.CompletedProcess(a, 128, "", "fatal: Not a valid commit name\n"))
    assert run_main(env, "--manual") == 1 and env.gh.posts == []
    assert "::error::cannot check" in capsys.readouterr().out


def test_auto_tag_off_skips_the_main_check(env, monkeypatch):
    monkeypatch.delenv("AUTO_TAG")
    monkeypatch.setattr(at, "git_rc", lambda *a: pytest.fail("git called"))
    assert run_main(env) == 0


def test_check_on_main_against_a_real_repo(tmp_path, monkeypatch):
    def g(*a):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *a], cwd=tmp_path, check=True,
                       capture_output=True, text=True)
    g("init", "-q", "-b", "main")
    g("commit", "-q", "--allow-empty", "-m", "one")
    g("checkout", "-q", "-b", "x")
    g("commit", "-q", "--allow-empty", "-m", "off main")
    off = subprocess.run(["git", "rev-parse", "HEAD"], cwd=tmp_path, capture_output=True, text=True).stdout.strip()
    g("checkout", "-q", "main")
    on = subprocess.run(["git", "rev-parse", "HEAD"], cwd=tmp_path, capture_output=True, text=True).stdout.strip()
    monkeypatch.chdir(tmp_path)
    assert at.check_on_main(on) is None
    assert at.check_on_main(off) == f"{off} is not on main"
    assert at.check_on_main("0" * 40).startswith("cannot check")


def test_no_candidate_alerts_and_fails(env, monkeypatch, capsys):
    def boom(names):
        raise next_rc_tag.NoCandidate("nothing sorts above")
    monkeypatch.setattr(at.next_rc_tag, "next_rc", boom)
    assert run_main(env) == 1 and env.gh.posts == []
    assert env.alerts == [("auto-tag: no candidate", "nothing sorts above")]
    assert "::error::nothing sorts above" in capsys.readouterr().out


def test_an_unknown_commit_is_an_error_not_a_traceback(env, monkeypatch, capsys):
    def fail(*args):
        raise subprocess.CalledProcessError(128, ["git", *args], stderr="fatal: malformed object name\n")
    monkeypatch.setattr(at, "git", fail)
    assert run_main(env) == 1 and env.gh.posts == []
    assert "::error::" in capsys.readouterr().out


# --- auto-tag.yml -----------------------------------------------------------

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "auto-tag.yml"
DOC = yaml.safe_load(WORKFLOW.read_text())
ON = DOC[True]  # PyYAML reads the bare key `on` as a boolean
JOB = DOC["jobs"]["tag"]
WATCH = DOC["jobs"]["watch"]
IMAGE = "ghcr.io/crosspad/crosspad-ci:v5.5.5-1@sha256:d89e2175ab2371eca90278a6a8774108b9387775344b2bbf837eabc2a30036bd"


def test_triggers_and_gate():
    assert ON["workflow_run"] == {"workflows": ["Build ESP-IDF Project"], "types": ["completed"],
                                  "branches": ["main"]}
    assert set(ON) == {"workflow_run", "workflow_dispatch"}
    assert list(ON["workflow_dispatch"]["inputs"]) == ["sha"] and ON["workflow_dispatch"]["inputs"]["sha"]["required"]
    assert JOB["if"].startswith("vars.AUTO_TAG == 'on' &&")
    dispatch, run = (part.strip() for part in JOB["if"].split("||", 1))
    assert "github.event_name == 'workflow_dispatch'" in dispatch
    assert "github.ref == 'refs/heads/main'" in dispatch   # a dispatch from a branch can tag only that branch's code
    assert "github.event.workflow_run.event == 'push'" in run
    assert "github.event.workflow_run.conclusion == 'success'" in run
    assert DOC["permissions"] == {"packages": "read", "contents": "read"}
    assert "concurrency" not in DOC and "concurrency" not in WATCH   # only the tagging job serialises
    assert JOB["concurrency"] == {"group": "auto-tag", "cancel-in-progress": False}
    assert "skips the bump-merge and green-run rules" in ON["workflow_dispatch"]["inputs"]["sha"]["description"]


def test_the_job_holding_the_bot_key_is_hardened():
    assert JOB["runs-on"] == "${{ vars.CI_SECRETS_RUNNER || 'ubuntu-latest' }}"
    assert JOB["timeout-minutes"] == 10
    assert JOB["container"]["image"] == IMAGE
    assert JOB["environment"] == "crosspad-bot"
    checkout = next(s for s in JOB["steps"] if s.get("uses", "").startswith("actions/checkout@"))
    assert checkout["with"] == {"fetch-depth": 0, "persist-credentials": False}


def test_token_scope():
    tok = next(s for s in JOB["steps"] if s.get("id") == "token")["with"]
    assert tok["repositories"] == "platform-idf"
    assert {k: v for k, v in tok.items() if k.startswith("permission-")} == {
        "permission-contents": "write", "permission-pull-requests": "read"}
    assert tok["app-id"] == "${{ vars.CROSSPAD_BOT_APP_ID }}"
    assert tok["private-key"] == "${{ secrets.CROSSPAD_BOT_PRIVATE_KEY }}"


def test_no_expression_reaches_a_shell_script():
    for job in (JOB, WATCH):
        runs = [s["run"] for s in job["steps"] if "run" in s]
        assert runs and not any("${{" in r for r in runs)
    env = next(s for s in JOB["steps"] if s.get("name") == "Tag")["env"]
    assert env["EVENT_NAME"] == "${{ github.event_name }}"
    assert env["SHA_INPUT"] == "${{ inputs.sha }}"
    assert env["RUN_SHA"] == "${{ github.event.workflow_run.head_sha }}"
    assert env["BOT_TOKEN"] == "${{ steps.token.outputs.token }}"


def test_the_tag_job_alerts_when_it_fails():
    last = JOB["steps"][-1]
    assert last["name"] == "Alert" and last["if"] == "failure()"
    assert last["env"]["DISCORD_CI_ALERTS_WEBHOOK"] == "${{ secrets.DISCORD_CI_ALERTS_WEBHOOK }}"
    assert last["env"]["SHA_INPUT"] == "${{ inputs.sha }}"
    assert last["env"]["RUN_SHA"] == "${{ github.event.workflow_run.head_sha }}"
    assert 'tools/ci_alert.py "auto-tag failed"' in last["run"]
    assert "${SHA_INPUT:-$RUN_SHA}" in last["run"]


def test_the_watch_job_holds_no_secret_but_the_webhook():
    cond = " ".join(WATCH["if"].split())
    assert "github.event_name == 'workflow_run'" in cond
    assert "github.event.workflow_run.event == 'push'" in cond
    assert "github.event.workflow_run.head_branch == 'main'" in cond
    for c in ("failure", "timed_out", "cancelled"):
        assert f"github.event.workflow_run.conclusion == '{c}'" in cond
    assert "success" not in cond
    assert "environment" not in WATCH
    assert WATCH["runs-on"] == "${{ vars.CI_RUNNER || 'ubuntu-latest' }}"
    assert WATCH["timeout-minutes"] == 5
    assert WATCH["container"]["image"] == IMAGE
    assert WATCH["permissions"] == {"packages": "read", "contents": "read", "pull-requests": "read", "actions": "read", "checks": "read"}
    checkout = next(s for s in WATCH["steps"] if s.get("uses", "").startswith("actions/checkout@"))
    assert checkout["with"] == {"persist-credentials": False}
    assert not any("create-github-app-token" in s.get("uses", "") for s in WATCH["steps"])
    step = next(s for s in WATCH["steps"] if s.get("name") == "Watch")
    assert step["env"] == {"GITHUB_TOKEN": "${{ github.token }}", "AUTO_TAG": "${{ vars.AUTO_TAG }}",
                           "DISCORD_CI_ALERTS_WEBHOOK": "${{ secrets.DISCORD_CI_ALERTS_WEBHOOK }}"}
    assert "--watch" in step["run"]


# --- watch ------------------------------------------------------------------

RED_RUN = dict(RUN, id=9, head_sha=SHA, conclusion="failure", html_url="https://github.com/o/r/actions/runs/9")


@pytest.mark.parametrize("auto_tag_on", [True, False])
@pytest.mark.parametrize("bump", [True, False])
@pytest.mark.parametrize("conclusion", ["failure", "timed_out"])
def test_a_red_main_run_alerts_whatever_the_rest(conclusion, bump, auto_tag_on):
    alert, _ = at.watch_decision(dict(RED_RUN, conclusion=conclusion), auto_tag_on, lambda: pytest.fail("API"),
                                 lambda: False)
    assert alert[0] == "main is red" and SHA in alert[1] and conclusion in alert[1]


@pytest.mark.parametrize("auto_tag_on, bump, alerts", [
    (True, True, True), (True, False, False), (False, True, False), (False, False, False)])
def test_a_cancelled_run_alerts_only_for_a_bump_merge_with_auto_tag_on(auto_tag_on, bump, alerts):
    calls = []
    alert, why = at.watch_decision(dict(RED_RUN, conclusion="cancelled"), auto_tag_on,
                                   lambda: calls.append(1) or bump, lambda: pytest.fail("quota"))
    assert (alert is not None) is alerts and why
    assert calls == ([1] if auto_tag_on else [])          # no API call when AUTO_TAG is off
    if alerts:
        assert alert[0] == "bump merge's main run was cancelled"
        assert "dispatch Auto tag with the newest main sha" in alert[1] and SHA in alert[1]


@pytest.mark.parametrize("run", [dict(RED_RUN, conclusion="success"), dict(RED_RUN, conclusion="skipped"),
                                 dict(RED_RUN, conclusion=None), dict(RED_RUN, event="pull_request"),
                                 dict(RED_RUN, head_branch="feat")])
def test_nothing_else_alerts(run):
    assert at.watch_decision(run, True, lambda: True, lambda: pytest.fail("quota"))[0] is None


def test_a_red_run_on_the_artifact_quota_alone_is_quiet():
    alert, why = at.watch_decision(RED_RUN, True, lambda: pytest.fail("API"), lambda: True)
    assert alert is None and "quota" in why


QUOTA_NOTE = {"annotation_level": "failure", "message": "Failed to CreateArtifact: Artifact storage quota has "
              "been hit. Unable to upload any new artifacts. Usage is recalculated every 6-12 hours."}
NODE_NOTE = {"annotation_level": "warning", "message": "Node.js 20 is deprecated."}
EXIT_NOTE = {"annotation_level": "failure", "message": "Process completed with exit code 2."}


class JobsGH:
    def __init__(self, jobs, notes):
        self.jobs, self.notes = jobs, notes

    def get(self, path):
        if path == "/repos/o/r/actions/runs/9/jobs?filter=latest&per_page=100":
            return {"jobs": self.jobs}
        prefix = "/repos/o/r/check-runs/"
        assert path.startswith(prefix) and path.endswith("/annotations"), path
        return self.notes[int(path[len(prefix):-len("/annotations")])]


@pytest.mark.parametrize("jobs, notes, quiet", [
    ([{"id": 1, "conclusion": "failure"}, {"id": 2, "conclusion": "success"}], {1: [NODE_NOTE, QUOTA_NOTE]}, True),
    ([{"id": 1, "conclusion": "failure"}, {"id": 2, "conclusion": "failure"}],
     {1: [QUOTA_NOTE], 2: [QUOTA_NOTE]}, True),
    ([{"id": 1, "conclusion": "failure"}, {"id": 2, "conclusion": "failure"}],
     {1: [QUOTA_NOTE], 2: [EXIT_NOTE]}, False),                     # a real failure beside the quota
    ([{"id": 1, "conclusion": "failure"}], {1: [QUOTA_NOTE, EXIT_NOTE]}, False),
    ([{"id": 1, "conclusion": "failure"}], {1: [NODE_NOTE]}, False),  # no failure annotation at all
    ([{"id": 1, "conclusion": "timed_out"}], {1: [EXIT_NOTE]}, False),
    ([{"id": 1, "conclusion": "success"}], {}, False),                # nothing failed: not explained
])
def test_quota_only(jobs, notes, quiet):
    assert at.quota_only(JobsGH(jobs, notes), "o/r", 9) is quiet


class WatchGH(FakeGH):
    """The pulls of FakeGH plus a red run's jobs: one that failed on a test, by default."""
    def __init__(self, prs):
        super().__init__(prs)
        self.runs = JobsGH([{"id": 1, "conclusion": "failure"}], {1: [EXIT_NOTE]})

    def get(self, path):
        if "/actions/runs/" in path or "/check-runs/" in path:
            return self.runs.get(path)
        return super().get(path)


@pytest.fixture
def wenv(tmp_path, monkeypatch):
    monkeypatch.setenv("AUTO_TAG", "on")
    monkeypatch.setattr(at, "git", lambda *a: pytest.fail("git called"))
    monkeypatch.setattr(at, "git_rc", lambda *a: pytest.fail("git called"))
    gh = WatchGH([BUMP])
    monkeypatch.setattr(at.gh_api, "from_env", lambda name: gh if name == "GITHUB_TOKEN" else pytest.fail(name))
    alerts = []
    monkeypatch.setattr(at.ci_alert, "send", lambda title, body: alerts.append((title, body)))
    event = tmp_path / "event.json"

    def run_watch(run):
        event.write_text(json.dumps({"workflow_run": run}))
        return at.main(["--repo", "o/r", "--event", str(event), "--watch"])
    return type("W", (), {"gh": gh, "alerts": alerts, "run": staticmethod(run_watch)})


def test_watch_alerts_on_a_red_run_even_with_auto_tag_off(wenv, monkeypatch):
    monkeypatch.delenv("AUTO_TAG")
    assert wenv.run(RED_RUN) == 0
    assert [a[0] for a in wenv.alerts] == ["main is red"] and wenv.gh.posts == []


def test_watch_is_quiet_for_a_run_red_on_the_artifact_quota(wenv, capsys):
    wenv.gh.runs.notes = {1: [NODE_NOTE, QUOTA_NOTE]}
    assert wenv.run(RED_RUN) == 0
    assert wenv.alerts == [] and "artifact storage quota" in capsys.readouterr().out


def test_watch_alerts_on_a_red_run_when_its_jobs_cannot_be_read(wenv, monkeypatch, capsys):
    def boom(path):
        raise at.gh_api.ApiError(403, "GET", path, "Resource not accessible by integration")
    monkeypatch.setattr(wenv.gh.runs, "get", boom)
    assert wenv.run(RED_RUN) == 0
    assert [a[0] for a in wenv.alerts] == ["main is red"] and "::warning::" in capsys.readouterr().out


def test_watch_alerts_on_a_red_run_without_a_run_id(wenv, monkeypatch):
    monkeypatch.setattr(wenv.gh.runs, "get", lambda path: pytest.fail("API called"))
    assert wenv.run({k: v for k, v in RED_RUN.items() if k != "id"}) == 0
    assert [a[0] for a in wenv.alerts] == ["main is red"]


def test_watch_alerts_on_a_cancelled_bump_merge(wenv):
    assert wenv.run(dict(RED_RUN, conclusion="cancelled")) == 0
    assert [a[0] for a in wenv.alerts] == ["bump merge's main run was cancelled"] and wenv.gh.posts == []


def test_watch_is_quiet_for_a_cancelled_human_merge(wenv, capsys):
    wenv.gh.prs = [dict(BUMP, user={"login": "matixan"})]
    assert wenv.run(dict(RED_RUN, conclusion="cancelled")) == 0
    assert wenv.alerts == [] and "not a bump merge" in capsys.readouterr().out


def test_watch_is_quiet_for_a_cancelled_run_with_auto_tag_off(wenv, monkeypatch, capsys):
    monkeypatch.delenv("AUTO_TAG")
    monkeypatch.setattr(wenv.gh, "get", lambda path: pytest.fail("API called"))
    assert wenv.run(dict(RED_RUN, conclusion="cancelled")) == 0
    assert wenv.alerts == [] and "AUTO_TAG is not on" in capsys.readouterr().out


def test_watch_alerts_when_the_pull_lookup_fails(wenv, monkeypatch):
    def boom(path):
        raise at.gh_api.ApiError(502, "GET", path, "bad gateway")
    monkeypatch.setattr(wenv.gh, "get", boom)
    assert wenv.run(dict(RED_RUN, conclusion="cancelled")) == 1
    assert [a[0] for a in wenv.alerts] == ["auto-tag watch failed"]


def test_watch_refuses_a_malformed_sha(wenv, capsys):
    assert wenv.run(dict(RED_RUN, head_sha="nope")) == 1 and wenv.alerts == []
    assert capsys.readouterr().out.startswith("::error::")


@pytest.mark.parametrize("extra", [["--sha", SHA], ["--manual"], []])
def test_watch_takes_only_repo_and_event(extra):
    with pytest.raises(SystemExit):
        at.main(["--repo", "o/r", "--watch", *(["--event", "x"] if extra else []), *extra])
