import base64, json, sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app_bump, deps_resolve as dr, gh_api
from test_app_bump import make_repo
from test_app_bump_writer import FakeGH

A, B, C, D = "a" * 40, "b" * 40, "c" * 40, "d" * 40
CG, GG = "1" * 40, "2" * 40   # core and gui gitlinks
MIX = "CrossPad/crosspad-mixer"
CORE_R, GUI_R = "CrossPad/crosspad-core", "CrossPad/crosspad-gui"


def b64(text):
    return {"content": base64.b64encode(text.encode()).decode()}


def manifest(v, req=None, app_id="mixer"):
    return b64(json.dumps({"id": app_id, "version": v, "requires": req or {}}))


def header(prefix, v):
    return b64("".join(f"#define {prefix}_{p} {x}\n" for p, x in zip(("MAJOR", "MINOR", "PATCH"), v.split("."))))


def repo_root(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}},
                     links={"mixer": A, "core": CG, "gui": GG})
    (root / ".gitmodules").write_text("".join(
        f'[submodule "{n}"]\n\tpath = components/crosspad-{n}\n\turl = https://github.com/CrossPad/crosspad-{n}.git\n'
        for n in ("mixer", "core", "gui")))
    return root


def routes(app_tags=(), core_tags=(), gui_tags=(), extra=None):
    """Pinned: mixer 0.1.0 @A, core 1.53.0 @CG, gui 1.23.0 @GG (default branch master)."""
    r = {("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={A}"): manifest("0.1.0"),
         ("GET", f"/repos/{CORE_R}/contents/include/crosspad/CrosspadCoreVersion.hpp?ref={CG}"):
             header("CROSSPAD_CORE_VERSION", "1.53.0"),
         ("GET", f"/repos/{GUI_R}/contents/include/crosspad-gui/CrosspadGuiVersion.hpp?ref={GG}"):
             header("CROSSPAD_GUI_VERSION", "1.23.0"),
         ("GET", f"/repos/{GUI_R}/contents/library.json?ref={GG}"):
             b64(json.dumps({"version": "1.23.0", "dependencies": {"crosspad-core": "^1.53.0"}})),
         ("GET", f"/repos/{MIX}/tags"): [{"name": t, "commit": {"sha": s}} for t, s in app_tags],
         ("GET", f"/repos/{CORE_R}/tags"): [{"name": t, "commit": {"sha": s}} for t, s in core_tags],
         ("GET", f"/repos/{GUI_R}/tags"): [{"name": t, "commit": {"sha": s}} for t, s in gui_tags],
         ("GET", f"/repos/{MIX}"): {"default_branch": "main"},
         ("GET", f"/repos/{CORE_R}"): {"default_branch": "main"},
         ("GET", f"/repos/{GUI_R}"): {"default_branch": "master"}}
    r.update(extra or {})
    return r


def eligible(repo, gitlink, sha, default="main"):
    return {("GET", f"/repos/{repo}/compare/{sha}...{default}"): {"status": "ahead"},
            ("GET", f"/repos/{repo}/compare/{gitlink}...{sha}"): {"status": "ahead"}}


def test_release_tags_keep_only_plain_release_tags_ascending():
    tags = [{"name": "v0.10.0", "commit": {"sha": B}}, {"name": "v0.9.0-rc1", "commit": {"sha": A}},
            {"name": "v0.2.0", "commit": {"sha": A}}, {"name": "v1.0", "commit": {"sha": A}},
            {"name": "v9.9.9", "commit": {"sha": "nope"}}]
    assert app_bump.release_tags(tags) == [((0, 2, 0), "v0.2.0", A), ((0, 10, 0), "v0.10.0", B)]


def test_pinned_only_when_nothing_is_newer(tmp_path):
    apps, libs, notes, diverged, skipped = app_bump.collect_options(FakeGH(routes()), repo_root(tmp_path), set())
    assert apps["mixer"] == dr.Options(dr.Release((0, 1, 0), A, None, {}), [])
    assert libs[dr.CORE].pinned.version == (1, 53, 0) and libs[dr.CORE].newer == []
    assert libs[dr.GUI].pinned.requires == {"crosspad-core": "^1.53.0"}
    assert diverged == [] and skipped == []


