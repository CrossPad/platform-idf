# tools/tests/test_deps_resolve.py
import json, sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deps_resolve as dr

CORE, GUI = dr.CORE, dr.GUI


def rel(v, req=None, tag=True):
    version = tuple(int(x) for x in v.split("."))
    return dr.Release(version, "c" + v.replace(".", "") , f"v{v}" if tag else None, req or {})


def opts(pinned, *newer):
    return dr.Options(pinned, list(newer))


def libs(core=("1.53.0",), gui=("1.23.0",), gui_req="^1.53.0"):
    return {CORE: opts(rel(core[0]), *[rel(v) for v in core[1:]]),
            GUI: opts(rel(gui[0], {CORE: gui_req}), *[rel(v, {CORE: gui_req}) for v in gui[1:]])}


def test_header_version_reads_the_triple():
    text = "#define CROSSPAD_CORE_VERSION_MAJOR 1\n#define CROSSPAD_CORE_VERSION_MINOR 53\n" \
           "#define CROSSPAD_CORE_VERSION_PATCH 0\n#define CROSSPAD_GUI_REQUIRES_CORE_MAJOR 9\n"
    assert dr.header_version(text, "CROSSPAD_CORE_VERSION") == (1, 53, 0)
    with pytest.raises(dr.sr.RangeError):
        dr.header_version("#define CROSSPAD_CORE_VERSION_MAJOR 1\n", "CROSSPAD_CORE_VERSION")


def test_manifest_requires_is_strict():
    assert dr.manifest_requires({"requires": {CORE: ">=1.31.0"}}) == {CORE: ">=1.31.0"}
    assert dr.manifest_requires({}) == {}
    for bad in [{"requires": ["crosspad-core"]}, {"requires": {"lvgl": ">=9"}},
                {"requires": {CORE: "latest"}}, {"requires": {CORE: 1}}, []]:
        with pytest.raises(dr.sr.RangeError):
            dr.manifest_requires(bad)


def test_gui_requires_reads_library_json_dependencies():
    assert dr.gui_requires({"dependencies": {CORE: "^1.53.0"}}) == {CORE: "^1.53.0"}
    for bad in [{}, {"dependencies": {}}, {"dependencies": {CORE: "nope"}}, []]:
        with pytest.raises(dr.sr.RangeError):
            dr.gui_requires(bad)


def test_all_newest_fits_and_libs_stay():
    apps = {"slicer": opts(rel("0.3.0", {CORE: ">=1.31.0"}), rel("0.3.1", {CORE: ">=1.31.0"}))}
    s = dr.solve(apps, libs(core=("1.53.0", "1.60.0")))
    assert s.picks["slicer"].version == (0, 3, 1) and s.picks[CORE].version == (1, 53, 0)
    assert s.held == []


def test_app_needing_newer_core_moves_core_to_the_lowest_that_fits():
    apps = {"slicer": opts(rel("0.3.0"), rel("0.4.0", {CORE: ">=1.58.0"}))}
    s = dr.solve(apps, libs(core=("1.53.0", "1.58.0", "1.60.0")))
    assert s.picks[CORE].version == (1, 58, 0) and s.picks["slicer"].version == (0, 4, 0)


def test_gui_moves_too_when_its_range_excludes_the_core_an_app_needs():
    apps = {"a": opts(rel("0.1.0"), rel("0.2.0", {CORE: ">=1.60.0"}))}
    lb = {CORE: opts(rel("1.53.0"), rel("1.60.0")),
          GUI: opts(rel("1.23.0", {CORE: "~1.53.0"}), rel("1.24.0", {CORE: "^1.60.0"}))}
    s = dr.solve(apps, lb)
    assert (s.picks[CORE].version, s.picks[GUI].version) == ((1, 60, 0), (1, 24, 0))


def test_disjoint_apps_hold_one_with_its_cause():
    apps = {"a": opts(rel("0.1.0"), rel("0.2.0", {CORE: "<1.55.0"})),
            "b": opts(rel("0.1.0"), rel("0.2.0", {CORE: ">=1.60.0"}))}
    s = dr.solve(apps, libs(core=("1.53.0", "1.60.0")))
    assert s.picks["a"].version == (0, 2, 0) and s.picks["b"].version == (0, 1, 0)  # tie -> lowest core
    assert s.held == ["b 0.2.0: needs crosspad-core >=1.60.0, has 1.53.0"]


def test_major_never_moves():
    apps = {"a": opts(rel("0.1.0"), rel("0.2.0", {CORE: ">=2.0.0"}))}
    s = dr.solve(apps, libs(core=("1.53.0", "2.0.0")))
    assert s.picks[CORE].version == (1, 53, 0)
    assert s.held == ["a 0.2.0: needs crosspad-core >=2.0.0, has 1.53.0"]


def test_an_intermediate_version_is_taken_when_the_newest_does_not_fit():
    apps = {"a": opts(rel("0.1.0"), rel("0.2.0"), rel("0.3.0", {CORE: ">=2.0.0"}))}
    s = dr.solve(apps, libs())
    assert s.picks["a"].version == (0, 2, 0) and len(s.held) == 1


def test_no_candidates_is_the_pinned_set():
    s = dr.solve({"a": opts(rel("0.1.0", {CORE: ">=1.0.0"}))}, libs())
    assert {k: v.version for k, v in s.picks.items()} == {"a": (0, 1, 0), CORE: (1, 53, 0), GUI: (1, 23, 0)}
    assert s.held == []


