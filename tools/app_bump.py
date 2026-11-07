#!/usr/bin/env python3
"""Official kit profiles pin every app to the commit its gitlink names.

CI builds what `profile apply` leaves in components/: today that is the
gitlink, by accident (apps.json is absent, so install() leaves a 'main'-ref
submodule alone). A pinned profile entry makes it the rule, and `check` keeps
pins and gitlinks equal. Spec: docs/design/2026-10-10-release-
automation-design.md, "platform-idf: deterministic kit builds".

  app_bump.py check                         pins == gitlinks, every app pinned
  app_bump.py pin <id>... | --all           set the pins from the gitlinks
  app_bump.py apply <id>=<sha>...           set the pins to these commits
  app_bump.py assert-built --profile <name> <components.txt>
  app_bump.py repos                         official apps' repo names, comma-separated;
                                            exit 1 with ::error:: on stderr (stdout stays
                                            empty) if any app has no repository
  app_bump.py plan                          what reconcile would do now (APP_READ_TOKEN); writes nothing
  app_bump.py reconcile --repo R [--event E]  keep the rolling auto/app-bump PR current (apps, core, gui)
"""
import argparse
import base64
import calendar
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path
from typing import NamedTuple

import deps_resolve
import gh_api
import semver_range

REPO = Path(__file__).resolve().parent.parent
PROFILE_REL = Path("config") / "profiles"


def app_path(app_id: str) -> str:
    return f"components/crosspad-{app_id}"


def _inline(entry: dict) -> str:
    return "{ " + ", ".join(f"{json.dumps(k)}: {json.dumps(v)}" for k, v in entry.items()) + " }"


def dumps_profile(doc: dict) -> str:
    """The profiles' own layout: json indent 2, one app per line."""
    apps = doc.get("apps", {})
    body = dict(doc)
    body["apps"] = "__APPS__"
    text = json.dumps(body, indent=2, ensure_ascii=False)
    inner = ",\n".join(f"    {json.dumps(k)}: {_inline(v)}" for k, v in apps.items())
    block = "{\n" + inner + "\n  }" if apps else "{}"
    return text.replace('"__APPS__"', block, 1) + "\n"


def profile_paths(root: Path) -> list[Path]:
    """Official profiles only: config/profiles/*.json, never community/."""
    return sorted((root / PROFILE_REL).glob("*.json"))


def load_profiles(root: Path) -> dict[str, dict]:
    return {p.stem: json.loads(p.read_text()) for p in profile_paths(root)}


def gitlinks(root: Path, rev: str = "HEAD") -> dict[str, str]:
    out = subprocess.run(["git", "-C", str(root), "ls-tree", rev, "components/"],
                         check=True, capture_output=True, text=True).stdout
    links = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        mode, kind, sha = meta.split()
        if mode == "160000" and kind == "commit":
            links[path] = sha
    return links


def check(root: Path, rev: str = "HEAD") -> list[str]:
    links = gitlinks(root, rev)
    problems = []
    for name, doc in load_profiles(root).items():
        for app_id, entry in doc.get("apps", {}).items():
            link = links.get(app_path(app_id))
            if entry.get("track") != "pinned":
                problems.append(f"{name}: {app_id}: track {entry.get('track', 'registry')}; official kits "
                                f"pin every app (tools/app_bump.py pin {app_id})")
            elif link is None:
                problems.append(f"{name}: {app_id}: no gitlink at {app_path(app_id)}")
            elif entry.get("commit") != link:
                problems.append(f"{name}: {app_id}: pinned {str(entry.get('commit'))[:12]}, gitlink "
                                f"{link[:12]} (tools/app_bump.py pin {app_id})")
    return problems


def apply_pins(root: Path, pins: dict[str, str]) -> dict[str, str]:
    """New text of every official profile that changes; nothing is written."""
    changed = {}
    for path in profile_paths(root):
        doc = json.loads(path.read_text())
        apps = doc.get("apps", {})
        touched = False
        for app_id, sha in pins.items():
            want = {"track": "pinned", "commit": sha}
            if app_id in apps and apps[app_id] != want:
                apps[app_id] = want
                touched = True
        if touched:
            changed[str(path.relative_to(root))] = dumps_profile(doc)
    return changed