def test_newer_app_tags_become_options(tmp_path):
    ex = {**eligible(MIX, A, B), **eligible(MIX, A, C),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0"),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={C}"): manifest("0.3.0", {"crosspad-core": ">=1.60.0"})}
    gh = FakeGH(routes(app_tags=[("v0.1.0", A), ("v0.2.0", B), ("v0.3.0", C)], extra=ex))
    apps, *_ = app_bump.collect_options(gh, repo_root(tmp_path), set())
    assert [(r.version, r.tag, r.commit) for r in apps["mixer"].newer] == [((0, 2, 0), "v0.2.0", B),
                                                                          ((0, 3, 0), "v0.3.0", C)]
    assert apps["mixer"].newer[1].requires == {"crosspad-core": ">=1.60.0"}


def test_tag_and_manifest_version_disagree(tmp_path):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.1.9")}
    apps, _, notes, _, skipped = app_bump.collect_options(FakeGH(routes(app_tags=[("v0.2.0", B)], extra=ex)),
                                                 repo_root(tmp_path), set())
    assert apps["mixer"].newer == []
    assert any("v0.2.0" in n and "0.1.9" in n for n in skipped) and not notes


def test_unreadable_range_in_a_tag_is_a_note_not_a_crash(tmp_path):
    ex = {**eligible(MIX, A, B),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0", {"crosspad-core": "latest"})}
    apps, _, notes, _, skipped = app_bump.collect_options(FakeGH(routes(app_tags=[("v0.2.0", B)], extra=ex)),
                                                 repo_root(tmp_path), set())
    assert apps["mixer"].newer == [] and any("v0.2.0" in n for n in skipped)


def test_diverged_and_off_branch_tags_are_skipped(tmp_path):
    ex = {("GET", f"/repos/{MIX}/compare/{B}...main"): {"status": "ahead"},
          ("GET", f"/repos/{MIX}/compare/{A}...{B}"): {"status": "diverged"},
          ("GET", f"/repos/{MIX}/compare/{C}...main"): {"status": "diverged"}}
    apps, _, notes, diverged, skipped = app_bump.collect_options(
        FakeGH(routes(app_tags=[("v0.2.0", B), ("v0.3.0", C)], extra=ex)), repo_root(tmp_path), set())
    assert apps["mixer"].newer == [] and diverged == ["mixer"]
    assert any("v0.3.0" in n and "default branch" in n for n in notes)


def test_core_tags_same_major_only(tmp_path):
    ex = {**eligible(CORE_R, CG, B),
          ("GET", f"/repos/{CORE_R}/contents/include/crosspad/CrosspadCoreVersion.hpp?ref={B}"):
              header("CROSSPAD_CORE_VERSION", "1.60.0")}
    gh = FakeGH(routes(core_tags=[("v1.22.0", D), ("v1.60.0", B), ("v2.0.0", C)], extra=ex))
    _, libs, notes, _, skipped = app_bump.collect_options(gh, repo_root(tmp_path), set())
    assert [r.version for r in libs[dr.CORE].newer] == [(1, 60, 0)]
    assert any("v2.0.0" in n and "MAJOR" in n for n in notes)


def test_lib_default_branch_is_read_not_assumed(tmp_path):
    lib = b64(json.dumps({"version": "1.24.0", "dependencies": {"crosspad-core": "^1.53.0"}}))
    ex = {**eligible(GUI_R, GG, B, default="master"),
          ("GET", f"/repos/{GUI_R}/contents/include/crosspad-gui/CrosspadGuiVersion.hpp?ref={B}"):
              header("CROSSPAD_GUI_VERSION", "1.24.0"),
          ("GET", f"/repos/{GUI_R}/contents/library.json?ref={B}"): lib}
    gh = FakeGH(routes(gui_tags=[("v1.24.0", B)], extra=ex))
    _, libs, *_ = app_bump.collect_options(gh, repo_root(tmp_path), set())
    assert [r.version for r in libs[dr.GUI].newer] == [(1, 24, 0)]
    assert ("GET", f"/repos/{GUI_R}/compare/{B}...master", None) in gh.calls


def test_hold_applies_to_apps_and_libs(tmp_path):
    gh = FakeGH(routes(app_tags=[("v0.2.0", B)], core_tags=[("v1.60.0", C)]))
    apps, libs, notes, *_ = app_bump.collect_options(gh, repo_root(tmp_path), {"mixer", "core"})
    assert apps["mixer"].newer == [] and libs[dr.CORE].newer == []
    assert "mixer: held (APP_BUMP_HOLD)" in notes and "core: held (APP_BUMP_HOLD)" in notes
    assert not any("/tags" in c[1] for c in gh.calls if "mixer" in c[1] or "core" in c[1])


def test_unreadable_pinned_component_stops_the_run(tmp_path):
    r = routes()
    r[("GET", f"/repos/{CORE_R}/contents/include/crosspad/CrosspadCoreVersion.hpp?ref={CG}")] = \
        gh_api.ApiError(404, "GET", "x", "missing")
    with pytest.raises(app_bump.BumpError, match="crosspad-core"):
        app_bump.collect_options(FakeGH(r), repo_root(tmp_path), set())


def test_a_server_error_is_raised_not_skipped(tmp_path):
    r = routes(app_tags=[("v0.2.0", B)])
    r[("GET", f"/repos/{MIX}/compare/{B}...main")] = gh_api.ApiError(502, "GET", "x", "bad gateway")
    with pytest.raises(gh_api.ApiError):
        app_bump.collect_options(FakeGH(r), repo_root(tmp_path), set())


def test_repos_scope_includes_core_and_gui(tmp_path):
    assert app_bump.repos_scope(repo_root(tmp_path)) == ["crosspad-core", "crosspad-gui", "crosspad-mixer"]


def test_repos_scope_fails_closed_without_a_library_entry(tmp_path):
    root = repo_root(tmp_path)
    (root / ".gitmodules").write_text('[submodule "m"]\n\tpath = components/crosspad-mixer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-mixer.git\n')
    with pytest.raises(app_bump.BumpError, match="crosspad-core"):
        app_bump.repos_scope(root)


def test_release_tags_ignore_malformed_and_hostile_tags():
    junk = ["v1.0", "1.2.3", "v1.2.3.4", "v01.2.3", "v1.2.3\n", " v1.2.3", "v١.0.0", "v1.2.3+meta",
            "v99999999999999.0.0000000", "v1.2.3-rc1"]
    tags = [{"name": n, "commit": {"sha": B}} for n in junk]
    tags += [{"name": "v9.9.9", "commit": {"sha": "not-a-sha"}}, {"name": "v8.8.8", "commit": {}},
             {"name": "v0.3.1", "commit": {"sha": B}}, {"name": "v0.3.0", "commit": {"sha": A}}]
    assert app_bump.release_tags(tags) == [((0, 3, 0), "v0.3.0", A), ((0, 3, 1), "v0.3.1", B)]
    assert app_bump.release_tags(tags[:len(junk)]) == []


DEEP = b64("[" * 100000)


def test_deeply_nested_manifest_at_a_tag_is_a_note_not_a_crash(tmp_path):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): DEEP}
    apps, _, notes, _, skipped = app_bump.collect_options(FakeGH(routes(app_tags=[("v0.2.0", B)], extra=ex)),
                                                 repo_root(tmp_path), set())
    assert apps["mixer"].newer == [] and any("v0.2.0" in n for n in skipped)


