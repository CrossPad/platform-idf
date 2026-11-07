#!/usr/bin/env python3
"""Release retention: keep the published set small, never delete.

Kept published:
  - every published release of the newest minor line;
  - in each older minor line its newest stable, or its newest release if it has
    no stable;
  - the newest stable overall and --keep (normally already kept).
Every other published release goes back to draft (reversible: publish it
again, which makes the announcer post again). Drafts and non-release tags are
never touched; nothing is ever published or deleted.

  release_retention.py --repo R [--keep TAG] [--apply] [--max N]
"""
import argparse
import os
import sys
from typing import NamedTuple

import fw_semver
import gh_api


class Plan(NamedTuple):
    keep: list            # [(tag, reason)] in device order
    unpublish: list       # [{"tag", "id"}] in device order
    newest_stable: str | None


def plan(releases: list[dict], keep_tags=()) -> Plan:
    published = []
    for r in releases:
        t = fw_semver.parse(r.get("tag_name", ""))
        if t is not None and not r.get("draft"):
            published.append((t, r))
    if not published:
        return Plan([], [], None)
    stables = [t for t, _ in published if t.stable]
    newest_stable = max(stables, key=fw_semver.key) if stables else None
    top = max(t.line for t, _ in published)
    keep: dict[str, str] = {}
    for t, _ in published:
        if t.line == top:
            keep[t.name] = f"newest minor line {top[0]}.{top[1]}"
    for line in sorted({t.line for t, _ in published} - {top}):
        members = [t for t, _ in published if t.line == line]
        st = [t for t in members if t.stable]
        pick = max(st or members, key=fw_semver.key)
        keep[pick.name] = f"newest {'stable' if st else 'release'} of {line[0]}.{line[1]}"
    if newest_stable is not None:
        keep.setdefault(newest_stable.name, "newest stable")
    names = {t.name for t, _ in published}
    for k in keep_tags:
        if k in names:
            keep.setdefault(k, "this run's release")
    order = sorted(published, key=lambda tr: fw_semver.key(tr[0]))
    return Plan([(t.name, keep[t.name]) for t, _ in order if t.name in keep],
                [{"tag": t.name, "id": r["id"]} for t, r in order if t.name not in keep],
                newest_stable.name if newest_stable else None)


def render(p: Plan, applying: bool) -> str:
    lines = [f"Retention ({'apply' if applying else 'dry run'}); newest stable: "
             f"{p.newest_stable or 'none published'}", "", "| tag | action | why |", "|---|---|---|"]
    lines += [f"| {t} | keep | {why} |" for t, why in p.keep]
    lines += [f"| {u['tag']} | unpublish to draft | superseded |" for u in p.unpublish]
    return "\n".join(lines) + "\n"


def apply(gh, repo: str, p: Plan, max_changes: int) -> int:
    if len(p.unpublish) > max_changes:
        print(f"::error::retention would unpublish {len(p.unpublish)} releases (> --max {max_changes}); "
              "review the dry run and re-run by hand with a higher --max")
        return 1
    for u in p.unpublish:
        gh.patch(f"/repos/{repo}/releases/{u['id']}", {"draft": True})
        print(f"unpublished {u['tag']} (now a draft)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--keep", action="append", default=[])
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--max", type=int, default=10)
    ns = ap.parse_args(argv)
    gh = gh_api.from_env()
    p = plan(list(gh.paginate(f"/repos/{ns.repo}/releases")), ns.keep)
    text = render(p, ns.apply)
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(text)
    return apply(gh, ns.repo, p, ns.max) if ns.apply else 0


if __name__ == "__main__":
    sys.exit(main())