def assert_built(profile_doc: dict, components_txt: str) -> list[str]:
    """`git submodule status` of a build against the profile's pins."""
    built = {}
    unbuilt = set()  # "-<sha> <path>": submodule not checked out
    for line in components_txt.splitlines():
        parts = line.strip().lstrip("+-U").split()
        if len(parts) >= 2:
            built[parts[1]] = parts[0]
            if line.startswith("-"):
                unbuilt.add(parts[1])
    problems = []
    for app_id, entry in profile_doc.get("apps", {}).items():
        if entry.get("track") != "pinned":
            continue
        got = built.get(app_path(app_id))
        if app_path(app_id) in unbuilt:
            problems.append(f"{app_id}: {app_path(app_id)} not checked out")
        elif got is None:
            problems.append(f"{app_id}: {app_path(app_id)} not in components.txt")
        elif got != entry["commit"]:
            problems.append(f"{app_id}: built {got[:12]}, pinned {entry['commit'][:12]}")
    return problems


# Tags, ids and shas come from app repos: untrusted, so matched as ASCII and in full.
RELEASE_TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", re.ASCII)
MAX_TAG_LEN = 23  # release tags are at most 23 characters
APP_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*", re.ASCII)
FULL_SHA = re.compile(r"[0-9a-f]{40}", re.ASCII)
GITHUB_URL = re.compile(r"github\.com[/:]([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?", re.ASCII)
LIB_HOLD = {"core": deps_resolve.CORE, "gui": deps_resolve.GUI}  # APP_BUMP_HOLD names for the libraries


class BumpError(Exception):
    """A configuration problem that must stop the run rather than be guessed around."""


REASON_MAX = 200   # a skipped tag's cause as it goes into the PR body and the held alert


def _safe(value) -> str:
    """Untrusted text for a one-line note: no control characters, bounded."""
    return re.sub(r"[^A-Za-z0-9._-]", "?", str(value))[:40]


def official_profile_files(root: Path) -> frozenset[str]:
    """Repo-relative paths of the official profiles: the only files the bump commit may write."""
    return frozenset(p.relative_to(root).as_posix() for p in profile_paths(root))


def official_apps(root: Path) -> list[str]:
    return sorted({i for d in load_profiles(root).values() for i in d.get("apps", {})})


def submodule_repos(root: Path) -> dict[str, str]:
    """components/<path> -> Owner/name for every GitHub submodule in .gitmodules."""
    if not (root / ".gitmodules").is_file():
        raise BumpError(".gitmodules not found")
    res = subprocess.run(["git", "config", "-f", str(root / ".gitmodules"), "--get-regexp",
                          r"^submodule\..*\.(path|url)$"], capture_output=True, text=True)
    if res.returncode not in (0, 1):  # 1 is "no match"; anything else is a missing or broken file
        raise BumpError(f"git config could not read .gitmodules (exit {res.returncode})")
    out = res.stdout
    paths, urls = {}, {}
    for line in out.splitlines():
        key, _, value = line.partition(" ")
        name = key[len("submodule."):key.rindex(".")]
        (paths if key.endswith(".path") else urls)[name] = value.strip()
    repos = {}
    for name, path in paths.items():
        m = GITHUB_URL.fullmatch(re.sub(r"^(?:https?://|git@|ssh://git@)", "", urls.get(name, "")))
        if m:
            repos[path] = m.group(1)
    return repos


def repos_scope(root: Path) -> list[str]:
    """Repo names (no owner) of every official app and of core/gui, for the bot token's
    `repositories:`. Fails closed: an empty or partial list could widen the token's reach,
    so a missing .gitmodules entry for any of them is an error, never a gap."""
    paths = {i: app_path(i) for i in official_apps(root)}
    paths.update(deps_resolve.LIBS)
    repos = submodule_repos(root)
    missing = sorted(name for name, path in paths.items() if path not in repos)
    if missing:
        raise BumpError("component(s) without a GitHub submodule entry: " + ", ".join(_safe(i) for i in missing))
    names = sorted({repos[p].split("/")[1] for p in paths.values()})
    if not names:
        raise BumpError("no official apps found; refusing to print an empty repository list")
    return names


