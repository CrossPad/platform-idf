#!/usr/bin/env python3
"""After publishing: crosspad.app's device catalog lists the tag for every kit
built, and still lists the newest stable (the Worker reads only the newest 20
releases; retention keeps the stable inside that window, this proves it).

  catalog_check.py --repo R --tag T --matrix JSON [--timeout 900] [--interval 15]
"""
import argparse
import json
import sys
import time
import urllib.request

import ci_alert
import fw_semver
import gh_api

CATALOG = "https://crosspad.app/api/device/catalog?rev={rev}"


def cells(matrix_json: str) -> list[tuple[str, str]]:
    return [(c["rev"], c["kit"]) for c in json.loads(matrix_json)["include"]]


def listed(catalog: dict, kit: str) -> set[str]:
    for k in catalog.get("kits", []):
        if k.get("kit") == kit:
            return {v.get("tag") for v in k.get("versions", [])}
    return set()


def missing(catalogs: dict, tag: str, cell_list, stable: str | None) -> list[str]:
    problems = []
    for rev, kit in cell_list:
        if tag not in listed(catalogs.get(rev, {}), kit):
            problems.append(f"{rev}/{kit}: {tag} not listed")
    if stable:
        for rev in sorted({r for r, _ in cell_list}):
            if stable not in listed(catalogs.get(rev, {}), "full"):
                problems.append(f"{rev}/full: newest stable {stable} not listed")
    return problems


def newest_published_stable(releases) -> str | None:
    stables = [t for t in (fw_semver.parse(r.get("tag_name", "")) for r in releases
                           if not r.get("draft")) if t is not None and t.stable]
    return max(stables, key=fw_semver.key).name if stables else None


def fetch(rev: str) -> dict:
    # A public endpoint, so no GitHub token: gh_api is for api.github.com only.
    req = urllib.request.Request(CATALOG.format(rev=rev), headers={"User-Agent": "crosspad-ci"})
    with urllib.request.urlopen(req, timeout=20) as r:
        body = json.loads(r.read())
    if not isinstance(body, dict):
        raise ValueError(f"catalog for {rev} is not a JSON object")
    return body


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--matrix", required=True)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--interval", type=float, default=15)
    ns = ap.parse_args(argv)
    try:
        stable = newest_published_stable(gh_api.from_env().paginate(f"/repos/{ns.repo}/releases"))
    except Exception as e:
        msg = f"release listing failed: {type(e).__name__}"
        ci_alert.send(f"catalog: {ns.tag}", msg)
        print(f"::error::{msg}")
        return 1
    cell_list = cells(ns.matrix)
    deadline = time.monotonic() + ns.timeout
    while True:
        try:
            cats = {rev: fetch(rev) for rev in sorted({r for r, _ in cell_list})}
            problems = missing(cats, ns.tag, cell_list, stable)
        except Exception as e:  # null kits (TypeError), IncompleteRead, ...: retry, then alert
            what = str(e) if isinstance(e, (OSError, ValueError)) else f"{type(e).__name__}: {e}"
            problems = [f"catalog fetch failed: {what}"]
        if not problems:
            print(f"catalog lists {ns.tag} for {len(cell_list)} cell(s); newest stable: {stable or 'none'}")
            return 0
        if time.monotonic() >= deadline:
            break
        time.sleep(max(0.0, min(ns.interval, deadline - time.monotonic())))
    ci_alert.send(f"catalog: {ns.tag}", "\n".join(problems))
    for p in problems:
        print(f"::error::{p}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