def test_deeply_nested_library_json_at_a_tag_is_a_note_not_a_crash(tmp_path):
    ex = {**eligible(GUI_R, GG, B, default="master"),
          ("GET", f"/repos/{GUI_R}/contents/include/crosspad-gui/CrosspadGuiVersion.hpp?ref={B}"):
              header("CROSSPAD_GUI_VERSION", "1.24.0"),
          ("GET", f"/repos/{GUI_R}/contents/library.json?ref={B}"): DEEP}
    _, libs, notes, _, skipped = app_bump.collect_options(FakeGH(routes(gui_tags=[("v1.24.0", B)], extra=ex)),
                                                 repo_root(tmp_path), set())
    assert libs[dr.GUI].newer == [] and any("v1.24.0" in n for n in skipped)


def test_deeply_nested_pinned_manifest_stops_the_run(tmp_path):
    r = routes(extra={("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={A}"): DEEP})
    with pytest.raises(app_bump.BumpError, match="mixer"):
        app_bump.collect_options(FakeGH(r), repo_root(tmp_path), set())


def test_deeply_nested_pinned_library_json_stops_the_run(tmp_path):
    r = routes(extra={("GET", f"/repos/{GUI_R}/contents/library.json?ref={GG}"): DEEP})
    with pytest.raises(app_bump.BumpError, match="crosspad-gui"):
        app_bump.collect_options(FakeGH(r), repo_root(tmp_path), set())