def parse_hold(raw, official) -> tuple[set, list]:
    """APP_BUMP_HOLD: comma-separated ids, case and whitespace insensitive."""
    hold = {h.strip().lower() for h in (raw or "").split(",") if h.strip()}
    notes = [f"APP_BUMP_HOLD: {_safe(h)} matches no official app" for h in sorted(hold) if h not in set(official)]
    return hold, notes


def _valid_release(name, sha) -> bool:
    return (isinstance(name, str) and len(name) <= MAX_TAG_LEN and RELEASE_TAG.fullmatch(name) is not None
            and isinstance(sha, str) and FULL_SHA.fullmatch(sha) is not None)


def release_tags(tags: list[dict]) -> list[tuple]:
    """Every plain vX.Y.Z (no prerelease) among GitHub /tags items, ascending:
    [(version, tag, commit sha)]."""
    out = []
    for t in tags:
        name, sha = t.get("name"), (t.get("commit") or {}).get("sha")
        if _valid_release(name, sha):
            out.append((tuple(int(x) for x in RELEASE_TAG.fullmatch(name).groups()), name, sha))
    return sorted(out)


BRANCH = "auto/app-bump"
BOT = "crosspad-bot[bot]"
# The bot's one commit may touch only app gitlinks and the official profiles (profile_paths,
# the same set apply_pins writes); bump-gate (tools/bump_gate.py) checks the PR from the other
# side: any top-level profile, but its content.
GITLINK_PATH = re.compile(r"components/crosspad-[a-z0-9]+(?:-[a-z0-9]+)*", re.ASCII)
REPO_NAME = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", re.ASCII)
BRANCH_NAME = re.compile(r"[A-Za-z0-9._/-]+", re.ASCII)
RELATIONS = frozenset({"ahead", "identical", "behind", "diverged"})
ALERT_HOUR_UTC = 5  # scheduled runs alert only in this UTC hour: with an hourly cron, once a day
STUCK_AFTER_S = 86400
DISABLE_AUTOMERGE = ("mutation($id: ID!) { disablePullRequestAutoMerge(input: {pullRequestId: $id}) "
                     "{ clientMutationId } }")
# expectedHeadOid: GitHub merges only the head this run created, never one that moved after.
AUTOMERGE = ("mutation($id: ID!, $oid: GitObjectID!) { enablePullRequestAutoMerge(input: "
             "{pullRequestId: $id, expectedHeadOid: $oid, mergeMethod: SQUASH}) { clientMutationId } }")


class Move(NamedTuple):
    name: str                      # app id, or crosspad-core / crosspad-gui
    label: str                     # what the title calls it: app id, core, gui
    path: str                      # the gitlink
    repo: str                      # Owner/name
    old: "deps_resolve.Release"
    new: "deps_resolve.Release"


def plan_moves(apps: dict, libs: dict, solution, repos: dict) -> list:
    """Every component the solution moves off its pinned commit: libraries first, then apps."""
    moves = []
    for name, opts in list(libs.items()) + sorted(apps.items()):
        new = solution.picks[name]
        if new.commit != opts.pinned.commit:
            is_lib = name in deps_resolve.LIBS
            moves.append(Move(name, deps_resolve.SHORT[name] if is_lib else name,
                              deps_resolve.LIBS[name] if is_lib else app_path(name),
                              repos[name], opts.pinned, new))
    return moves


def pr_title(moves) -> str:
    fmt = semver_range.fmt
    return "chore(deps): " + ", ".join(f"{m.label} {fmt(m.old.version)}→{fmt(m.new.version)}" for m in moves)


def pr_body(moves, held) -> str:
    fmt = semver_range.fmt
    rows = ["| component | from | to | commits |", "|---|---|---|---|"]
    for m in moves:
        a, b = m.old.commit[:12], m.new.commit[:12]
        rows.append(f"| {m.label} | {fmt(m.old.version)} | {fmt(m.new.version)} | "
                    f"[{a}...{b}](https://github.com/{m.repo}/compare/{a}...{b}) |")
    text = ("Automatic bump: gitlinks plus `\"track\":\"pinned\"` in the official kit profiles, one "
            "set that satisfies every app's `requires` (tools/app_bump.py reconcile, tools/deps_resolve.py). "
            "Merges itself when `bump-gate`, `build (v1)` and `build (v2)` are green and "
            "APP_BUMP_AUTOMERGE=on.\n\n" + "\n".join(rows) + "\n")
    if held:
        text += ("\n**Held back** (not in this bump; the newest release's ranges conflict with this set, "
                 "or its tag is unusable):\n\n") + \
                "\n".join(f"- {h}" for h in held) + "\n"
    return text


