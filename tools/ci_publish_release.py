#!/usr/bin/env python3
"""After the draft upload: prove every asset landed, then publish an rc.

A draft is not visible to GET /releases/tags/<tag>, so the release is found
by listing. Only vX.Y.Z-rcN is published, and only with AUTO_PUBLISH=on;
a stable or any other pre-release label stays a draft for a human.

  ci_publish_release.py verify  --repo R --tag T --out DIR
  ci_publish_release.py publish --repo R --tag T --out DIR

publish runs the same verify against DIR first; any problem and nothing is
published.
"""
import argparse
import os
import sys
from pathlib import Path

import fw_semver
import gh_api


def find_release(gh, repo: str, tag: str) -> dict | None:
    for r in gh.paginate(f"/repos/{repo}/releases"):
        if r.get("tag_name") == tag:
            return r
    return None


def verify(release: dict, out_dir: Path) -> list[str]:
    local = {p.name: p.stat().st_size for p in out_dir.iterdir() if p.is_file()}
    remote = {a["name"]: a for a in release.get("assets", [])}
    problems = []
    if not local:
        problems.append("no built assets to verify against")
    for name, size in sorted(local.items()):
        a = remote.get(name)
        if a is None:
            problems.append(f"{name}: not on the release")
        elif a.get("state") != "uploaded":
            problems.append(f"{name}: state {a.get('state')}")
        elif a.get("size") != size:
            problems.append(f"{name}: {a.get('size')} B on the release, {size} B here")
    for name in sorted(set(remote) - set(local)):
        problems.append(f"{name}: on the release, not built by this run")
    if not release.get("draft"):
        problems.append("the release is not a draft")
    return problems


def publish_decision(tag: str, auto_publish: str) -> tuple[bool, str]:
    if auto_publish != "on":
        return False, "AUTO_PUBLISH is not on"
    if not fw_semver.RC_TAG.fullmatch(tag):
        return False, f"{tag} is not an rc tag; a human publishes it"
    return True, "rc tag"


def _output(key: str, value: str) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{key}={value}\n")


def _summary(line: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"\n{line}\n")
    except OSError as e:
        print(f"summary not written: {type(e).__name__}")


def publish(gh, repo: str, tag: str, out_dir: Path) -> int:
    ok, why = publish_decision(tag, os.environ.get("AUTO_PUBLISH", ""))
    if not ok:
        cmd = f"gh release edit {tag} -R {repo} --draft=false"
        print(f"not published: {why}. A human: {cmd}")
        _summary(f"**{tag} is a draft waiting for a human** ({why}). Publish it with: `{cmd}`")
        _output("published", "false")
        return 0
    rel = find_release(gh, repo, tag)
    if rel is None or not rel.get("draft"):
        print(f"::error::{tag}: no draft release to publish")
        return 1
    problems = verify(rel, out_dir)
    if problems:
        for p in problems:
            print(f"::error::{p}")
        print(f"::error::{tag}: not published, the draft does not match the build")
        return 1
    gh.patch(f"/repos/{repo}/releases/{rel['id']}", {"draft": False})
    print(f"published {tag}")
    _output("published", "true")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["verify", "publish"])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--out", type=Path)
    ns = ap.parse_args(argv)
    if ns.out is None:
        ap.error("--out is required")
    gh = gh_api.from_env()
    if ns.cmd == "publish":
        return publish(gh, ns.repo, ns.tag, ns.out)
    rel = find_release(gh, ns.repo, ns.tag)
    if rel is None:
        print(f"::error::{ns.tag}: no release")
        return 1
    problems = verify(rel, ns.out)
    for p in problems:
        print(f"::error::{p}")
    print(f"{len(rel.get('assets', []))} assets on the draft; {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
