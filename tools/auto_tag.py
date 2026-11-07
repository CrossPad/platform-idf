#!/usr/bin/env python3
"""Tag the next -rcN on a bump merge whose post-merge main.yml run is green.

Rules (spec, "Exact rules: Auto-tag"): the commit is on main (an ancestor of HEAD); the run is a push to main that
succeeded; the commit is a merged auto/app-bump PR by crosspad-bot[bot] with
label auto-bump; no v* tag points at the commit or at a descendant of it;
next_rc_tag finds a valid candidate. --manual skips the green-run and the
bump-merge rules only: the commit must still be on main and untagged. The tag is
created through the API with the bot token, so it starts main.yml's tag build.

  auto_tag.py --repo R --sha S [--event $GITHUB_EVENT_PATH] [--manual]

--watch (the Auto tag workflow's watch job, no bot token) tags nothing: it reads
the workflow_run event and alerts when a push run on main ended red, or when a
bump merge's run was cancelled (that commit is never tagged by itself). A run red
only because GitHub refused its artifact uploads (storage quota) is not alerted:
the build passed, and the quota frees itself within hours.

  auto_tag.py --repo R --event $GITHUB_EVENT_PATH --watch
"""
import argparse
import json
import os
import re
import subprocess
import sys

import app_bump
import ci_alert
import gh_api
import next_rc_tag

SHA = re.compile(r"[0-9a-f]{40}")
TITLE_PREFIX = "chore(deps): "
CONTROL = re.compile(r"[\x00-\x1f\x7f-\x9f]+")


def bump_merge(prs: list[dict]) -> dict | None:
    for pr in prs:
        if (pr.get("merged_at") and pr["head"]["ref"] == app_bump.BRANCH
                and pr["user"]["login"] == app_bump.BOT
                and any(l["name"] == "auto-bump" for l in pr.get("labels", []))):
            return pr
    return None


def should_tag(run: dict | None, manual: bool) -> tuple[bool, str]:
    if manual:
        return True, "manual"
    if run is None:
        return False, "no workflow_run event to judge"
    if run["event"] != "push" or run["head_branch"] != "main":
        return False, f"run is {run['event']} on {run['head_branch']}"
    if run["conclusion"] != "success":
        return False, f"run concluded {run['conclusion']}"
    return True, "green push to main"


RED = ("failure", "timed_out")
QUOTA = "Artifact storage quota has been hit"


def quota_only(gh, repo: str, run_id: int) -> bool:
    """True when every failed job of the run failed on the artifact storage quota
    and on nothing else (its failure annotations all say so)."""
    jobs = gh.get(f"/repos/{repo}/actions/runs/{run_id}/jobs?filter=latest&per_page=100")["jobs"]
    failed = [j for j in jobs if j.get("conclusion") in RED]
    if not failed:
        return False
    for job in failed:
        notes = gh.get(f"/repos/{repo}/check-runs/{job['id']}/annotations")
        errors = [a.get("message", "") for a in notes if a.get("annotation_level") == "failure"]
        if not errors or not all(QUOTA in m for m in errors):
            return False
    return True


def watch_decision(run: dict, auto_tag_on: bool, is_bump, is_quota) -> tuple[tuple[str, str] | None, str]:
    """(alert (title, body) or None, why). is_bump is called only for a cancelled run
    with AUTO_TAG on, is_quota only for a red one: each costs API calls."""
    sha, conclusion = run.get("head_sha", ""), run.get("conclusion")
    if run.get("event") != "push" or run.get("head_branch") != "main":
        return None, f"run is {run.get('event')} on {run.get('head_branch')}"
    detail = f"{run['html_url']}\n" if run.get("html_url") else ""
    if conclusion in RED:
        if is_quota():
            return None, "red only on the artifact storage quota"
        return ("main is red", f"{detail}main.yml on {sha} concluded {conclusion}"), "red"
    if conclusion != "cancelled":
        return None, f"run concluded {conclusion}"
    if not auto_tag_on:
        return None, "run was cancelled and AUTO_TAG is not on"
    if not is_bump():
        return None, "run was cancelled, but the commit is not a bump merge"
    return ("bump merge's main run was cancelled",
            f"{detail}{sha} is a bump merge and will not be tagged by itself; "
            "dispatch Auto tag with the newest main sha"), "cancelled bump merge"