class BumpWriter:
    def __init__(self, gh, repo: str):
        self.gh, self.repo = gh, repo
        self.owner = repo.split("/")[0]

    def open_pr(self) -> dict | None:
        """The bot's open PR against main from the bump branch. Any other open PR on that
        branch is not ours to adopt, close or overwrite: stop before a write."""
        prs = self.gh.get(f"/repos/{self.repo}/pulls?head={self.owner}:{BRANCH}&state=open")
        for pr in prs or []:
            if (pr.get("user") or {}).get("login") == BOT and (pr.get("base") or {}).get("ref") == "main":
                return pr
        if prs:
            raise BumpError(f"open PR #{_safe(prs[0].get('number'))} on {BRANCH} is not the bot's PR "
                            f"against main; leaving it and the branch alone")
        return None

    @staticmethod
    def _guard_branch() -> None:
        if BRANCH != "auto/app-bump":
            raise ValueError(f"refusing to write refs for {_safe(BRANCH)}")

    def build_commit(self, base_sha, pins, files, message) -> tuple[str, str]:
        """One commit on top of base_sha: gitlinks and the official profiles, nothing else."""
        for path, sha in pins.items():
            if not GITLINK_PATH.fullmatch(path) or not FULL_SHA.fullmatch(sha):
                raise ValueError(f"not an app gitlink update: {_safe(path)}")
        profiles = official_profile_files(REPO)
        for path in files:
            if path not in profiles:
                raise ValueError(f"not an official profile: {_safe(path)}")
        base_tree = self.gh.get(f"/repos/{self.repo}/git/commits/{base_sha}")["tree"]["sha"]
        entries = [{"path": p, "mode": "160000", "type": "commit", "sha": s} for p, s in sorted(pins.items())]
        entries += [{"path": p, "mode": "100644", "type": "blob", "content": t} for p, t in sorted(files.items())]
        tree = self.gh.post(f"/repos/{self.repo}/git/trees", {"base_tree": base_tree, "tree": entries})["sha"]
        commit = self.gh.post(f"/repos/{self.repo}/git/commits",
                              {"message": message, "tree": tree, "parents": [base_sha]})["sha"]
        return commit, tree

    def head_matches(self, head_sha: str, tree: str, base_sha: str) -> bool:
        """The branch head is already exactly this commit's content, as a single commit on base."""
        c = self.gh.get(f"/repos/{self.repo}/git/commits/{head_sha}")
        return c["tree"]["sha"] == tree and [p["sha"] for p in c["parents"]] == [base_sha]

    def _create_branch(self, commit_sha: str) -> None:
        self._guard_branch()
        self.gh.post(f"/repos/{self.repo}/git/refs", {"ref": f"refs/heads/{BRANCH}", "sha": commit_sha})

    def _delete_branch(self) -> None:
        """Delete the branch; one already gone (404, or 422 "Reference does not exist") is fine."""
        self._guard_branch()
        try:
            self.gh.delete(f"/repos/{self.repo}/git/refs/heads/{BRANCH}")
        except gh_api.ApiError as e:
            if e.status not in (404, 422):
                raise

    def update_branch(self, commit_sha: str) -> bool:
        """Point the branch at the commit, force: the branch never holds more than the bot's one commit.

        If main changed .github/workflows/* since the branch's base, GitHub may refuse the force
        update to an App without the workflows permission (403/422). Then the branch is deleted,
        which closes its PR, and created afresh at the commit. Returns True exactly then, so the
        caller opens a new PR (ensure_pr fresh=True); a missing branch was only created: False."""
        self._guard_branch()
        try:
            self.gh.get(f"/repos/{self.repo}/git/ref/heads/{BRANCH}")
        except gh_api.ApiError as e:
            if e.status != 404:
                raise
            self._create_branch(commit_sha)
            return False
        try:
            self.gh.patch(f"/repos/{self.repo}/git/refs/heads/{BRANCH}", {"sha": commit_sha, "force": True})
        except gh_api.ApiError as e:
            if e.status not in (403, 422):
                raise
            print(f"::notice::{BRANCH}: force update refused ({e.status}); recreating the branch, "
                  f"which closes its PR, and opening a new one")
            self._delete_branch()
            self._create_branch(commit_sha)
            return True
        return False

    def ensure_pr(self, title: str, body: str, fresh: bool = False) -> dict:
        """The bot's PR for the branch, created or updated. fresh=True (the branch was just
        recreated) always opens a new PR: GitHub closes the old one asynchronously, so a
        listing may still show it, and it must not be edited or reused."""
        pr = None if fresh else self.open_pr()
        if pr is None:
            return self.gh.post(f"/repos/{self.repo}/pulls",
                                {"title": title, "head": BRANCH, "base": "main", "body": body})
        self.gh.patch(f"/repos/{self.repo}/pulls/{pr['number']}", {"title": title, "body": body})
        return pr

    def labels(self, pr: dict, add: list[str], remove: list[str]) -> None:
        if add:
            self.gh.post(f"/repos/{self.repo}/issues/{pr['number']}/labels", {"labels": add})
        for name in remove:
            try:
                self.gh.delete(f"/repos/{self.repo}/issues/{pr['number']}/labels/{name}")
            except gh_api.ApiError as e:
                if e.status != 404:
                    raise

    def enable_automerge(self, pr: dict, commit_sha: str) -> None:
        try:
            self.gh.graphql(AUTOMERGE, id=pr["node_id"], oid=commit_sha)
        except gh_api.ApiError as e:
            if "clean status" not in str(e).lower() and "already" not in str(e).lower():
                raise
            print(f"::notice::#{pr['number']}: auto-merge not needed ({_safe(str(e)[:80])})")

    def disable_automerge(self, pr: dict) -> None:
        self.gh.graphql(DISABLE_AUTOMERGE, id=pr["node_id"])

    def close(self, pr: dict) -> None:
        self._guard_branch()
        self.gh.patch(f"/repos/{self.repo}/pulls/{pr['number']}", {"state": "closed"})
        self._delete_branch()


