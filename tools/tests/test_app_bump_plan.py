import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app_bump
import deps_resolve as dr
from test_app_bump import make_repo, git

A, B = "a" * 40, "b" * 40
C1, C2, G1, S1, S2 = "1" * 40, "2" * 40, "3" * 40, "4" * 40, "5" * 40


def r(v, sha, tag=None, req=None):
    return dr.Release(tuple(int(x) for x in v.split(".")), sha, tag, req or {})


def setup_plan():
    apps = {"slicer": dr.Options(r("0.3.1", S1), [r("0.4.0", S2, "v0.4.0", {dr.CORE: ">=1.60.0"})]),
            "song": dr.Options(r("0.2.0", "6" * 40), [])}
    libs = {dr.CORE: dr.Options(r("1.53.0", C1), [r("1.60.0", C2, "v1.60.0")]),
            dr.GUI: dr.Options(r("1.23.0", G1, req={dr.CORE: "^1.53.0"}), [])}
    repos = {"slicer": "CrossPad/crosspad-slicer", "song": "CrossPad/crosspad-song",
             dr.CORE: "CrossPad/crosspad-core", dr.GUI: "CrossPad/crosspad-gui"}
    return apps, libs, repos


def test_plan_moves_lists_libraries_first_and_only_what_moves():
    apps, libs, repos = setup_plan()
    moves = app_bump.plan_moves(apps, libs, dr.solve(apps, libs), repos)
    assert [(m.label, m.path, m.new.commit) for m in moves] == [
        ("core", "components/crosspad-core", C2), ("slicer", "components/crosspad-slicer", S2)]


def test_pr_title_names_old_and_new_versions():
    apps, libs, repos = setup_plan()
    moves = app_bump.plan_moves(apps, libs, dr.solve(apps, libs), repos)
    assert app_bump.pr_title(moves) == "chore(deps): core 1.53.0→1.60.0, slicer 0.3.1→0.4.0"


def test_pr_body_links_each_move_and_lists_held():
    apps, libs, repos = setup_plan()
    moves = app_bump.plan_moves(apps, libs, dr.solve(apps, libs), repos)
    body = app_bump.pr_body(moves, ["mixer 0.13.0: needs crosspad-core >=2.0.0, has 1.60.0"])
    assert "| core | 1.53.0 | 1.60.0 | [111111111111...222222222222](https://github.com/CrossPad/crosspad-core/compare/111111111111...222222222222) |" in body
    assert "Held back" in body and "- mixer 0.13.0: needs crosspad-core >=2.0.0, has 1.60.0" in body
    assert "**Held back** (not in this bump; the newest release's ranges conflict with this set" in body
    assert "no core/gui this bump can reach" not in body
    assert "Held back" not in app_bump.pr_body(moves, [])


def test_nothing_to_move_is_an_empty_plan():
    apps = {"song": dr.Options(r("0.2.0", "6" * 40), [])}
    libs = {dr.CORE: dr.Options(r("1.53.0", C1), []), dr.GUI: dr.Options(r("1.23.0", G1), [])}
    assert app_bump.plan_moves(apps, libs, dr.solve(apps, libs), {}) == []


def test_official_apps_and_repos(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}}, {"sampler": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text('[submodule "m"]\n\tpath = components/crosspad-mixer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-mixer.git\n'
                                      '[submodule "s"]\n\tpath = components/crosspad-sequencer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-pattern-seq.git\n')
    assert app_bump.official_apps(root) == ["mixer", "sampler"]
    assert app_bump.submodule_repos(root) == {"components/crosspad-mixer": "CrossPad/crosspad-mixer",
                                              "components/crosspad-sequencer": "CrossPad/crosspad-pattern-seq"}


def test_submodule_repos_skips_non_github_and_odd_urls(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text('[submodule "a"]\n\tpath = components/a\n\turl = https://example.com/x/y.git\n'
                                      '[submodule "b"]\n\tpath = components/b\n\turl = git@github.com:CrossPad/b-repo.git\n'
                                      '[submodule "c"]\n\tpath = components/c\n\turl = https://github.com/CrossPad/c d.git\n')
    assert app_bump.submodule_repos(root) == {"components/b": "CrossPad/b-repo"}


def test_repos_cli_prints_names_of_official_apps_only(tmp_path, capsys, monkeypatch):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": A}}, {"sampler": {"track": "pinned", "commit": A}})
    (root / ".gitmodules").write_text('[submodule "m"]\n\tpath = components/crosspad-mixer\n'
                                      '\turl = https://github.com/CrossPad/crosspad-mixer.git\n'
                                      '[submodule "s"]\n\tpath = components/crosspad-sampler\n'
                                      '\turl = https://github.com/CrossPad/crosspad-sampler.git\n'
                                      '[submodule "c"]\n\tpath = components/crosspad-core\n'
                                      '\turl = https://github.com/CrossPad/crosspad-core.git\n'
                                      '[submodule "g"]\n\tpath = components/crosspad-gui\n'
                                      '\turl = https://github.com/CrossPad/crosspad-gui.git\n'
                                      '[submodule "o"]\n\tpath = components/other\n'
                                      '\turl = https://github.com/CrossPad/other.git\n')
    monkeypatch.setattr(app_bump, "REPO", root)
    assert app_bump.main(["repos"]) == 0
    assert capsys.readouterr().out == "crosspad-core,crosspad-gui,crosspad-mixer,crosspad-sampler\n"
