import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app_bump
import deps_resolve as dr
import gh_api
from test_app_bump import make_repo, git

A, B, BASE = "a" * 40, "b" * 40, "c" * 40


class FakeGH:
    def __init__(self, routes=None):
        self.routes = routes or {}
        self.calls = []
        self.queries = []
        self.graphql_error = None

    def _hit(self, method, path, body=None):
        assert path.startswith("/"), path  # gh_api refuses anything else
        self.calls.append((method, path, body))
        out = self.routes.get((method, path))
        if callable(out):  # a route whose answer changes between calls
            out = out()
        if isinstance(out, Exception):
            raise out
        return out

    def get(self, path):
        return self._hit("GET", path)

    def post(self, path, body):
        return self._hit("POST", path, body)

    def patch(self, path, body):
        return self._hit("PATCH", path, body)

    def delete(self, path):
        return self._hit("DELETE", path)

    def paginate(self, path):
        out = self._hit("GET", path)
        assert isinstance(out, list), "paginate handles bare-list endpoints only"
        yield from out

    def graphql(self, query, **v):
        self.calls.append(("GRAPHQL", query.split("(")[0].strip(), v))
        self.queries.append(query)
        if self.graphql_error:
            raise self.graphql_error
        return {}


def test_build_commit_writes_gitlinks_and_profiles_on_the_base_tree():
    gh = FakeGH({("GET", f"/repos/o/r/git/commits/{BASE}"): {"tree": {"sha": "T0"}},
                 ("POST", "/repos/o/r/git/trees"): {"sha": "T1"},
                 ("POST", "/repos/o/r/git/commits"): {"sha": "C1"}})
    w = app_bump.BumpWriter(gh, "o/r")
    assert w.build_commit(BASE, {"components/crosspad-mixer": B}, {"config/profiles/full.json": "{}\n"},
                          "chore(apps): bump mixer 0.2.0") == ("C1", "T1")
    tree = gh.calls[1][2]
    assert tree["base_tree"] == "T0"
    assert {"path": "components/crosspad-mixer", "mode": "160000", "type": "commit", "sha": B} in tree["tree"]
    assert {"path": "config/profiles/full.json", "mode": "100644", "type": "blob", "content": "{}\n"} in tree["tree"]
    assert gh.calls[2][2] == {"message": "chore(apps): bump mixer 0.2.0", "tree": "T1", "parents": [BASE]}


@pytest.mark.parametrize("pins,files", [
    ({"components/crosspad-mixer/../x": B}, {}),
    ({"main/main.cpp": B}, {}),
    ({"components/crosspad-mixer": "B" * 40}, {}),               # not a full lowercase sha
    ({"components/crosspad-mixer": B}, {"main/main.cpp": "x"}),
    ({"components/crosspad-mixer": B}, {"config/profiles/community/x.json": "x"}),
    ({"components/crosspad-mixer": B}, {".github/workflows/main.yml": "x"}),
])
def test_build_commit_refuses_anything_but_gitlinks_and_the_official_profiles(pins, files):
    gh = FakeGH()
    with pytest.raises(ValueError):
        app_bump.BumpWriter(gh, "o/r").build_commit(BASE, pins, files, "m")
    assert gh.calls == []  # refused before any API call


def test_build_commit_accepts_every_official_profile_not_just_two(tmp_path, monkeypatch):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}}, {})
    (root / "config" / "profiles" / "pro.json").write_text("{}\n")   # a third official kit
    monkeypatch.setattr(app_bump, "REPO", root)
    gh = FakeGH({("GET", f"/repos/o/r/git/commits/{BASE}"): {"tree": {"sha": "T0"}},
                 ("POST", "/repos/o/r/git/trees"): {"sha": "T1"},
                 ("POST", "/repos/o/r/git/commits"): {"sha": "C1"}})
    files = {f"config/profiles/{n}.json": "{}\n" for n in ("full", "lite", "pro")}
    assert app_bump.BumpWriter(gh, "o/r").build_commit(BASE, {"components/crosspad-mixer": B}, files, "m") == \
        ("C1", "T1")
    for path in ("config/profiles/max.json", "config/profiles/community/x.json"):   # not official here
        with pytest.raises(ValueError):
            app_bump.BumpWriter(FakeGH(), "o/r").build_commit(BASE, {}, {path: "x"}, "m")


def test_update_branch_creates_then_force_updates():
    missing = gh_api.ApiError(404, "GET", "x", "nf")
    gh = FakeGH({("GET", "/repos/o/r/git/ref/heads/auto/app-bump"): missing})
    app_bump.BumpWriter(gh, "o/r").update_branch("C1")
    assert ("POST", "/repos/o/r/git/refs", {"ref": "refs/heads/auto/app-bump", "sha": "C1"}) in gh.calls
    gh2 = FakeGH({("GET", "/repos/o/r/git/ref/heads/auto/app-bump"): {"object": {"sha": "C0"}}})
    app_bump.BumpWriter(gh2, "o/r").update_branch("C1")
    assert ("PATCH", "/repos/o/r/git/refs/heads/auto/app-bump", {"sha": "C1", "force": True}) in gh2.calls


def test_update_branch_does_not_swallow_other_errors():
    gh = FakeGH({("GET", "/repos/o/r/git/ref/heads/auto/app-bump"): gh_api.ApiError(500, "GET", "x", "boom")})
    with pytest.raises(gh_api.ApiError):
        app_bump.BumpWriter(gh, "o/r").update_branch("C1")


REF = "/repos/o/r/git/ref/heads/auto/app-bump"
REFS = "/repos/o/r/git/refs/heads/auto/app-bump"