# --- skipped: a tag's CONTENT problem is reported as held, a topology problem is only a note ---------

def _mixer_b(manifest_answer, extra=None):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest_answer}
    ex.update(extra or {})
    return routes(app_tags=[("v0.2.0", B)], extra=ex)


def test_an_unreadable_range_at_a_candidate_tag_is_skipped_with_a_readable_reason(tmp_path):
    r = _mixer_b(manifest("0.2.0", {"crosspad-core": ">=1.2.0 <2.0.0"}))
    apps, _, notes, _, skipped = app_bump.collect_options(FakeGH(r), repo_root(tmp_path), set())
    assert apps["mixer"].newer == [] and notes == []
    [line] = skipped
    assert line.startswith("mixer v0.2.0: ") and ">=1.2.0 <2.0.0" in line


def test_a_tag_whose_manifest_says_another_version_is_skipped(tmp_path):
    _, _, notes, _, skipped = app_bump.collect_options(FakeGH(_mixer_b(manifest("0.1.9"))),
                                                       repo_root(tmp_path), set())
    assert skipped == ["mixer v0.2.0: its version reads 0.1.9"] and notes == []


def test_a_tag_with_the_wrong_app_id_is_skipped(tmp_path):
    _, _, _, _, skipped = app_bump.collect_options(FakeGH(_mixer_b(manifest("0.2.0", app_id="other"))),
                                                   repo_root(tmp_path), set())
    assert len(skipped) == 1 and skipped[0].startswith("mixer v0.2.0: ") and "other" in skipped[0]


def test_a_tag_with_an_unreadable_gui_header_or_library_json_is_skipped(tmp_path):
    ex = {**eligible(GUI_R, GG, B, default="master"),
          ("GET", f"/repos/{GUI_R}/contents/include/crosspad-gui/CrosspadGuiVersion.hpp?ref={B}"):
              gh_api.ApiError(404, "GET", "x", "")}
    _, libs, notes, _, skipped = app_bump.collect_options(
        FakeGH(routes(gui_tags=[("v1.24.0", B)], extra=ex)), repo_root(tmp_path), set())
    assert libs[dr.GUI].newer == [] and notes == []
    assert skipped == ["gui v1.24.0: no readable include/crosspad-gui/CrosspadGuiVersion.hpp"]


def test_topology_causes_are_notes_and_never_skipped(tmp_path):
    # off the default branch, behind the gitlink, another MAJOR, held: log notes only
    ex = {("GET", f"/repos/{MIX}/compare/{B}...main"): {"status": "diverged"},
          ("GET", f"/repos/{MIX}/compare/{C}...main"): {"status": "ahead"},
          ("GET", f"/repos/{MIX}/compare/{A}...{C}"): {"status": "behind"},
          **eligible(CORE_R, CG, D)}
    gh = FakeGH(routes(app_tags=[("v0.2.0", B), ("v0.3.0", C)], core_tags=[("v2.0.0", D)], extra=ex))
    root = repo_root(tmp_path)
    apps, libs, notes, _, skipped = app_bump.collect_options(gh, root, set())
    assert apps["mixer"].newer == [] and libs[dr.CORE].newer == [] and skipped == []
    assert len(notes) == 3
    _, _, notes, _, skipped = app_bump.collect_options(FakeGH(routes(app_tags=[("v0.2.0", B)])), root, {"mixer"})
    assert skipped == [] and "mixer: held (APP_BUMP_HOLD)" in notes


