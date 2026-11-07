#!/usr/bin/env python3
"""A red main.yml run on the bump PR: re-run the failed jobs once (a runner can be
flaky), then label bump-failed, comment the failed cells and alert #ci-alerts,
once per head commit.

Fast path: the workflow_run event of the failed run. Safety net: `sweep`, from
the hourly schedule, looks at the newest main.yml run of the bot PR's head and
does what the fast path would have done (a re-run started with GITHUB_TOKEN may
never fire workflow_run again). Both act only on this repo's own auto/app-bump
PR, read nothing but the payload and the API, and never touch the PR's code.

  bump_report.py --event $GITHUB_EVENT_PATH --repo R
  bump_report.py sweep --repo R
"""
import argparse
import calendar
import json
import sys
import time
import unicodedata

import app_bump
import ci_alert
import gh_api

FAILED = ("failure", "timed_out")
SWEEP_RERUN_AFTER_S = 15 * 60  # a younger first failure is the fast path's
NAME_MAX = 100  # a job name, before escaping
# Job names come from the PR's own workflow YAML: no markdown, links or mentions.
_MARKDOWN = str.maketrans({c: "\\" + c for c in "\\[]<>`*_~|#"})


def MARKER(sha: str) -> str:
    return f"<!-- bump-failed:{sha} -->"


def _now() -> float:
    return time.time()


def _iso(text: str) -> float:
    return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%SZ"))


def _full_name(repo) -> str | None:
    return repo.get("full_name") if isinstance(repo, dict) else None


def _bump_pr(run: dict) -> int | None:
    for pr in run.get("pull_requests") or []:
        if (pr.get("head") or {}).get("ref") == app_bump.BRANCH:
            return pr["number"]
    return None


def _clean(name) -> str:
    """A job name as plain text: no control characters, no markdown, no live link or mention."""
    text = "".join(ch for ch in str(name) if not unicodedata.category(ch).startswith("C"))
    if len(text) > NAME_MAX:
        text = text[:NAME_MAX] + "..."
    return text.translate(_MARKDOWN).replace("@", "@\u200b").replace("://", ":\u200b//")


def _is_bot_pr(pr: dict, repo: str) -> bool:
    head = pr.get("head") or {}
    return (pr.get("state") == "open" and (pr.get("user") or {}).get("login") == app_bump.BOT
            and head.get("ref") == app_bump.BRANCH and _full_name(head.get("repo")) == repo
            and (pr.get("base") or {}).get("ref") == "main")


def _own_bump_run(run: dict, repo: str) -> bool:
    """A pull_request run of `repo`'s own bump branch: never a fork's branch of the same name
    or a fork PR on the same commit. Shared by the fast path and the sweep."""
    return (run.get("event") == "pull_request" and run.get("head_branch") == app_bump.BRANCH
            and _full_name(run.get("head_repository")) == repo)


def _next_step(run: dict) -> str:
    return "rerun" if run.get("run_attempt", 1) == 1 else "report"


def action(run: dict) -> str:
    base = _full_name(run.get("repository"))
    if (not base or not _own_bump_run(run, base) or run.get("conclusion") not in FAILED
            or _bump_pr(run) is None):
        return "ignore"
    return _next_step(run)


def _failed_jobs(gh, repo: str, run_id) -> list[dict]:
    """The jobs endpoint answers an object, not a list: page it by hand."""
    jobs, page = [], 1
    while True:
        batch = gh.get(f"/repos/{repo}/actions/runs/{run_id}/jobs?per_page=100&page={page}").get("jobs") or []
        jobs += batch
        if len(batch) < 100:
            return [j for j in jobs if j.get("conclusion") in FAILED]
        page += 1


def report(gh, repo: str, run: dict, number: int | None = None) -> None:
    number = number if number is not None else _bump_pr(run)
    gh.post(f"/repos/{repo}/issues/{number}/labels", {"labels": ["bump-failed"]})
    if any(MARKER(run["head_sha"]) in (c.get("body") or "")
           for c in gh.paginate(f"/repos/{repo}/issues/{number}/comments")):
        print(f"#{number}: already reported for {run['head_sha'][:12]}")
        return
    failed = _failed_jobs(gh, repo, run["id"])
    names = [_clean(j.get("name")) for j in failed]
    lines = "\n".join(f"- [{n}]({j['html_url']})" for n, j in zip(names, failed)) or "- (no failed job listed)"
    gh.post(f"/repos/{repo}/issues/{number}/comments",
            {"body": f"Bump build failed after a re-run:\n\n{lines}\n\nFix the app (a new version "
                     f"supersedes this pin), hold it with APP_BUMP_HOLD, or fix main.\n\n{MARKER(run['head_sha'])}"})
    ci_alert.send(f"bump PR #{number} red",
                  "\n".join(names + [run.get("html_url") or ""]).strip())


def rerun(gh, repo: str, run: dict) -> None:
    gh.post(f"/repos/{repo}/actions/runs/{run['id']}/rerun-failed-jobs", {})
    print(f"re-running the failed jobs of run {run['id']}")


def _newest(runs: list[dict]) -> dict:
    return max(runs, key=lambda r: (r.get("created_at") or "", r.get("id") or 0))


def sweep(gh, repo: str) -> None:
    owner = repo.split("/")[0]
    prs = [p for p in gh.get(f"/repos/{repo}/pulls?head={owner}:{app_bump.BRANCH}&base=main&state=open") or []
           if _is_bot_pr(p, repo)]
    if not prs:
        print("no open bump PR")
        return
    pr = prs[0]
    sha = pr["head"]["sha"]
    runs = gh.get(f"/repos/{repo}/actions/workflows/main.yml/runs?head_sha={sha}&event=pull_request")
    runs = [r for r in (runs or {}).get("workflow_runs") or [] if _own_bump_run(r, repo)]
    if not runs:
        print(f"#{pr['number']}: no main.yml run for {sha[:12]} yet")
        return
    run = _newest(runs)
    if run.get("status") != "completed" or run.get("conclusion") not in FAILED:
        print(f"#{pr['number']}: run {run['id']} is {run.get('status')}/{run.get('conclusion')}; nothing to do")
        return
    if _next_step(run) == "report":
        report(gh, repo, run, pr["number"])
    elif _now() - _iso(run["updated_at"]) > SWEEP_RERUN_AFTER_S:
        rerun(gh, repo, run)
    else:
        print(f"#{pr['number']}: run {run['id']} failed under {SWEEP_RERUN_AFTER_S // 60} min ago; "
              f"left to the workflow_run path")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", choices=["sweep"])
    ap.add_argument("--event")
    ap.add_argument("--repo", required=True)
    ns = ap.parse_args(argv)
    if ns.cmd == "sweep":
        sweep(gh_api.from_env(), ns.repo)
        return 0
    if not ns.event:
        ap.error("--event is required without sweep")
    with open(ns.event) as f:
        run = json.load(f)["workflow_run"]
    what = action(run) if _full_name(run.get("repository")) == ns.repo else "ignore"
    if what == "ignore":
        print("not a failed bump PR run")
        return 0
    gh = gh_api.from_env()
    number = _bump_pr(run)
    pr = gh.get(f"/repos/{ns.repo}/pulls/{number}")
    if not _is_bot_pr(pr, ns.repo) or pr["head"]["sha"] != run["head_sha"]:
        print(f"#{number}: run {run['id']} is not of the bot PR's current head; ignored")
        return 0
    if what == "rerun":
        rerun(gh, ns.repo, run)
    else:
        report(gh, ns.repo, run, number)
    return 0


if __name__ == "__main__":
    sys.exit(main())