@pytest.mark.parametrize("status", [403, 422])
def test_update_branch_recreates_the_branch_when_github_refuses_the_force_update(status, capsys):
    """A newer main that changed .github/workflows/* may make GitHub refuse the PATCH to an
    App without the workflows permission; a fresh ref (and so a fresh PR) is the way round."""
    gh = FakeGH({("GET", REF): {"object": {"sha": "C0"}},
                 ("PATCH", REFS): gh_api.ApiError(status, "PATCH", "x", "refusing to allow a GitHub App")})
    app_bump.BumpWriter(gh, "o/r").update_branch("C1")
    assert writes(gh) == [("PATCH", REFS, {"sha": "C1", "force": True}),
                          ("DELETE", REFS, None),
                          ("POST", "/repos/o/r/git/refs", {"ref": "refs/heads/auto/app-bump", "sha": "C1"})]
    out = capsys.readouterr().out
    assert out.startswith("::notice::") and str(status) in out


@pytest.mark.parametrize("status", [403, 422])
def test_update_branch_reports_that_it_recreated_the_branch(status):
    gh = FakeGH({("GET", REF): {"object": {"sha": "C0"}},
                 ("PATCH", REFS): gh_api.ApiError(status, "PATCH", "x", "refusing")})
    assert app_bump.BumpWriter(gh, "o/r").update_branch("C1") is True


def test_update_branch_reports_false_for_a_normal_force_update_and_for_a_missing_branch():
    gh = FakeGH({("GET", REF): {"object": {"sha": "C0"}}})
    assert app_bump.BumpWriter(gh, "o/r").update_branch("C1") is False
    missing = FakeGH({("GET", REF): gh_api.ApiError(404, "GET", "x", "nf")})
    assert app_bump.BumpWriter(missing, "o/r").update_branch("C1") is False


def test_update_branch_raises_on_any_other_patch_error_and_deletes_nothing():
    gh = FakeGH({("GET", REF): {"object": {"sha": "C0"}},
                 ("PATCH", REFS): gh_api.ApiError(500, "PATCH", "x", "boom")})
    with pytest.raises(gh_api.ApiError):
        app_bump.BumpWriter(gh, "o/r").update_branch("C1")
    assert writes(gh) == [("PATCH", REFS, {"sha": "C1", "force": True})]


def test_update_branch_fallback_stops_if_the_delete_fails():
    gh = FakeGH({("GET", REF): {"object": {"sha": "C0"}},
                 ("PATCH", REFS): gh_api.ApiError(422, "PATCH", "x", ""),
                 ("DELETE", REFS): gh_api.ApiError(500, "DELETE", "x", "boom")})
    with pytest.raises(gh_api.ApiError):
        app_bump.BumpWriter(gh, "o/r").update_branch("C1")
    assert not any(c[0] == "POST" for c in gh.calls)


def test_unchanged_head_needs_no_push():
    gh = FakeGH({("GET", "/repos/o/r/git/commits/C0"): {"tree": {"sha": "T1"}, "parents": [{"sha": BASE}]}})
    w = app_bump.BumpWriter(gh, "o/r")
    assert w.head_matches("C0", "T1", BASE) is True
    assert w.head_matches("C0", "T2", BASE) is False


def test_head_with_extra_parents_is_not_a_match():
    gh = FakeGH({("GET", "/repos/o/r/git/commits/C0"): {"tree": {"sha": "T1"},
                                                          "parents": [{"sha": BASE}, {"sha": A}]}})
    assert app_bump.BumpWriter(gh, "o/r").head_matches("C0", "T1", BASE) is False


def test_ensure_pr_creates_with_label_and_automerge_is_graphql():
    gh = FakeGH({("GET", "/repos/o/r/pulls?head=o:auto/app-bump&state=open"): [],
                 ("POST", "/repos/o/r/pulls"): {"number": 7, "node_id": "PR_7"}})
    w = app_bump.BumpWriter(gh, "o/r")
    pr = w.ensure_pr("chore(apps): bump mixer 0.2.0", "body")
    w.labels(pr, add=["auto-bump"], remove=["bump-failed"])
    w.enable_automerge(pr, "C1")
    assert "expectedHeadOid: $oid" in gh.queries[0] and "$oid: GitObjectID!" in gh.queries[0]
    assert ("POST", "/repos/o/r/issues/7/labels", {"labels": ["auto-bump"]}) in gh.calls
    assert ("DELETE", "/repos/o/r/issues/7/labels/bump-failed", None) in gh.calls
    assert ("GRAPHQL", "mutation", {"id": "PR_7", "oid": "C1"}) in gh.calls


BOT_PR = {"number": 7, "node_id": "PR_7", "user": {"login": "crosspad-bot[bot]"}, "base": {"ref": "main"},
          "head": {"sha": "C0"}, "created_at": "2020-01-01T00:00:00Z"}


def test_ensure_pr_updates_the_open_one():
    gh = FakeGH({("GET", "/repos/o/r/pulls?head=o:auto/app-bump&state=open"): [BOT_PR]})
    pr = app_bump.BumpWriter(gh, "o/r").ensure_pr("t", "b")
    assert pr == BOT_PR
    assert ("PATCH", "/repos/o/r/pulls/7", {"title": "t", "body": "b"}) in gh.calls
    assert not any(c[0] == "POST" for c in gh.calls)


def test_ensure_pr_fresh_opens_a_new_pr_without_listing_the_old_ones():
    gh = FakeGH({("GET", "/repos/o/r/pulls?head=o:auto/app-bump&state=open"): [BOT_PR],
                 ("POST", "/repos/o/r/pulls"): {"number": 8, "node_id": "PR_8"}})
    pr = app_bump.BumpWriter(gh, "o/r").ensure_pr("t", "b", fresh=True)
    assert pr == {"number": 8, "node_id": "PR_8"}
    assert gh.calls == [("POST", "/repos/o/r/pulls",
                         {"title": "t", "head": "auto/app-bump", "base": "main", "body": "b"})]


