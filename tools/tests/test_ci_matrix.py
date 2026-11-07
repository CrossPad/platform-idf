import json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_matrix
import fw_icon

REPO = Path(__file__).resolve().parents[2]


def profile(path, **over):
    """A valid profile at `path` with its 64x64 icon next to it in icons/."""
    icons = path.parent / "icons"
    icons.mkdir(parents=True, exist_ok=True)
    (icons / f"{path.stem}.png").write_bytes(fw_icon.grid_png(4, (255, 136, 0)))
    doc = {"name": path.stem, "display_name": path.stem.title()[:24], "description": "A test kit.",
           "icon": f"icons/{path.stem}.png", "platform": "esp-idf", "features": {},
           "apps": {"sampler": {"track": "registry"}}}
    if path.parent.name == "community":
        doc["author"] = "Tester"
    doc.update(over)
    path.write_text(json.dumps(doc))
    return path


def test_valid_profile(tmp_path):
    assert ci_matrix.validate_profile(profile(tmp_path / "lite.json")) == []


def test_profile_name_rules(tmp_path):
    assert ci_matrix.validate_profile(profile(tmp_path / "lite.json", name="other"))
    assert ci_matrix.validate_profile(profile(tmp_path / "v2.json"))             # looks like a revision
    assert ci_matrix.validate_profile(profile(tmp_path / "Lite.json"))
    assert ci_matrix.validate_profile(profile(tmp_path / "abcdefghijklmnop.json"))   # 16 characters
    assert ci_matrix.validate_profile(profile(tmp_path / "lite2.json", platform="arduino"))
    assert ci_matrix.validate_profile(profile(tmp_path / "lite3.json", apps={}))
    (tmp_path / "broken.json").write_text("{")
    assert ci_matrix.validate_profile(tmp_path / "broken.json")


def test_matrix_default_and_all(tmp_path, monkeypatch):
    (tmp_path / "community").mkdir()
    profile(tmp_path / "full.json")
    profile(tmp_path / "lite.json")
    profile(tmp_path / "community" / "drums.json")
    monkeypatch.setattr(ci_matrix, "PROFILE_DIR", tmp_path)
    monkeypatch.setattr(ci_matrix, "COMMUNITY_DIR", tmp_path / "community")
    m, errors = ci_matrix.matrix(all_kits=False)
    assert errors == []
    assert m["include"] == [{"rev": "v2", "kit": "full", "profile": "full", "tier": "official"}]
    m, errors = ci_matrix.matrix(all_kits=True)
    assert errors == []
    assert {(c["kit"], c["profile"], c["tier"]) for c in m["include"]} == {
        ("full", "full", "official"), ("lite", "lite", "official"),
        ("drums", "community/drums", "community")}
    assert len(m["include"]) == 3
    m, errors = ci_matrix.matrix(all_kits=True, revs=("v1", "v2"))
    assert errors == []
    assert len(m["include"]) == 6
    _, errors = ci_matrix.matrix(all_kits=False, revs=("v3",))
    assert errors


def test_duplicate_kit_and_missing_default(tmp_path, monkeypatch):
    (tmp_path / "community").mkdir()
    profile(tmp_path / "lite.json")
    profile(tmp_path / "community" / "lite.json")
    monkeypatch.setattr(ci_matrix, "PROFILE_DIR", tmp_path)
    monkeypatch.setattr(ci_matrix, "COMMUNITY_DIR", tmp_path / "community")
    _, errors = ci_matrix.matrix(all_kits=True)
    assert any("twice" in e for e in errors)
    assert any("full.json" in e for e in errors)


def test_repository_profiles_are_valid():
    _, errors = ci_matrix.matrix(all_kits=True)
    assert errors == []


def test_full_profile_is_the_checked_in_build():
    # The default build is kit "full" and keeps the old release file names;
    # a full.json that drifted from crosspad.config.json would ship two
    # different images under one kit id.
    full = json.loads((REPO / "config" / "profiles" / "full.json").read_text())
    cfg = json.loads((REPO / "crosspad.config.json").read_text())
    assert set(full["apps"]) == set(cfg["apps"])
    assert full.get("features", {}) == cfg.get("features", {})