def _relation(read_gh, repo: str, base: str, head: str) -> str | None:
    """compare status of base...head. "Not comparable" (404, or 422 for no merge base) and
    an odd answer are None: unknown is never `ahead`, so it can never become a pin. Every other
    API error (5xx, 403/429 rate limits) is re-raised: a transient read failure must stop the
    run before a write, not look like "nothing to bump" and close the PR."""
    try:
        status = read_gh.get(f"/repos/{repo}/compare/{base}...{head}")["status"]
    except gh_api.ApiError as e:
        if e.status in (404, 422):
            return None
        raise
    except (KeyError, TypeError):
        return None
    return status if isinstance(status, str) and status in RELATIONS else None


def _content(read_gh, repo: str, path: str, ref: str) -> str | None:
    """A file's text at ref; None when it is not there or not text. Other API errors raise."""
    try:
        c = read_gh.get(f"/repos/{repo}/contents/{path}?ref={ref}")
        return base64.b64decode(c["content"]).decode("utf-8")
    except gh_api.ApiError as e:
        if e.status == 404:
            return None
        raise
    except (KeyError, TypeError, ValueError):  # binascii.Error and UnicodeDecodeError are ValueErrors
        return None


def _json(text: str):
    """json.loads for untrusted text: a document nested too deeply is a ValueError, not a RecursionError."""
    try:
        return json.loads(text)
    except RecursionError:
        raise ValueError("document nested too deeply") from None


def _app_release(read_gh, repo: str, app_id: str, sha: str, tag) -> "deps_resolve.Release | str":
    """The app's Release at sha, or why it is not one."""
    text = _content(read_gh, repo, "crosspad-app.json", sha)
    if text is None:
        return "no readable crosspad-app.json"
    try:
        doc = _json(text)
        if not isinstance(doc, dict) or doc.get("id") != app_id:
            return f"crosspad-app.json has id {_safe(doc.get('id') if isinstance(doc, dict) else None)}"
        return deps_resolve.Release(semver_range.parse_version(doc.get("version")), sha, tag,
                                    deps_resolve.manifest_requires(doc))
    except ValueError as e:
        return _oneline(e, REASON_MAX)