def test_close_closes_and_deletes_the_branch():
    gh = FakeGH()
    app_bump.BumpWriter(gh, "o/r").close({"number": 7})
    assert ("PATCH", "/repos/o/r/pulls/7", {"state": "closed"}) in gh.calls
    assert ("DELETE", "/repos/o/r/git/refs/heads/auto/app-bump", None) in gh.calls


def test_open_pr_adopts_only_the_bots_pr_against_main():
    url = "/repos/o/r/pulls?head=o:auto/app-bump&state=open"
    human = {"number": 3, "user": {"login": "matixan"}, "base": {"ref": "main"}}
    other_base = dict(BOT_PR, base={"ref": "release"})
    assert app_bump.BumpWriter(FakeGH({("GET", url): [human, BOT_PR]}), "o/r").open_pr() == BOT_PR
    assert app_bump.BumpWriter(FakeGH({("GET", url): []}), "o/r").open_pr() is None
    for prs in ([human], [other_base]):  # not ours: stop rather than force-push over it
        gh = FakeGH({("GET", url): prs})
        with pytest.raises(app_bump.BumpError):
            app_bump.BumpWriter(gh, "o/r").open_pr()


def test_branch_writes_refuse_any_other_ref(monkeypatch):
    monkeypatch.setattr(app_bump, "BRANCH", "main")
    gh = FakeGH()
    w = app_bump.BumpWriter(gh, "o/r")
    with pytest.raises(ValueError):
        w.update_branch("C1")
    with pytest.raises(ValueError):
        w.close({"number": 7})
    assert gh.calls == []


def test_enable_automerge_tolerates_an_already_clean_pr_but_not_other_errors(capsys):
    gh = FakeGH()
    gh.graphql_error = gh_api.ApiError(200, "POST", "/graphql", '[{"message": "Pull request is in clean status"}]')
    app_bump.BumpWriter(gh, "o/r").enable_automerge({"number": 7, "node_id": "PR_7"}, "C1")
    assert "::notice::" in capsys.readouterr().out
    gh.graphql_error = gh_api.ApiError(200, "POST", "/graphql", '[{"message": "Auto merge is not allowed"}]')
    with pytest.raises(gh_api.ApiError):
        app_bump.BumpWriter(gh, "o/r").enable_automerge({"number": 7, "node_id": "PR_7"}, "C1")


def test_title_and_body():
    old, new = dr.Release((0, 12, 0), A, None, {}), dr.Release((0, 12, 1), B, "v0.12.1", {})
    moves = [app_bump.Move("mixer", "mixer", "components/crosspad-mixer", "CrossPad/crosspad-mixer", old, new)]
    assert app_bump.pr_title(moves) == "chore(deps): mixer 0.12.0→0.12.1"
    body = app_bump.pr_body(moves, [])
    assert f"https://github.com/CrossPad/crosspad-mixer/compare/{A[:12]}...{B[:12]}" in body


def gitmodules_repo(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}},
                     links={"mixer": A, "core": "1" * 40, "gui": "2" * 40})
    (root / ".gitmodules").write_text('[submodule "m"]\n\tpath = components/crosspad-mixer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-mixer.git\n'
                                      '[submodule "c"]\n\tpath = components/crosspad-core\n'
                                      '\turl = https://github.com/CrossPad/crosspad-core.git\n'
                                      '[submodule "g"]\n\tpath = components/crosspad-gui\n'
                                      '\turl = https://github.com/CrossPad/crosspad-gui.git\n')
    return root


# --- APP_BUMP_HOLD ---------------------------------------------------------

def test_parse_hold_strips_splits_lowercases_and_ignores_empties():
    hold, notes = app_bump.parse_hold(" Mixer , ,song,,  ", ["mixer", "song", "sampler"])
    assert hold == {"mixer", "song"} and notes == []
    assert app_bump.parse_hold("", ["mixer"]) == (set(), [])
    assert app_bump.parse_hold(None, ["mixer"]) == (set(), [])


def test_parse_hold_notes_ids_that_match_no_official_app():
    hold, notes = app_bump.parse_hold("mixer, Nope,x\ny", ["mixer"])
    assert hold == {"mixer", "nope", "x\ny"}
    assert len(notes) == 2 and all("no official app" in n and "\n" not in n for n in notes)
    assert any("nope" in n for n in notes) and not any("mixer" in n for n in notes)


# --- repos fails closed ----------------------------------------------------

def test_submodule_repos_raises_on_git_config_failure(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text("[submodule \"m\"\n\tbroken")
    with pytest.raises(app_bump.BumpError):
        app_bump.submodule_repos(root)
    (root / ".gitmodules").unlink()
    with pytest.raises(app_bump.BumpError):
        app_bump.submodule_repos(root)


def test_submodule_repos_empty_gitmodules_is_empty(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text("")
    assert app_bump.submodule_repos(root) == {}


def run_repos(root, monkeypatch, capsys):
    """(rc, stdout, stderr); on failure stdout must stay empty: Task 16 reads it as the token scope."""
    monkeypatch.setattr(app_bump, "REPO", root)
    rc = app_bump.main(["repos"])
    out, err = capsys.readouterr()
    if rc != 0:
        assert out == ""
    return rc, out, err


def test_repos_fails_when_an_official_app_has_no_submodule_entry(tmp_path, monkeypatch, capsys):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}, "song": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text('[submodule "m"]\n\tpath = components/crosspad-mixer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-mixer.git\n')
    rc, out, err = run_repos(root, monkeypatch, capsys)
    assert rc == 1 and err.startswith("::error::") and "song" in err


def test_repos_fails_when_gitmodules_is_missing_or_broken(tmp_path, monkeypatch, capsys):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}})
    rc, out, err = run_repos(root, monkeypatch, capsys)
    assert rc == 1 and err.startswith("::error::") and err.count("\n") == 1
    (root / ".gitmodules").write_text("[submodule \"m\"\n\tbroken")
    rc, out, err = run_repos(root, monkeypatch, capsys)
    assert rc == 1 and err.startswith("::error::")