# --- untrusted input from the GitHub API ----------------------------------------------------------

def _one_tag(tmp_path, extra, hold=frozenset()):
    return app_bump.collect_options(FakeGH(routes(app_tags=[("v0.2.0", B)], extra=extra)),
                                    repo_root(tmp_path), set(hold))


@pytest.mark.parametrize("status", ["AHEAD", "ahead_by_3", "", 7, None, ["ahead"], {"a": 1}])
def test_an_odd_compare_status_is_never_eligible(tmp_path, status):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/compare/{A}...{B}"): {"status": status},
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    apps, _, notes, _, skipped = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and skipped == [] and len(notes) == 1


@pytest.mark.parametrize("answer", [{}, None, [], "ahead"])
def test_a_compare_without_a_status_is_never_eligible(tmp_path, answer):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/compare/{A}...{B}"): answer,
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    assert _one_tag(tmp_path, ex)[0]["mixer"].newer == []


@pytest.mark.parametrize("status", ["AHEAD", "", 7, None])
def test_an_odd_branch_compare_status_means_not_on_the_default_branch(tmp_path, status):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/compare/{B}...main"): {"status": status},
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    apps, _, notes, _, _ = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and any("default branch" in n for n in notes)


@pytest.mark.parametrize("code", [404, 422])
def test_a_failing_branch_compare_means_not_on_the_default_branch(tmp_path, code):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/compare/{B}...main"): gh_api.ApiError(code, "GET", "x", ""),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    apps, _, notes, _, skipped = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and skipped == [] and any("default branch" in n for n in notes)


@pytest.mark.parametrize("code", [404, 422])
def test_a_failing_gitlink_compare_is_not_eligible(tmp_path, code):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/compare/{A}...{B}"): gh_api.ApiError(code, "GET", "x", ""),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    apps, _, notes, _, skipped = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and skipped == [] and len(notes) == 1


@pytest.mark.parametrize("default", ["main?x=1 #", "main\n::error::x", "", None, 7, ["main"]])
def test_a_hostile_default_branch_never_reaches_a_url(tmp_path, default):
    ex = {("GET", f"/repos/{MIX}"): {"default_branch": default},
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): manifest("0.2.0")}
    gh = FakeGH(routes(app_tags=[("v0.2.0", B)], extra=ex))
    apps, _, notes, _, _ = app_bump.collect_options(gh, repo_root(tmp_path), set())
    assert apps["mixer"].newer == [] and any("default branch" in n for n in notes)
    assert not any(("x=1" in path) or ("::error" in path) or ("%0A" in path) for _, path, _ in gh.calls)


MALFORMED = {
    "no content key": {"encoding": "base64"},
    "not base64": {"content": "QQ"},
    "not utf-8": {"content": base64.b64encode(b"\xff\xfe\x00").decode()},
    "not json": b64("this is not json"),
    "json array": b64("[1, 2, 3]"),
}


@pytest.mark.parametrize("shape", list(MALFORMED))
def test_a_malformed_candidate_manifest_is_skipped_not_a_crash(tmp_path, shape):
    ex = {**eligible(MIX, A, B), ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): MALFORMED[shape]}
    apps, _, notes, _, skipped = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and notes == []
    assert len(skipped) == 1 and skipped[0].startswith("mixer v0.2.0: ")


def test_a_missing_candidate_manifest_is_skipped(tmp_path):
    ex = {**eligible(MIX, A, B),
          ("GET", f"/repos/{MIX}/contents/crosspad-app.json?ref={B}"): gh_api.ApiError(404, "GET", "x", "")}
    apps, _, notes, _, skipped = _one_tag(tmp_path, ex)
    assert apps["mixer"].newer == [] and notes == []
    assert skipped == ["mixer v0.2.0: no readable crosspad-app.json"]