def _lib_release(read_gh, repo: str, lib: str, sha: str, tag) -> "deps_resolve.Release | str":
    rel, prefix = deps_resolve.HEADERS[lib]
    text = _content(read_gh, repo, rel, sha)
    if text is None:
        return f"no readable {rel}"
    try:
        version = deps_resolve.header_version(text, prefix)
        req = {}
        if lib == deps_resolve.GUI:
            lib_text = _content(read_gh, repo, "library.json", sha)
            if lib_text is None:
                return "no readable library.json"
            req = deps_resolve.gui_requires(_json(lib_text))
        return deps_resolve.Release(version, sha, tag, req)
    except ValueError as e:
        return _oneline(e, REASON_MAX)


def _ineligible(read_gh, repo: str, default, gitlink: str, tag: str, sha: str) -> str | None:
    """Why this tag cannot be pinned (by git topology), or None. "diverged" is its own answer."""
    if not (isinstance(default, str) and BRANCH_NAME.fullmatch(default)) or \
            _relation(read_gh, repo, sha, urllib.parse.quote(default, safe="/")) not in ("ahead", "identical"):
        return f"{tag} is not on the default branch"
    rel = _relation(read_gh, repo, gitlink, sha)
    if rel == "diverged":
        return "diverged"
    if rel != "ahead":
        return f"{tag} is {_safe(rel)} relative to the gitlink"
    return None


def collect_options(read_gh, root: Path, hold: set) -> tuple:
    """(apps, libs, notes, diverged, skipped): each official app's and core/gui's pinned Release and
    the newer releases it may move to. notes are topology causes (log only), skipped are tags whose
    CONTENT disqualifies them ("mixer v0.4.0: <reason>"; reported as held). Reads only. A pinned component that cannot be read is
    a BumpError; every API error but 404/422 is raised, before any write."""
    links, repos = gitlinks(root), submodule_repos(root)
    comps = [(i, app_path(i), i, False) for i in official_apps(root)]
    comps += [(lib, path, deps_resolve.SHORT[lib], True) for lib, path in deps_resolve.LIBS.items()]
    apps, libs, notes, diverged, skipped = {}, {}, [], [], []
    for name, path, hold_id, is_lib in comps:
        repo = repos.get(path)
        if repo is None or path not in links:
            if is_lib:
                raise BumpError(f"{name}: no GitHub submodule or gitlink at {path}")
            print(f"::warning::{name}: no GitHub submodule or gitlink at {path}; not reconciled")
            continue
        read = (lambda sha, tag, r=repo, n=name: _lib_release(read_gh, r, n, sha, tag)) if is_lib else \
               (lambda sha, tag, r=repo, n=name: _app_release(read_gh, r, n, sha, tag))
        pinned = read(links[path], None)
        if isinstance(pinned, str):
            raise BumpError(f"{name}: the pinned commit {links[path][:12]}: {pinned}")
        newer = []
        if hold_id in hold:
            notes.append(f"{hold_id}: held (APP_BUMP_HOLD)")
        else:
            default = None
            for version, tag, sha in release_tags(list(read_gh.paginate(f"/repos/{repo}/tags"))):
                if version <= pinned.version:
                    continue
                if is_lib and version[0] != pinned.version[0]:
                    notes.append(f"{hold_id}: {tag} is another MAJOR; a human decision")
                    continue
                if default is None:
                    default = read_gh.get(f"/repos/{repo}").get("default_branch")
                why = _ineligible(read_gh, repo, default, links[path], tag, sha)
                if why == "diverged":
                    if name not in diverged:
                        diverged.append(name)
                    notes.append(f"{hold_id}: {tag} and the gitlink have diverged; needs a human")
                    continue
                if why:
                    notes.append(f"{hold_id}: {why}")
                    continue
                r = read(sha, tag)
                if isinstance(r, str):
                    skipped.append(f"{hold_id} {tag}: {r}")
                elif r.version != version:
                    skipped.append(f"{hold_id} {tag}: its version reads {semver_range.fmt(r.version)}")
                else:
                    newer.append(r)
        (libs if is_lib else apps)[name] = deps_resolve.Options(pinned, newer)
    return apps, libs, notes, diverged, skipped


def _local_head(root: Path) -> str:
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()


def _now() -> float:
    return time.time()