def test_repos_fails_when_there_are_no_official_apps(tmp_path, monkeypatch, capsys):
    root = make_repo(tmp_path, {})
    (root / ".gitmodules").write_text("")
    rc, out, err = run_repos(root, monkeypatch, capsys)
    assert rc == 1 and err.startswith("::error::")


def test_repos_never_prints_an_empty_list_on_stdout(tmp_path, monkeypatch, capsys):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text('[submodule "x"]\n\tpath = components/x\n\turl = https://example.com/a/b.git\n')
    rc, out, err = run_repos(root, monkeypatch, capsys)
    assert rc == 1 and out == "" and err.startswith("::error::")


def test_repos_prints_the_list_on_stdout_when_all_is_well(tmp_path, monkeypatch, capsys):
    root = gitmodules_repo(tmp_path)
    assert run_repos(root, monkeypatch, capsys) == (0, "crosspad-core,crosspad-gui,crosspad-mixer\n", "")


# --- reconcile -------------------------------------------------------------

CG, GG = "1" * 40, "2" * 40


def options(newer_req=None, newer=True, skipped=()):
    apps = {"mixer": dr.Options(dr.Release((0, 1, 0), A, None, {}),
                                [dr.Release((0, 2, 0), B, "v0.2.0", newer_req or {})] if newer else [])}
    libs = {dr.CORE: dr.Options(dr.Release((1, 53, 0), CG, None, {}), []),
            dr.GUI: dr.Options(dr.Release((1, 23, 0), GG, None, {dr.CORE: "^1.53.0"}), [])}
    return apps, libs, [], [], list(skipped)


ORIG_COLLECT = app_bump.collect_options
SLOT = 1791696600.0   # 2026-10-11 05:30:00 UTC, inside the alert hour
OLD = "2026-10-09T00:00:00Z"   # > 24 h before SLOT
RECENT = "2026-10-11T04:00:00Z"
WRITES = ("POST", "PATCH", "DELETE", "GRAPHQL")


@pytest.fixture
def env(tmp_path, monkeypatch):
    root = gitmodules_repo(tmp_path)
    head = git(root, "rev-parse", "HEAD").strip()
    monkeypatch.setattr(app_bump, "REPO", root)
    for k in ("APP_BUMP_HOLD", "APP_BUMP_AUTOMERGE", "GITHUB_REF"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setenv("APP_READ_TOKEN", "read")
    monkeypatch.setenv("BUMP_TOKEN", "write")
    monkeypatch.setenv("CHECKS_TOKEN", "checks")
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT)
    alerts = []
    import ci_alert
    monkeypatch.setattr(ci_alert, "send", lambda *a, **k: alerts.append(a))
    monkeypatch.setattr(app_bump, "collect_options", lambda read_gh, r, hold: options())
    return root, head, alerts


def install_gh(monkeypatch, gh, read=None, checks=None):
    clients = {"write": gh, "read": read or FakeGH(), "checks": checks or FakeGH()}
    monkeypatch.setattr(gh_api, "GitHub", lambda token, *a, **k: clients[token])


def commit_route(tree, parents, date=OLD):
    return {"tree": {"sha": tree}, "parents": [{"sha": p} for p in parents], "committer": {"date": date}}


def write_routes(head, open_prs=()):
    return {("GET", "/repos/o/r/git/ref/heads/main"): {"object": {"sha": head}},
            ("GET", f"/repos/o/r/git/commits/{head}"): {"tree": {"sha": "T0"}},
            ("POST", "/repos/o/r/git/trees"): {"sha": "T1"},
            ("POST", "/repos/o/r/git/commits"): {"sha": "C1"},
            ("GET", "/repos/o/r/git/ref/heads/auto/app-bump"): gh_api.ApiError(404, "GET", "x", ""),
            ("GET", "/repos/o/r/pulls?head=o:auto/app-bump&state=open"): list(open_prs),
            ("POST", "/repos/o/r/pulls"): dict(BOT_PR, head={"sha": "C1"}, created_at="2026-10-11T05:30:00Z")}


def open_pr_routes(head, tree="T1", date=OLD, **pr_over):
    routes = write_routes(head, open_prs=[dict(BOT_PR, **pr_over)])
    routes[("GET", "/repos/o/r/git/commits/C0")] = commit_route(tree, [head], date)
    return routes


def writes(gh):
    return [c for c in gh.calls if c[0] in WRITES]


def test_reconcile_opens_one_commit_pr_touching_only_gitlinks_and_profiles(env, monkeypatch):
    root, head, _ = env
    gh = FakeGH(write_routes(head))
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    [trees] = [c for c in gh.calls if c[:2] == ("POST", "/repos/o/r/git/trees")]
    paths = {e["path"] for e in trees[2]["tree"]}
    assert paths == {"components/crosspad-mixer", "config/profiles/full.json"}
    [commit] = [c for c in gh.calls if c[:2] == ("POST", "/repos/o/r/git/commits")]
    assert commit[2]["parents"] == [head]
    full = [e for e in trees[2]["tree"] if e["path"].endswith("full.json")][0]["content"]
    assert json.loads(full)["apps"]["mixer"] == {"track": "pinned", "commit": B}
    assert ("POST", "/repos/o/r/git/refs", {"ref": "refs/heads/auto/app-bump", "sha": "C1"}) in gh.calls
    assert ("POST", "/repos/o/r/issues/7/labels", {"labels": ["auto-bump"]}) in gh.calls
    assert not any(c[0] == "GRAPHQL" for c in gh.calls)  # APP_BUMP_AUTOMERGE unset