def test_a_broken_pinned_set_is_unsatisfiable():
    with pytest.raises(dr.Unsatisfiable, match="a 0.1.0: needs crosspad-core >=1.60.0"):
        dr.solve({"a": opts(rel("0.1.0", {CORE: ">=1.60.0"}))}, libs())


def test_ties_take_the_lowest_pair_core_first():
    s = dr.solve({}, libs(core=("1.53.0", "1.54.0"), gui=("1.23.0", "1.24.0")))
    assert (s.picks[CORE].version, s.picks[GUI].version) == ((1, 53, 0), (1, 23, 0))


# --- check_tree (offline) -------------------------------------------------

def tree(tmp_path, apps, core="1.53.0", gui="1.23.0", gui_dep="^1.53.0", profile=None):
    def header(prefix, v):
        return "".join(f"#define {prefix}_{p} {x}\n" for p, x in zip(("MAJOR", "MINOR", "PATCH"), v.split(".")))
    c = tmp_path / "components" / "crosspad-core" / "include" / "crosspad"
    g = tmp_path / "components" / "crosspad-gui" / "include" / "crosspad-gui"
    c.mkdir(parents=True), g.mkdir(parents=True)
    (c / "CrosspadCoreVersion.hpp").write_text(header("CROSSPAD_CORE_VERSION", core))
    (g / "CrosspadGuiVersion.hpp").write_text(header("CROSSPAD_GUI_VERSION", gui))
    (tmp_path / "components" / "crosspad-gui" / "library.json").write_text(
        json.dumps({"version": gui, "dependencies": {CORE: gui_dep}}))
    for app_id, (v, req) in apps.items():
        d = tmp_path / "components" / f"crosspad-{app_id}"
        d.mkdir(parents=True)
        (d / "crosspad-app.json").write_text(json.dumps({"id": app_id, "version": v, "requires": req}))
    (tmp_path / "config" / "profiles").mkdir(parents=True)
    (tmp_path / "config" / "profiles" / "full.json").write_text(
        json.dumps({"apps": {a: {} for a in (profile or apps)}}))
    return tmp_path


def test_check_passes_a_consistent_tree(tmp_path):
    root = tree(tmp_path, {"slicer": ("0.3.0", {CORE: ">=1.31.0", GUI: ">=1.14.0"})})
    assert dr.check_tree(root, ["slicer"]) == []


def test_check_names_the_app_and_the_range(tmp_path):
    root = tree(tmp_path, {"slicer": ("0.4.0", {CORE: ">=1.60.0"})})
    assert dr.check_tree(root, ["slicer"]) == ["slicer 0.4.0: needs crosspad-core >=1.60.0, has 1.53.0"]


def test_check_judges_gui_against_core(tmp_path):
    root = tree(tmp_path, {}, gui_dep="^1.60.0")
    assert dr.check_tree(root, []) == ["crosspad-gui 1.23.0: needs crosspad-core ^1.60.0, has 1.53.0"]


def test_check_reports_an_unreadable_range(tmp_path):
    root = tree(tmp_path, {"a": ("0.1.0", {CORE: ">=1.2.0 <2.0.0"})})
    [line] = dr.check_tree(root, ["a"])
    assert line.startswith("cannot read the pinned versions") and "a" in line


def test_check_reads_only_the_profiles_apps(tmp_path, capsys):
    root = tree(tmp_path, {"sampler": ("0.8.4", {CORE: ">=1.45.0"})}, profile=["sampler"])
    (root / "config" / "profiles" / "lite.json").write_text(json.dumps({"apps": {"sampler": {}}}))
    assert dr.main(["check", "--profile", "lite", "--root", str(root)]) == 0
    assert "lite: 1 app(s), crosspad-core 1.53.0, crosspad-gui 1.23.0" in capsys.readouterr().out


def test_check_cli_fails_with_errors(tmp_path, capsys):
    root = tree(tmp_path, {"slicer": ("0.4.0", {CORE: ">=1.60.0"})})
    assert dr.main(["check", "--profile", "full", "--root", str(root)]) == 1
    assert "::error::full: slicer 0.4.0: needs crosspad-core >=1.60.0" in capsys.readouterr().out


@pytest.mark.parametrize("name", ["../x", "Full", "", "a/b"])
def test_check_cli_refuses_an_odd_profile_name(tmp_path, name):
    assert dr.main(["check", "--profile", name, "--root", str(tmp_path)]) == 1


def test_main_yml_checks_ranges_in_every_official_cell():
    doc = yaml.safe_load((Path(__file__).resolve().parents[2] / ".github" / "workflows" / "main.yml").read_text())
    steps = doc["jobs"]["build"]["steps"]
    names = [s.get("name", "") for s in steps]
    i = names.index("Dependency ranges hold (${{ matrix.profile }})")
    assert names.index("Apply the kit's profile (${{ matrix.profile }})") == i - 1
    assert steps[i]["if"] == "matrix.tier == 'official'"
    assert steps[i]["run"] == 'python3 tools/deps_resolve.py check --profile "${{ matrix.profile }}"'


def test_the_repository_itself_is_consistent():
    root = Path(__file__).resolve().parents[2]
    if not (root / "components" / "crosspad-core" / "include").is_dir():
        pytest.skip("submodules not checked out")
    apps = json.loads((root / "config" / "profiles" / "full.json").read_text())["apps"]
    assert dr.check_tree(root, sorted(apps)) == []