def _alert_slot(event: str) -> bool:
    """Alerts are rate limited by slot, not by state: a scheduled run may alert only in the
    ALERT_HOUR_UTC hour, so an hourly cron posts each standing condition at most once a day.
    Manual and push runs never alert. The workflow's cron must fire in that hour."""
    return event == "schedule" and time.gmtime(_now()).tm_hour == ALERT_HOUR_UTC


def _iso(text: str) -> float:
    return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%SZ"))


def _stuck(gh, checks_gh, repo: str, pr: dict) -> bool:
    """Open more than 24 h with no finished check. Age counts from the head commit's committer
    date (a rebuilt bump is a new commit), not from when the PR was first opened. Check runs
    are read with checks_gh (the job's GITHUB_TOKEN): the bot's App has no Checks permission."""
    sha = pr["head"]["sha"]
    try:
        age = _now() - _iso(gh.get(f"/repos/{repo}/git/commits/{sha}")["committer"]["date"])
    except (KeyError, TypeError, ValueError):
        print(f"::notice::#{pr['number']}: head commit date unreadable; stuck check skipped")
        return False
    runs = checks_gh.get(f"/repos/{repo}/commits/{sha}/check-runs").get("check_runs", [])
    return age > STUCK_AFTER_S and not any(r.get("conclusion") for r in runs)


def _checkout_is_current(base: str) -> bool:
    """The profiles come from this checkout; committing them onto a different main would
    silently revert whatever changed there. False means: write nothing this run."""
    if base == _local_head(REPO):
        return True
    ref = os.environ.get("GITHUB_REF", "")
    if ref and ref != "refs/heads/main":
        print(f"::notice::checkout is {_safe(ref)}, not main; reconcile only runs from main")
    else:
        print(f"main moved since checkout ({base[:12]}); not reconciling, the next run does")
    return False


def _oneline(line, limit: int | None = None) -> str:
    """Text read from other repos (a range, an app id) becomes one inert line: no newline can
    start a second line, so nothing printed or sent can smuggle a workflow command. limit bounds it."""
    return re.sub(r"[\x00-\x1f\x7f]", "?", str(line))[:limit]


def _plan(read_gh) -> tuple:
    """(moves, held, notes, diverged) from the live repos. Reads only. held is what the solver
    leaves below its newest release plus the tags skipped for their content."""
    hold, hold_notes = parse_hold(os.environ.get("APP_BUMP_HOLD"), official_apps(REPO) + sorted(LIB_HOLD))
    apps, libs, notes, diverged, skipped = collect_options(read_gh, REPO, hold)
    try:
        solution = deps_resolve.solve(apps, libs)
    except deps_resolve.Unsatisfiable as e:
        raise BumpError(f"the pinned set breaks a range, fix main first: {_oneline(e)}") from e
    by_path = submodule_repos(REPO)
    repos = {name: by_path[deps_resolve.LIBS.get(name) or app_path(name)] for name in list(apps) + list(libs)}
    held = [_oneline(h) for h in solution.held] + [_oneline(k, REASON_MAX) for k in skipped]
    return (plan_moves(apps, libs, solution, repos), held,
            [_oneline(n) for n in hold_notes + notes], diverged)


def cmd_plan() -> int:
    moves, held, notes, _ = _plan(gh_api.GitHub(os.environ["APP_READ_TOKEN"]))
    for line in notes:
        print(line)
    print(pr_title(moves) if moves else "nothing to move")
    for h in held:
        print(f"held: {h}")
    return 0