def test_automerge_is_armed_on_a_new_pr_and_on_a_push_when_on(env, monkeypatch):
    root, head, _ = env
    monkeypatch.setenv("APP_BUMP_AUTOMERGE", "on")
    gh = FakeGH(write_routes(head))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    # armed on the commit this run created, so GitHub refuses a head that moved afterwards
    assert ("GRAPHQL", "mutation", {"id": "PR_7", "oid": "C1"}) in gh.calls
    assert "enablePullRequestAutoMerge" in gh.queries[0]
    gh2 = FakeGH(open_pr_routes(head, tree="T-old"))  # open PR with other content: this run pushes
    install_gh(monkeypatch, gh2)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert len(gh2.queries) == 1 and "enablePullRequestAutoMerge" in gh2.queries[0]


def test_automerge_is_not_rearmed_when_nothing_was_pushed(env, monkeypatch):
    root, head, _ = env
    monkeypatch.setenv("APP_BUMP_AUTOMERGE", "on")
    gh = FakeGH(open_pr_routes(head))  # head already has this content
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert gh.queries == [] and not any(c[1].startswith("/repos/o/r/git/refs") for c in gh.calls)


def test_automerge_is_disarmed_when_off_and_left_alone_when_not_armed(env, monkeypatch):
    root, head, _ = env
    gh = FakeGH(open_pr_routes(head, auto_merge={"merge_method": "squash"}))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert len(gh.queries) == 1 and "disablePullRequestAutoMerge" in gh.queries[0]
    assert ("GRAPHQL", "mutation", {"id": "PR_7"}) in gh.calls
    monkeypatch.setenv("APP_BUMP_AUTOMERGE", "off")
    gh2 = FakeGH(open_pr_routes(head, auto_merge=None))
    install_gh(monkeypatch, gh2)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert gh2.queries == []


def test_reconcile_with_nothing_to_bump_closes_the_open_pr(env, monkeypatch):
    root, head, _ = env
    monkeypatch.setattr(app_bump, "collect_options", lambda read_gh, r, hold: options(newer=False))
    gh = FakeGH(write_routes(head, open_prs=[BOT_PR]))
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    assert ("PATCH", "/repos/o/r/pulls/7", {"state": "closed"}) in gh.calls
    assert not any(c[:2] == ("POST", "/repos/o/r/git/commits") for c in gh.calls)


def test_reconcile_hold_blocks_the_pin(env, monkeypatch, capsys):
    root, head, _ = env
    monkeypatch.setenv("APP_BUMP_HOLD", " MIXER, ghost ")
    holds = []

    def collect(read_gh, r, hold):   # collect_options drops a held component's newer releases (its own tests)
        holds.append(hold)
        return (*options(newer=False)[:2], ["mixer: held (APP_BUMP_HOLD)"], [], [])

    monkeypatch.setattr(app_bump, "collect_options", collect)
    gh = FakeGH(write_routes(head))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    out = capsys.readouterr().out
    assert holds == [{"mixer", "ghost"}] and "held" in out and "ghost" in out
    assert writes(gh) == []


def test_main_moved_writes_nothing_even_when_there_is_nothing_to_bump(env, monkeypatch, capsys):
    root, head, _ = env
    monkeypatch.setattr(app_bump, "collect_options", lambda read_gh, r, hold: options(newer=False))
    gh = FakeGH(write_routes("d" * 40, open_prs=[BOT_PR]))
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    assert writes(gh) == []  # the close path is behind the guard too
    out = capsys.readouterr().out
    assert "main moved" in out and "::notice::" not in out


def test_a_checkout_that_is_not_main_says_so(env, monkeypatch, capsys):
    root, head, _ = env
    monkeypatch.setenv("GITHUB_REF", "refs/heads/feature/x")
    gh = FakeGH(write_routes("d" * 40))
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    out = capsys.readouterr().out
    assert out.startswith("::notice::checkout is refs") and "main moved" not in out
    assert writes(gh) == []


def test_reconcile_skips_the_push_when_the_open_pr_already_has_the_content(env, monkeypatch):
    root, head, _ = env
    gh = FakeGH(open_pr_routes(head))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert not any(c[1].startswith("/repos/o/r/git/refs") for c in gh.calls)


def test_reconcile_force_pushes_a_pr_head_that_is_not_the_single_bot_commit(env, monkeypatch):
    root, head, _ = env
    routes = open_pr_routes(head)
    routes[("GET", "/repos/o/r/git/commits/C0")] = commit_route("T1", [head, A])
    routes[("GET", "/repos/o/r/git/ref/heads/auto/app-bump")] = {"object": {"sha": "C0"}}
    gh = FakeGH(routes)
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert ("PATCH", "/repos/o/r/git/refs/heads/auto/app-bump", {"sha": "C1", "force": True}) in gh.calls


