"""app-bump.yml: the token scope fails closed, and the triggers, guards and tokens
are the ones the spec asks for."""
import subprocess
from pathlib import Path

import pytest
import yaml

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "app-bump.yml"
DOC = yaml.safe_load(WORKFLOW.read_text())
ON = DOC[True]  # PyYAML reads the bare key `on` as a boolean
JOBS = DOC["jobs"]


def step(job: str, sid: str) -> dict:
    return next(s for s in JOBS[job]["steps"] if s.get("id") == sid)


def run_repos_step(tmp_path, tool: str) -> tuple[int, str]:
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "app_bump.py").write_text(tool)
    out = tmp_path / "out"
    out.write_text("")
    r = subprocess.run(["bash", "-e", "-c", step("reconcile", "repos")["run"]], cwd=tmp_path,
                       env={"PATH": "/usr/bin:/bin", "GITHUB_OUTPUT": str(out)}, capture_output=True, text=True)
    return r.returncode, out.read_text()


def test_repos_step_writes_the_list(tmp_path):
    code, out = run_repos_step(tmp_path, "print('slicer,beats')\n")
    assert code == 0 and out == "list=slicer,beats\n"


@pytest.mark.parametrize("tool", ["import sys; print('::error::x', file=sys.stderr); sys.exit(1)\n",
                                  "print('')\n", "pass\n"])
def test_repos_step_fails_closed(tmp_path, tool):
    code, out = run_repos_step(tmp_path, tool)
    assert code != 0 and out == ""


def test_triggers():
    assert ON["schedule"] == [{"cron": "17 * * * *"}]   # hourly, so it fires in ALERT_HOUR_UTC (05)
    assert ON["repository_dispatch"] == {"types": ["app-released"]}
    assert "workflow_dispatch" in ON and ON["workflow_dispatch"] is None   # no inputs
    assert ON["workflow_run"] == {"workflows": ["Build ESP-IDF Project"], "types": ["completed"],
                                  "branches": ["auto/app-bump"]}
    assert "push" not in ON
    assert "pull_request" not in ON and "pull_request_target" not in ON
    assert "client_payload" not in WORKFLOW.read_text()


def test_reconcile_runs_from_main_only_and_is_serialised():
    job = JOBS["reconcile"]
    assert "github.ref == 'refs/heads/main'" in job["if"]
    assert job["environment"] == "crosspad-bot"
    assert job["concurrency"] == {"group": "app-bump", "cancel-in-progress": False}


def test_tokens():
    read, write = step("reconcile", "read")["with"], step("reconcile", "write")["with"]
    assert read["repositories"] == "${{ steps.repos.outputs.list }}"
    assert {k for k in read if k.startswith("permission-")} == {"permission-contents"}
    assert read["permission-contents"] == "read"
    assert write["repositories"] == "platform-idf"
    # Only permissions the crosspad-bot installation has: asking for one it lacks (checks)
    # makes the mint fail with 422, and every reconcile run with it.
    assert {k: v for k, v in write.items() if k.startswith("permission-")} == {
        "permission-contents": "write", "permission-pull-requests": "write"}
    assert "permission-checks" not in WORKFLOW.read_text()
    for w in (read, write):
        assert w["app-id"] == "${{ vars.CROSSPAD_BOT_APP_ID }}"
        assert w["private-key"] == "${{ secrets.CROSSPAD_BOT_PRIVATE_KEY }}"


def test_check_runs_are_read_with_the_jobs_own_token():
    job = JOBS["reconcile"]
    assert job["permissions"] == {"contents": "read", "checks": "read"}
    env = next(s for s in job["steps"] if s.get("name") == "Reconcile")["env"]
    assert env["CHECKS_TOKEN"] == "${{ github.token }}"
    assert env["BUMP_TOKEN"] == "${{ steps.write.outputs.token }}"
    assert env["APP_READ_TOKEN"] == "${{ steps.read.outputs.token }}"


def test_every_job_has_a_timeout():
    for name, job in JOBS.items():
        assert job["timeout-minutes"] == 15, name


def test_header_says_a_foreign_pr_on_the_branch_stops_reconcile():
    header = WORKFLOW.read_text().split("\non:", 1)[0]
    assert "non-bot PR on auto/app-bump" in header and "until a human closes it" in header


def test_reporter_is_guarded():
    cond = " ".join(JOBS["report-failure"]["if"].split())
    for guard in ("github.event_name == 'workflow_run'", "head_branch == 'auto/app-bump'",
                  "head_repository.full_name == github.repository", "workflow_run.event == 'pull_request'",
                  "(github.event.workflow_run.conclusion == 'failure' || "
                  "github.event.workflow_run.conclusion == 'timed_out')"):
        assert guard in cond
    assert JOBS["report-sweep"]["if"] == "github.event_name == 'schedule' && github.ref == 'refs/heads/main'"


@pytest.mark.parametrize("name, args", [("report-failure", '--event "$GITHUB_EVENT_PATH"'),
                                        ("report-sweep", "sweep")])
def test_reporters_are_least_privileged_and_serialised(name, args):
    job = JOBS[name]
    assert job["permissions"] == {"contents": "read", "actions": "write", "pull-requests": "write"}
    assert "environment" not in job
    assert job["concurrency"] == {"group": "bump-report", "cancel-in-progress": False}
    assert job["runs-on"] == "${{ vars.CI_RUNNER || 'ubuntu-latest' }}"
    checkout = job["steps"][0]
    assert checkout["uses"].startswith("actions/checkout@") and checkout["with"] == {"persist-credentials": False}
    run = job["steps"][-1]
    assert run["env"]["GH_TOKEN"] == "${{ github.token }}"
    assert run["run"] == f'python3 tools/bump_report.py {args} --repo "$GITHUB_REPOSITORY"'


def test_every_action_is_pinned_to_a_sha():
    for job in JOBS.values():
        for s in job["steps"]:
            if "uses" in s:
                ref = s["uses"].split("@", 1)[1]
                assert len(ref) == 40 and all(c in "0123456789abcdef" for c in ref), s["uses"]


def test_the_job_holding_the_bot_key_runs_on_the_secrets_runner():
    for job in JOBS.values():
        mints = any("create-github-app-token" in s.get("uses", "") for s in job["steps"])
        assert ("CI_SECRETS_RUNNER" in job["runs-on"]) == mints
    assert JOBS["reconcile"]["runs-on"] == "${{ vars.CI_SECRETS_RUNNER || 'ubuntu-latest' }}"


def test_container_image_is_pinned_by_digest():
    for job in JOBS.values():
        image = job["container"]["image"]
        assert image.startswith("espressif/idf:v5.5.5@sha256:") and len(image.split("@sha256:")[1]) == 64