def cmd_reconcile(repo: str, event: str) -> int:
    import ci_alert
    if not REPO_NAME.fullmatch(repo):
        raise ValueError(f"not an Owner/name repository: {_safe(repo)}")
    read_gh = gh_api.GitHub(os.environ["APP_READ_TOKEN"])
    gh = gh_api.GitHub(os.environ["BUMP_TOKEN"])
    checks_gh = gh_api.GitHub(os.environ["CHECKS_TOKEN"])  # GITHUB_TOKEN, checks: read; nothing else
    base = gh.get(f"/repos/{repo}/git/ref/heads/main")["object"]["sha"]
    if not _checkout_is_current(base):
        return 0
    moves, held, notes, diverged = _plan(read_gh)  # every read error is raised here, before any write
    for note in notes:
        print(note)
    alerting = _alert_slot(event)
    if diverged and alerting:
        ci_alert.send("app bump: needs a human", "diverged: " + ", ".join(diverged))
    if held and alerting:
        ci_alert.send("app bump: held", "\n".join(held))
    w = BumpWriter(gh, repo)
    pr = w.open_pr()
    if not moves:
        if pr is not None:
            w.close(pr)
            print(f"closed #{pr['number']}: nothing left to bump")
        return 0
    files = apply_pins(REPO, {m.name: m.new.commit for m in moves if m.name not in deps_resolve.LIBS})
    gl = {m.path: m.new.commit for m in moves}
    title = pr_title(moves)
    commit, tree = w.build_commit(base, gl, files, title)
    pushed = not (pr is not None and w.head_matches(pr["head"]["sha"], tree, base))
    recreated = False
    if pushed:
        recreated = w.update_branch(commit)
    else:
        print(f"#{pr['number']} already has this content on main; nothing pushed")
    pr = w.ensure_pr(title, pr_body(moves, held), fresh=recreated)
    # bump-failed belongs to a head commit (bump_report): a new head clears it, an unchanged one keeps it.
    w.labels(pr, add=["auto-bump"], remove=["bump-failed"] if pushed else [])
    if os.environ.get("APP_BUMP_AUTOMERGE") == "on":
        # Arm only a head this run created: a human who disarmed an unchanged PR stays obeyed.
        if pushed:
            w.enable_automerge(pr, commit)
    elif pr.get("auto_merge"):
        w.disable_automerge(pr)
        print(f"::notice::#{pr['number']}: auto-merge disarmed (APP_BUMP_AUTOMERGE is not on)")
    if alerting and not pushed and _stuck(gh, checks_gh, repo, pr):  # a head just replaced has not had time to run
        ci_alert.send("app bump: stuck", f"#{pr['number']} open > 24 h with no finished check (runner offline?)")
    print(f"#{pr['number']}: {title}")
    return 0


def _write(root: Path, texts: dict[str, str]) -> None:
    for rel, text in texts.items():
        (root / rel).write_text(text)
        print(f"updated {rel}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("repos")
    sub.add_parser("plan")
    p = sub.add_parser("pin")
    p.add_argument("apps", nargs="*")
    p.add_argument("--all", action="store_true")
    a = sub.add_parser("apply")
    a.add_argument("pins", nargs="+", metavar="ID=SHA")
    r = sub.add_parser("reconcile")
    r.add_argument("--repo", required=True)
    r.add_argument("--event", default="workflow_dispatch")
    b = sub.add_parser("assert-built")
    b.add_argument("--profile", required=True)
    b.add_argument("components")
    ns = ap.parse_args(argv)

    if ns.cmd == "check":
        problems = check(REPO)
        for line in problems:
            print(f"::error::{line}")
        return 1 if problems else 0
    if ns.cmd == "pin":
        links = gitlinks(REPO)
        ids = sorted({i for d in load_profiles(REPO).values() for i in d.get("apps", {})}) if ns.all else ns.apps
        missing = [i for i in ids if app_path(i) not in links]
        if missing:
            print(f"no gitlink for: {', '.join(missing)}")
            return 1
        _write(REPO, apply_pins(REPO, {i: links[app_path(i)] for i in ids}))
        return 0
    if ns.cmd == "apply":
        pins = dict(item.split("=", 1) for item in ns.pins)
        _write(REPO, apply_pins(REPO, pins))
        return 0
    if ns.cmd == "repos":
        try:
            print(",".join(repos_scope(REPO)))
        except BumpError as e:
            print(f"::error::{e}", file=sys.stderr)  # stdout is consumed as the token scope
            return 1
        return 0
    if ns.cmd == "plan":
        try:
            return cmd_plan()
        except (gh_api.ApiError, BumpError) as e:
            print(f"::error::{e}")
            return 1
    if ns.cmd == "reconcile":
        try:
            return cmd_reconcile(ns.repo, ns.event)
        except (gh_api.ApiError, BumpError) as e:
            print(f"::error::{e}")
            return 1
    doc = json.loads((REPO / PROFILE_REL / f"{ns.profile}.json").read_text())
    problems = assert_built(doc, Path(ns.components).read_text())
    for line in problems:
        print(f"::error::{ns.profile}: {line}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