def test_reconcile_recreates_the_branch_and_opens_a_new_pr_when_the_force_update_is_refused(env, monkeypatch):
    root, head, _ = env
    monkeypatch.setenv("APP_BUMP_AUTOMERGE", "on")
    routes = open_pr_routes(head, tree="T-old")
    routes[("GET", REF)] = {"object": {"sha": "C0"}}
    routes[("PATCH", REFS)] = gh_api.ApiError(422, "PATCH", "x", "refusing to allow a GitHub App")
    gh = FakeGH(routes)
    routes[("POST", "/repos/o/r/pulls")] = dict(BOT_PR, number=8, node_id="PR_8", head={"sha": "C1"})
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    w = writes(gh)
    assert w.index(("DELETE", REFS, None)) < w.index(
        ("POST", "/repos/o/r/git/refs", {"ref": "refs/heads/auto/app-bump", "sha": "C1"}))
    [new_pr] = [c for c in w if c[:2] == ("POST", "/repos/o/r/pulls")]
    assert new_pr[2]["head"] == "auto/app-bump" and new_pr[2]["base"] == "main"
    assert not any(c[:2] == ("PATCH", "/repos/o/r/pulls/7") for c in w)   # the old PR is not edited
    assert ("POST", "/repos/o/r/issues/8/labels", {"labels": ["auto-bump"]}) in w
    assert ("GRAPHQL", "mutation", {"id": "PR_8", "oid": "C1"}) in w


def test_reconcile_opens_a_new_pr_even_when_the_listing_still_shows_the_old_one(env, monkeypatch):
    """GitHub closes the old PR asynchronously after the branch delete: a listing right after
    can still return it. A recreated branch must never reuse that PR."""
    root, head, _ = env
    monkeypatch.setenv("APP_BUMP_AUTOMERGE", "on")
    routes = open_pr_routes(head, tree="T-old")   # the listing keeps answering with BOT_PR (#7)
    routes[("GET", REF)] = {"object": {"sha": "C0"}}
    routes[("PATCH", REFS)] = gh_api.ApiError(403, "PATCH", "x", "refusing to allow a GitHub App")
    routes[("POST", "/repos/o/r/pulls")] = dict(BOT_PR, number=8, node_id="PR_8", head={"sha": "C1"})
    gh = FakeGH(routes)
    install_gh(monkeypatch, gh)
    assert app_bump.cmd_reconcile("o/r", "workflow_dispatch") == 0
    w = writes(gh)
    assert ("DELETE", REFS, None) in w
    assert len([c for c in w if c[:2] == ("POST", "/repos/o/r/pulls")]) == 1
    assert not any(c[:2] == ("PATCH", "/repos/o/r/pulls/7") for c in w)
    assert not any(c[1].startswith("/repos/o/r/issues/7/") for c in w)
    assert ("POST", "/repos/o/r/issues/8/labels", {"labels": ["auto-bump"]}) in w
    assert ("GRAPHQL", "mutation", {"id": "PR_8", "oid": "C1"}) in w


def test_bump_failed_is_cleared_only_when_this_run_pushed_a_new_head(env, monkeypatch):
    root, head, _ = env
    clear = ("DELETE", "/repos/o/r/issues/7/labels/bump-failed", None)
    gh = FakeGH(open_pr_routes(head))   # head already has this content: the red build still stands
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert clear not in gh.calls
    assert ("POST", "/repos/o/r/issues/7/labels", {"labels": ["auto-bump"]}) in gh.calls
    gh2 = FakeGH(open_pr_routes(head, tree="T-old"))   # a new head: the old failure no longer applies
    install_gh(monkeypatch, gh2)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert clear in gh2.calls


def test_reconcile_refuses_a_foreign_pr_on_the_bump_branch_before_any_write(env, monkeypatch):
    root, head, _ = env
    human = {"number": 3, "user": {"login": "matixan"}, "base": {"ref": "main"}, "head": {"sha": "H"}}
    gh = FakeGH(write_routes(head, open_prs=[human]))
    install_gh(monkeypatch, gh)
    with pytest.raises(app_bump.BumpError):
        app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert writes(gh) == []
    assert app_bump.main(["reconcile", "--repo", "o/r"]) == 1


# alerts: once a day per condition, by slot

def test_diverged_alert_only_in_the_scheduled_slot(env, monkeypatch):
    root, head, alerts = env
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda read_gh, r, hold: (*options(newer=False)[:2], [], ["mixer"], []))
    install_gh(monkeypatch, FakeGH(write_routes(head)))
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")   # manual run in the slot: no alert
    assert alerts == []
    app_bump.cmd_reconcile("o/r", "schedule")
    assert len(alerts) == 1 and "mixer" in alerts[0][1]
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT + 3600)   # 06:30, the next hourly run
    app_bump.cmd_reconcile("o/r", "schedule")
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT - 3600)   # 04:30
    app_bump.cmd_reconcile("o/r", "schedule")
    assert len(alerts) == 1


def test_hourly_schedule_over_a_day_alerts_once(env, monkeypatch):
    root, head, alerts = env
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda read_gh, r, hold: (*options(newer=False)[:2], [], ["mixer"], []))
    install_gh(monkeypatch, FakeGH(write_routes(head)))
    for h in range(24):
        monkeypatch.setattr(app_bump, "_now", lambda h=h: SLOT - 5.5 * 3600 + h * 3600)
        app_bump.cmd_reconcile("o/r", "schedule")
    assert len(alerts) == 1


CHECK_RUNS = ("GET", "/repos/o/r/commits/C0/check-runs")


def checks_gh(runs):
    return FakeGH({CHECK_RUNS: {"check_runs": runs}})


