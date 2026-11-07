#!/usr/bin/env python3
"""bump-gate: a required check on every PR; only auto/app-bump PRs are judged.

A bump PR may change official apps' and crosspad-core/crosspad-gui gitlinks and, in config/profiles/*.json,
only those apps' entries, to {"track":"pinned","commit":<new gitlink>}; one
GitHub-verified commit by crosspad-bot[bot], not from a fork, every matrix cell
green. Any other PR passes at once: the gate never blocks a person.

Which versions those are is judged where the content is: the build's `deps_resolve.py check` and `crosspad_deps.h`; a red cell fails this gate.

It runs from the BASE checkout (nothing of the PR is executed: git reads the
PR's objects, nothing more), so the PR cannot rewrite bump_gate.py. It judges
the head commit the event names, never a ref fetched later, and fails when the
API's one commit is not that head. When it cannot learn the facts of a bump PR
it fails; it never guesses a pass.

What keeps the gate itself honest is outside this file: on pull_request the
workflow file comes from the PR, so the protection relies on the auto/app-bump
branch ruleset (only crosspad-bot may update it) and on the bot's App having no
workflows permission, which keeps it from touching .github/ at all.

  bump_gate.py --event $GITHUB_EVENT_PATH --build-result success
"""
import argparse
import copy
import json
import re
import subprocess
import sys

import app_bump
import deps_resolve
import gh_api

PROFILE = re.compile(r"config/profiles/[^/]+\.json")
REGULAR = "100644"
GITLINK = "160000"


def is_bump(pr: dict) -> bool:
    return pr["head"]["ref"] == app_bump.BRANCH


def parse_raw_diff(text: str) -> list[tuple[str, str, str, str]]:
    """`git diff --raw --no-abbrev` rows as (old_mode, new_mode, new_sha, path)."""
    rows = []
    for line in text.splitlines():
        if not line.startswith(":"):
            continue
        meta, _, path = line.partition("\t")
        fields = meta[1:].split()
        if len(fields) != 5 or not path:
            raise ValueError(f"unreadable diff row: {line[:80]!r}")
        old_mode, new_mode, _old_sha, new_sha, _status = fields
        rows.append((old_mode, new_mode, new_sha, path))
    return rows


def judge(pr, commits, raw, base_profiles, head_profiles, official, build_result, head_sha=None) -> list[str]:
    problems = []
    author = (pr.get("user") or {}).get("login")
    if author != app_bump.BOT:
        problems.append(f"author {author}, not {app_bump.BOT}")
    head_repo = (pr["head"].get("repo") or {}).get("full_name")
    if head_repo is None or head_repo != pr["base"]["repo"]["full_name"]:
        problems.append("head is a fork (or its repository is gone)")
    if len(commits) != 1:
        problems.append(f"{len(commits)} commits; a bump PR has exactly one commit")
    if head_sha is not None and commits and commits[-1].get("sha") != head_sha:
        problems.append("the PR's newest commit is not the head this check was started for")
    for c in commits:
        if (c.get("author") or {}).get("login") != app_bump.BOT:
            problems.append("a commit not authored by the bot")
        if not (c["commit"].get("verification") or {}).get("verified"):
            problems.append("a commit without a verified signature")
    official_paths = {app_bump.app_path(i): i for i in official}
    lib_paths = set(deps_resolve.LIBS.values())
    moved = {}
    for old_mode, new_mode, new_sha, path in raw:
        if path in official_paths:
            if old_mode == new_mode == GITLINK:
                moved[official_paths[path]] = new_sha
            else:
                problems.append(f"{path}: an official app's gitlink may only move to another commit")
        elif path in lib_paths:
            if old_mode != GITLINK or new_mode != GITLINK:
                problems.append(f"{path}: a library gitlink may only move to another commit")
        elif PROFILE.fullmatch(path):
            if new_mode != REGULAR:
                problems.append(f"{path}: not a regular file")
        else:
            problems.append(f"{path}: not an official app gitlink or a top-level kit profile")
    for name in sorted(set(base_profiles) | set(head_profiles)):
        if name not in base_profiles or name not in head_profiles:
            problems.append(f"config/profiles/{name}.json: a bump PR neither adds nor removes a profile")
            continue
        want = copy.deepcopy(base_profiles[name])
        for app_id, sha in moved.items():
            if app_id in want.get("apps", {}):
                want["apps"][app_id] = {"track": "pinned", "commit": sha}
        if head_profiles[name] != want:
            problems.append(f"config/profiles/{name}.json: changes beyond pinning the moved apps "
                            "to their new gitlinks")
    if build_result != "success":
        problems.append(f"build matrix: {build_result} (every cell must be green)")
    return problems


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def _profiles_at(ref: str) -> dict:
    """name -> parsed config/profiles/<name>.json at ref; regular files only."""
    out = {}
    for row in _git("ls-tree", "-z", ref, "config/profiles/").split("\0"):
        meta, _, path = row.partition("\t")
        if not path or not PROFILE.fullmatch(path) or meta.split()[0] != REGULAR:
            continue
        out[path.rsplit("/", 1)[1][:-len(".json")]] = json.loads(_git("show", f"{ref}:{path}"))
    return out


def _judge_bump(pr: dict, build_result: str) -> list[str]:
    base, head = pr["base"]["sha"], pr["head"]["sha"]
    for name, sha in (("base", base), ("head", head)):
        if not isinstance(sha, str) or not app_bump.FULL_SHA.fullmatch(sha):
            raise ValueError(f"{name} sha {sha!r} is not a full commit id")
    # The workflow fetches refs/pull/N/head; if the PR moved on since this event, the
    # event's head is gone from it and the newer event's run is the one that counts.
    _git("cat-file", "-e", f"{head}^{{commit}}")
    repo = pr["base"]["repo"]["full_name"]
    commits = list(gh_api.from_env().paginate(f"/repos/{repo}/pulls/{pr['number']}/commits"))
    raw = _git("diff", "--raw", "--no-abbrev", "--no-renames", "--no-ext-diff", f"{base}...{head}", "--")
    return judge(pr, commits, parse_raw_diff(raw), _profiles_at(base), _profiles_at(head),
                 app_bump.official_apps(app_bump.REPO), build_result, head_sha=head)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--event", required=True)
    ap.add_argument("--build-result", required=True)
    ns = ap.parse_args(argv)
    try:
        with open(ns.event) as f:
            pr = json.load(f).get("pull_request")
        if pr is None:
            raise KeyError("pull_request")
        bump = is_bump(pr)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        # Whether the PR is the bot's is the one fact that decides; without it, no pass.
        print(f"::error::bump-gate: cannot read the pull request from the event: {e!r}")
        return 1
    if not bump:
        print("not a bump PR: nothing to judge")
        return 0
    try:
        problems = _judge_bump(pr, ns.build_result)
    except (Exception, SystemExit) as e:
        problems = [f"cannot determine the facts of this PR ({type(e).__name__}: {e}); not merging blind"]
    for p in problems:  # a path or an error text must not smuggle in a workflow command
        print("::error::" + re.sub(r"[\x00-\x1f\x7f]", "?", p))
    print("bump-gate: " + ("FAIL" if problems else "pass"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