def test_profile_display_fields(tmp_path):
    assert ci_matrix.validate_profile(profile(tmp_path / "lite.json")) == []
    assert any("display_name" in e for e in ci_matrix.validate_profile(profile(tmp_path / "a.json", display_name="")))
    assert any("display_name" in e for e in
               ci_matrix.validate_profile(profile(tmp_path / "b.json", display_name="x" * 25)))
    assert any("cannot draw" in e for e in
               ci_matrix.validate_profile(profile(tmp_path / "c.json", description="Kit ★")))
    assert any("description" in e for e in      # 100 characters, 200 bytes
               ci_matrix.validate_profile(profile(tmp_path / "d.json", description="ż" * 100)))
    assert any("icon" in e for e in ci_matrix.validate_profile(profile(tmp_path / "e.json", icon="icons/x.png")))
    assert any("apps" in e for e in ci_matrix.validate_profile(
        profile(tmp_path / "f.json", apps={f"app{i}": {"track": "registry"} for i in range(17)})))
    (tmp_path / "community").mkdir()
    p = profile(tmp_path / "community" / "drums.json")
    assert ci_matrix.validate_profile(p) == []
    doc = json.loads(p.read_text())
    del doc["author"]
    p.write_text(json.dumps(doc))
    assert any("author" in e for e in ci_matrix.validate_profile(p))


def test_profile_icon_must_be_a_64px_png(tmp_path):
    p = profile(tmp_path / "lite.json")
    (tmp_path / "icons" / "lite.png").write_bytes(fw_icon.write_png(32, 32, [(0, 0, 0, 255)] * 1024))
    assert any("64x64" in e for e in ci_matrix.validate_profile(p))
    (tmp_path / "icons" / "lite.png").unlink()
    assert any("lite.png" in e for e in ci_matrix.validate_profile(p))


def test_del_is_not_a_drawable_character(tmp_path):
    assert any("cannot draw" in e for e in
               ci_matrix.validate_profile(profile(tmp_path / "g.json", display_name="a\x7fb")))


def test_profile_image_shape(tmp_path):
    assert ci_matrix.validate_profile(profile(tmp_path / "game.json", standalone="Doom", top_bar=False,
                                              sample_engine=False)) == []
    assert any("standalone" in e for e in ci_matrix.validate_profile(profile(tmp_path / "a.json", standalone="")))
    assert any("top_bar" in e for e in ci_matrix.validate_profile(profile(tmp_path / "b.json", top_bar="no")))
    assert any("sample_engine" in e for e in ci_matrix.validate_profile(profile(tmp_path / "c.json", sample_engine=0)))
    assert ci_matrix.validate_profile(profile(tmp_path / "sdr.json", standalone="Sdr", radio=False, audio=False)) == []
    assert any("radio" in e for e in ci_matrix.validate_profile(profile(tmp_path / "d.json", radio="off")))
    assert any("audio" in e for e in ci_matrix.validate_profile(profile(tmp_path / "e.json", audio=1)))


def test_profile_sdkconfig_lines(tmp_path):
    lines = ["CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y", "# CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB is not set"]
    assert ci_matrix.validate_profile(profile(tmp_path / "sdr.json", sdkconfig=lines)) == []
    assert any("sdkconfig" in e for e in ci_matrix.validate_profile(profile(tmp_path / "a.json", sdkconfig="CONFIG_X=y")))
    assert any("sdkconfig" in e for e in ci_matrix.validate_profile(profile(tmp_path / "b.json", sdkconfig=["X=y"])))
    assert any("sdkconfig" in e for e in ci_matrix.validate_profile(profile(tmp_path / "c.json", sdkconfig=["CONFIG_X=y;CONFIG_Y=n"])))
    (tmp_path / "community").mkdir()
    assert any("official" in e for e in ci_matrix.validate_profile(
        profile(tmp_path / "community" / "drums.json", sdkconfig=lines)))