def test_stuck_alert_ages_from_the_head_commit_not_the_pr(env, monkeypatch):
    root, head, alerts = env
    gh = FakeGH(open_pr_routes(head, date=OLD))   # PR created 2020, head commit 2 days old, no finished check
    checks = checks_gh([{"conclusion": None}])
    install_gh(monkeypatch, gh, checks=checks)
    app_bump.cmd_reconcile("o/r", "schedule")
    assert [a[0] for a in alerts] == ["app bump: stuck"]
    # check runs are read with the job's GITHUB_TOKEN (CHECKS_TOKEN): the App has no Checks permission
    assert checks.calls == [CHECK_RUNS + (None,)]
    assert not any("check-runs" in c[1] for c in gh.calls)
    alerts.clear()
    fresh = open_pr_routes(head, date=RECENT)   # PR is years old, but this commit is 1.5 h old
    install_gh(monkeypatch, FakeGH(fresh), checks=checks_gh([]))
    app_bump.cmd_reconcile("o/r", "schedule")
    assert alerts == []


def test_stuck_reads_the_commit_with_the_bump_token_and_check_runs_with_the_checks_token(monkeypatch):
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT)
    gh = FakeGH({("GET", "/repos/o/r/git/commits/C0"): commit_route("T1", [BASE], OLD)})
    pr = dict(BOT_PR, head={"sha": "C0"})
    assert app_bump._stuck(gh, checks_gh([]), "o/r", pr) is True
    assert app_bump._stuck(gh, checks_gh([{"conclusion": "failure"}]), "o/r", pr) is False
    assert all("check-runs" not in c[1] for c in gh.calls)


def test_stuck_alert_needs_the_slot_no_finished_check_and_an_unchanged_head(env, monkeypatch):
    root, head, alerts = env
    base = open_pr_routes(head, date=OLD)
    install_gh(monkeypatch, FakeGH(base), checks=checks_gh([{"conclusion": "success"}]))
    app_bump.cmd_reconcile("o/r", "schedule")             # a finished check
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT + 3600)
    checks = checks_gh([])
    install_gh(monkeypatch, FakeGH(base), checks=checks)
    app_bump.cmd_reconcile("o/r", "schedule")             # outside the slot
    assert checks.calls == []
    monkeypatch.setattr(app_bump, "_now", lambda: SLOT)
    base[("GET", "/repos/o/r/git/commits/C0")] = commit_route("T9", [head], OLD)
    base[("GET", "/repos/o/r/git/ref/heads/auto/app-bump")] = {"object": {"sha": "C0"}}
    install_gh(monkeypatch, FakeGH(base), checks=checks_gh([]))
    app_bump.cmd_reconcile("o/r", "schedule")             # we just replaced the head
    assert alerts == []


# a transient read error must stop the run before any write

@pytest.mark.parametrize("status", [500, 502, 403, 429])
@pytest.mark.parametrize("what", ["relation", "contents"])
def test_transient_read_errors_raise_and_write_nothing(env, monkeypatch, status, what):
    root, head, _ = env
    monkeypatch.setattr(app_bump, "collect_options", ORIG_COLLECT)
    from test_app_bump_options import routes, eligible, manifest   # here: that module imports FakeGH from this one
    mix = "CrossPad/crosspad-mixer"
    manifest_b = ("GET", f"/repos/{mix}/contents/crosspad-app.json?ref={B}")
    read = routes(app_tags=[("v0.2.0", B)], extra={**eligible(mix, A, B), manifest_b: manifest("0.2.0")})
    key = ("GET", f"/repos/{mix}/compare/{A}...{B}") if what == "relation" else manifest_b
    read[key] = gh_api.ApiError(status, "GET", "x", "boom")
    gh = FakeGH(write_routes(head, open_prs=[BOT_PR]))   # a PR is open: the failure must not close it
    install_gh(monkeypatch, gh, read=FakeGH(read))
    with pytest.raises(gh_api.ApiError):
        app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    assert writes(gh) == []
    assert app_bump.main(["reconcile", "--repo", "o/r"]) == 1


@pytest.mark.parametrize("status", [404, 422])
def test_no_merge_base_is_an_unknown_relation_not_an_error(status):
    gh = FakeGH({("GET", f"/repos/o/r/compare/{A}...{B}"): gh_api.ApiError(status, "GET", "x", "")})
    assert app_bump._relation(gh, "o/r", A, B) is None


def test_reconcile_rejects_a_malformed_repo(env, monkeypatch):
    install_gh(monkeypatch, FakeGH())
    with pytest.raises(ValueError):
        app_bump.cmd_reconcile("o/r/../x", "workflow_dispatch")


def test_held_alert_only_in_the_scheduled_slot(env, monkeypatch):
    root, head, alerts = env
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda read_gh, r, hold: options({"crosspad-core": ">=2.0.0"}))
    install_gh(monkeypatch, FakeGH(write_routes(head)))
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")   # manual run in the slot: no alert
    assert alerts == []
    app_bump.cmd_reconcile("o/r", "schedule")
    assert alerts == [("app bump: held", "mixer 0.2.0: needs crosspad-core >=2.0.0, has 1.53.0")]


def test_reconcile_moves_core_with_the_app(env, monkeypatch):
    root, head, _ = env
    apps, libs, *_ = options({"crosspad-core": ">=1.60.0"})
    libs[dr.CORE] = dr.Options(libs[dr.CORE].pinned, [dr.Release((1, 60, 0), "3" * 40, "v1.60.0", {})])
    monkeypatch.setattr(app_bump, "collect_options", lambda read_gh, r, hold: (apps, libs, [], [], []))
    gh = FakeGH(write_routes(head))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    tree = next(body for m, path, body in gh.calls if (m, path) == ("POST", "/repos/o/r/git/trees"))
    links = {e["path"]: e["sha"] for e in tree["tree"] if e["mode"] == "160000"}
    assert links == {"components/crosspad-core": "3" * 40, "components/crosspad-mixer": B}
    commit = next(body for m, path, body in gh.calls if (m, path) == ("POST", "/repos/o/r/git/commits"))
    assert commit["message"] == "chore(deps): core 1.53.0→1.60.0, mixer 0.1.0→0.2.0"


