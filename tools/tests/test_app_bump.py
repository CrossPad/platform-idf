import json, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app_bump

REPO = Path(__file__).resolve().parents[2]
SHA_A = "a" * 40
SHA_B = "b" * 40


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


def make_repo(tmp_path, apps_full, apps_lite=None, links=None):
    """A git repo with config/profiles/{full,lite}.json and gitlinks at components/crosspad-<id>."""
    root = tmp_path / "repo"
    (root / "config" / "profiles" / "community").mkdir(parents=True)
    git(root, "init", "-q")
    for name, apps in (("full", apps_full), ("lite", apps_lite)):
        if apps is None:
            continue
        doc = {"name": name, "display_name": name, "icon": f"icons/{name}.png",
               "description": "d", "platform": "esp-idf", "features": {}, "apps": apps}
        (root / "config" / "profiles" / f"{name}.json").write_text(app_bump.dumps_profile(doc))
    (root / "config" / "profiles" / "community" / "x.json").write_text("{}\n")
    git(root, "add", ".")
    for app_id, sha in (links or {}).items():
        git(root, "update-index", "--add", "--cacheinfo", f"160000,{sha},components/crosspad-{app_id}")
    git(root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "init")
    return root


def test_dumps_profile_round_trips_the_real_profiles():
    for path in sorted((REPO / "config" / "profiles").glob("*.json")):
        text = path.read_text()
        assert app_bump.dumps_profile(json.loads(text)) == text, path.name


def test_gitlinks_reads_mode_160000_entries(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": SHA_A}}, links={"mixer": SHA_A})
    assert app_bump.gitlinks(root) == {"components/crosspad-mixer": SHA_A}


def test_check_passes_when_pins_equal_gitlinks(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": SHA_A}}, links={"mixer": SHA_A})
    assert app_bump.check(root) == []


def test_check_names_a_pin_that_differs_from_the_gitlink(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": SHA_B}}, links={"mixer": SHA_A})
    [problem] = app_bump.check(root)
    assert "full: mixer" in problem and SHA_A[:12] in problem and "app_bump.py pin mixer" in problem


def test_check_refuses_an_unpinned_official_app(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "branch", "ref": "main"}}, links={"mixer": SHA_A})
    [problem] = app_bump.check(root)
    assert "track branch" in problem


def test_check_ignores_community_profiles(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": SHA_A}}, links={"mixer": SHA_A})
    assert app_bump.check(root) == []          # community/x.json is "{}" and is not read


def test_check_names_an_app_without_a_gitlink(tmp_path):
    root = make_repo(tmp_path, {"ghost": {"track": "pinned", "commit": SHA_A}})
    [problem] = app_bump.check(root)
    assert "no gitlink" in problem


def test_apply_pins_rewrites_only_the_named_app_in_every_profile_listing_it(tmp_path):
    root = make_repo(tmp_path,
                     {"mixer": {"track": "pinned", "commit": SHA_A}, "song": {"track": "pinned", "commit": SHA_A}},
                     {"mixer": {"track": "registry"}})
    out = app_bump.apply_pins(root, {"mixer": SHA_B})
    assert set(out) == {"config/profiles/full.json", "config/profiles/lite.json"}
    full = json.loads(out["config/profiles/full.json"])
    assert full["apps"]["mixer"] == {"track": "pinned", "commit": SHA_B}
    assert full["apps"]["song"] == {"track": "pinned", "commit": SHA_A}
    assert json.loads(out["config/profiles/lite.json"])["apps"]["mixer"]["commit"] == SHA_B


def test_apply_pins_returns_nothing_when_nothing_changes(tmp_path):
    root = make_repo(tmp_path, {"mixer": {"track": "pinned", "commit": SHA_A}})
    assert app_bump.apply_pins(root, {"mixer": SHA_A}) == {}


def test_assert_built_matches_submodule_status_lines():
    doc = {"apps": {"mixer": {"track": "pinned", "commit": SHA_A}}}
    ok = f" {SHA_A} components/crosspad-mixer (heads/main)\n"
    moved = f"+{SHA_B} components/crosspad-mixer (heads/main)\n"
    assert app_bump.assert_built(doc, ok) == []
    [problem] = app_bump.assert_built(doc, moved)
    assert "built " + SHA_B[:12] in problem
    [missing] = app_bump.assert_built(doc, "")
    assert "not in components.txt" in missing


def test_assert_built_flags_a_submodule_that_is_not_checked_out():
    doc = {"apps": {"mixer": {"track": "pinned", "commit": SHA_A}}}
    # a leading "-" is `git submodule status` for "not initialised", even when the sha matches
    [problem] = app_bump.assert_built(doc, f"-{SHA_A} components/crosspad-mixer\n")
    assert "not checked out" in problem