def watch(repo: str, event_path: str) -> int:
    with open(event_path, encoding="utf-8") as f:
        run = json.load(f)["workflow_run"]
    sha = run.get("head_sha", "")
    if not SHA.fullmatch(sha):
        print("::error::workflow_run.head_sha must be 40 lowercase hex characters")
        return 1

    def is_bump() -> bool:
        gh = gh_api.from_env("GITHUB_TOKEN")
        return bump_merge(gh.get(f"/repos/{repo}/commits/{sha}/pulls")) is not None

    def is_quota() -> bool:
        # Unreadable jobs alert as before: a quiet red main is worse than a noisy one.
        if not isinstance(run.get("id"), int):
            return False
        try:
            return quota_only(gh_api.from_env("GITHUB_TOKEN"), repo, run["id"])
        except gh_api.ApiError as e:
            print(f"::warning::could not read the run's jobs: {e}")
            return False
    try:
        alert, why = watch_decision(run, os.environ.get("AUTO_TAG") == "on", is_bump, is_quota)
    except gh_api.ApiError as e:
        ci_alert.send("auto-tag watch failed", f"{sha}: could not tell whether the cancelled run was a bump merge: {e}")
        print(f"::error::{e}")
        return 1
    if alert is None:
        print(f"no alert: {why}")
        return 0
    ci_alert.send(*alert)
    return 0


def tag_message(title: str) -> str:
    """The bump PR's title without its prefix and without control characters."""
    return f"CrossPad beta: {CONTROL.sub(' ', title).strip().removeprefix(TITLE_PREFIX)}"


def create_tag(gh, repo: str, tag: str, sha: str, message: str) -> None:
    obj = gh.post(f"/repos/{repo}/git/tags", {"tag": tag, "message": message, "object": sha, "type": "commit"})
    gh.post(f"/repos/{repo}/git/refs", {"ref": f"refs/tags/{tag}", "sha": obj["sha"]})


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def git_rc(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], capture_output=True, text=True)


def check_on_main(sha: str) -> str | None:
    """None when sha is an ancestor of HEAD (main on both trigger paths), else the error.
    fetch-depth 0 fetches every branch: a commit that merely exists locally may be
    on any of them, and a v* tag is permanent."""
    r = git_rc("merge-base", "--is-ancestor", sha, "HEAD")
    if r.returncode == 0:
        return None
    if r.returncode == 1:
        return f"{sha} is not on main"
    return f"cannot check {sha[:12]} against main: {r.stderr.strip()[:200] or 'git exit ' + str(r.returncode)}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--sha")
    ap.add_argument("--event")
    ap.add_argument("--manual", action="store_true")
    ap.add_argument("--watch", action="store_true")
    ns = ap.parse_args(argv)
    if ns.watch:
        if not ns.event or ns.sha or ns.manual:
            ap.error("--watch takes --repo and --event only")
        return watch(ns.repo, ns.event)
    if ns.sha is None:
        ap.error("--sha is required")
    if not SHA.fullmatch(ns.sha):
        print("::error::--sha must be 40 lowercase hex characters")
        return 1
    if os.environ.get("AUTO_TAG") != "on":
        print("AUTO_TAG is not on")
        return 0
    err = check_on_main(ns.sha)
    if err:
        print(f"::error::{err}")
        return 1
    run = None
    if ns.event and not ns.manual:
        with open(ns.event, encoding="utf-8") as f:
            run = json.load(f)["workflow_run"]
    ok, why = should_tag(run, ns.manual)
    if not ok:
        print(f"no tag: {why}")
        return 0
    gh = gh_api.from_env("BOT_TOKEN")
    message = f"CrossPad (manual auto-tag of {ns.sha[:12]})"
    if not ns.manual:
        pr = bump_merge(gh.get(f"/repos/{ns.repo}/commits/{ns.sha}/pulls"))
        if pr is None:
            print("no tag: not a bump merge (human merges ride along in the next rc)")
            return 0
        message = tag_message(pr["title"])
    try:
        later = git("tag", "--contains", ns.sha, "--list", "v*").split()
    except subprocess.CalledProcessError as e:
        print(f"::error::git tag --contains {ns.sha[:12]} failed: {e.stderr.strip()[:200]}")
        return 1
    if later:
        print("no tag: a v* tag already points at this commit or a later one")
        return 0
    try:
        tag = next_rc_tag.next_rc(git("tag", "-l", "v*").split())
    except next_rc_tag.NoCandidate as e:
        ci_alert.send("auto-tag: no candidate", str(e))
        print(f"::error::{e}")
        return 1
    create_tag(gh, ns.repo, tag, ns.sha, message)
    print(f"tagged {tag} at {ns.sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