def test_untrusted_text_is_one_line_before_it_is_printed_or_sent(env, monkeypatch, capsys):
    root, head, alerts = env
    hostile = "mixer 0.2.0: needs crosspad-core >=2.0.0\n::error::x"
    monkeypatch.setattr(dr, "solve", lambda apps, libs: dr.Solution(
        {"mixer": apps["mixer"].pinned, dr.CORE: libs[dr.CORE].pinned, dr.GUI: libs[dr.GUI].pinned}, [hostile]))
    monkeypatch.setattr(app_bump, "collect_options", lambda read_gh, r, hold: (
        *options(newer=False)[:2], ["mixer: range\n::error::y"], [], []))
    install_gh(monkeypatch, FakeGH(write_routes(head)))
    app_bump.cmd_reconcile("o/r", "schedule")
    assert alerts == [("app bump: held", "mixer 0.2.0: needs crosspad-core >=2.0.0?::error::x")]
    out = capsys.readouterr().out
    assert "\n::error::" not in out and "mixer: range?::error::y" in out


def test_plan_command_prints_and_writes_nothing(monkeypatch, capsys):
    from test_app_bump_plan import setup_plan
    apps, libs, repos = setup_plan()
    monkeypatch.setenv("APP_READ_TOKEN", "t")
    monkeypatch.delenv("APP_BUMP_HOLD", raising=False)
    monkeypatch.setattr(app_bump.gh_api, "GitHub", lambda token: FakeGH())
    monkeypatch.setattr(app_bump, "official_apps", lambda root: ["slicer", "song"])
    monkeypatch.setattr(app_bump, "collect_options", lambda gh, root, hold: (apps, libs, ["n1"], [], []))
    monkeypatch.setattr(app_bump, "submodule_repos", lambda root: {
        "components/crosspad-slicer": repos["slicer"], "components/crosspad-song": repos["song"],
        "components/crosspad-core": repos[dr.CORE], "components/crosspad-gui": repos[dr.GUI]})
    assert app_bump.main(["plan"]) == 0
    out = capsys.readouterr().out
    assert "n1" in out and "chore(deps): core 1.53.0→1.60.0, slicer 0.3.1→0.4.0" in out


def test_a_pinned_set_that_breaks_a_range_stops_the_run_before_any_write(env, monkeypatch, capsys):
    root, head, _ = env
    monkeypatch.setattr(dr, "solve", lambda apps, libs: (_ for _ in ()).throw(dr.Unsatisfiable("gui: a\n::error::z")))
    gh = FakeGH(write_routes(head, open_prs=[BOT_PR]))
    install_gh(monkeypatch, gh)
    assert app_bump.main(["reconcile", "--repo", "o/r"]) == 1
    assert writes(gh) == []
    out = capsys.readouterr().out
    assert "fix main first: gui: a?::error::z" in out and "\n::error::z" not in out


def test_skipped_tags_join_the_held_alert_bounded_and_on_one_line(env, monkeypatch):
    root, head, alerts = env
    skipped = ["mixer v0.2.0: unreadable version range: '>=1.2.0 <2.0.0'", "mixer v0.3.0: " + "y" * 500]
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda read_gh, r, hold: options({"crosspad-core": ">=2.0.0"}, skipped=skipped))
    install_gh(monkeypatch, FakeGH(write_routes(head)))
    app_bump.cmd_reconcile("o/r", "schedule")
    [(title, text)] = alerts
    held = text.split("\n")
    assert held[0] == "mixer 0.2.0: needs crosspad-core >=2.0.0, has 1.53.0"
    assert held[1] == skipped[0] and len(held) == 3 and len(held[2]) == app_bump.REASON_MAX


def test_skipped_tags_are_listed_as_held_back_in_the_pr_body(env, monkeypatch, capsys):
    root, head, alerts = env
    skipped = ["mixer v0.3.0: bad\n::error::z"]
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda read_gh, r, hold: options(skipped=skipped))   # a move exists: a PR is opened
    gh = FakeGH(write_routes(head))
    install_gh(monkeypatch, gh)
    app_bump.cmd_reconcile("o/r", "workflow_dispatch")
    [pr] = [c for c in gh.calls if c[:2] == ("POST", "/repos/o/r/pulls")]
    assert "- mixer v0.3.0: bad?::error::z" in pr[2]["body"].split("\n")


def test_plan_command_prints_skipped_tags_as_held(monkeypatch, capsys):
    from test_app_bump_plan import setup_plan
    apps, libs, repos = setup_plan()
    monkeypatch.setenv("APP_READ_TOKEN", "t")
    monkeypatch.delenv("APP_BUMP_HOLD", raising=False)
    monkeypatch.setattr(app_bump.gh_api, "GitHub", lambda token: FakeGH())
    monkeypatch.setattr(app_bump, "official_apps", lambda root: ["slicer", "song"])
    monkeypatch.setattr(app_bump, "collect_options",
                        lambda gh, root, hold: (apps, libs, [], [], ["slicer v9.9.9: no readable crosspad-app.json"]))
    monkeypatch.setattr(app_bump, "submodule_repos", lambda root: {
        "components/crosspad-slicer": repos["slicer"], "components/crosspad-song": repos["song"],
        "components/crosspad-core": repos[dr.CORE], "components/crosspad-gui": repos[dr.GUI]})
    assert app_bump.main(["plan"]) == 0
    assert "held: slicer v9.9.9: no readable crosspad-app.json" in capsys.readouterr().out
