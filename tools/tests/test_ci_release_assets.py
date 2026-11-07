import json, sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ci_release_assets
import fw_icon
import fw_sign
from test_ci_matrix import profile
from test_fw_image_desc import image
from test_fw_sign import keypair

TAG = "v1.3.0-rc1"
# `git submodule status` after a profile moved a branch-tracked app past the tag's gitlink.
COMPONENTS = (" 1111111111111111111111111111111111111111 components/crosspad-core (v1.30.0)\n"
              "+2222222222222222222222222222222222222222 components/crosspad-sampler (heads/main)\n")
APPS = {"sampler": {"track": "registry"}, "mixer": {"track": "registry"}}


def cell(dist, rev, kit, *, rev_num=2, version=TAG.encode()):
    d = dist / f"build-{rev}-{kit}"
    (d / "bootloader").mkdir(parents=True)
    (d / "partition_table").mkdir()
    (d / "CrossPad.bin").write_bytes(image(board_rev=rev_num, kit=kit.encode(), version=version))
    (d / "assets.bin").write_bytes(b"A" * 4096)
    (d / "assets_id.txt").write_text(bytes(range(32)).hex())
    (d / "bootloader" / "bootloader.bin").write_bytes(b"B")
    (d / "partition_table" / "partition-table.bin").write_bytes(b"P")
    (d / "CrossPad.elf").write_bytes(b"ELF")
    (d / "components.txt").write_text(COMPONENTS)
    return d


def setup(tmp_path):
    key, pub = keypair(tmp_path)
    header = tmp_path / "fw_sign_pubkey.h"
    header.write_text(fw_sign.header_text([fw_sign.public_der(pub)]))
    dist = tmp_path / "dist"
    cell(dist, "v1", "full", rev_num=1)
    cell(dist, "v2", "full")
    cell(dist, "v2", "drums")
    profiles = tmp_path / "profiles"
    profile(profiles / "full.json", display_name="Full", description="The full kit.", apps=APPS)
    (profiles / "community").mkdir()
    profile(profiles / "community" / "drums.json", display_name="Drums", description="The drums kit.",
            author="Ola", apps=APPS)
    cells = [{"rev": "v1", "kit": "full", "profile": "full", "tier": "official"},
             {"rev": "v2", "kit": "full", "profile": "full", "tier": "official"},
             {"rev": "v2", "kit": "drums", "profile": "community/drums", "tier": "community"}]
    return key, pub, header, dist, cells, profiles


def test_release_files(tmp_path):
    key, pub, header, dist, cells, profiles = setup(tmp_path)
    out = tmp_path / "out"
    doc = ci_release_assets.build_release(TAG, dist, out, cells, key, header, profiles)
    names = {p.name for p in out.iterdir()}
    for rev, kit in (("v1", "full"), ("v2", "full"), ("v2", "drums")):
        base = f"CrossPad-{TAG}-{rev}-{kit}"
        assert {f"{base}.bin", f"{base}-assets.bin", f"{base}-manifest.json", f"{base}-manifest.sig",
                f"{base}-icon.bin"} <= names
        assert fw_sign.verify(out / f"{base}-manifest.json", (out / f"{base}-manifest.sig").read_bytes(), pub)
        assert len((out / f"{base}-icon.bin").read_bytes()) == fw_icon.ICON_BYTES
    for rev in ("v1", "v2"):
        base = f"CrossPad-{TAG}-{rev}"
        assert {f"{base}.bin", f"{base}-assets.bin", f"{base}-bootloader.bin",
                f"{base}-partition-table.bin", f"{base}.elf"} <= names
    assert f"CrossPad-{TAG}-v2-drums-bootloader.bin" not in names
    m = json.loads((out / f"CrossPad-{TAG}-v2-drums-manifest.json").read_text())
    assert m["name"] == "drums-1.3.0-rc1" and m["tier"] == "community" and m["channel"] == "beta"
    assert m["display_name"] == "Drums" and "icon.bin" in m["files"]
    kits = json.loads((out / f"CrossPad-{TAG}-kits.json").read_text())
    assert kits == doc
    assert kits == {"v": 1, "tag": TAG, "kits": [
        {"kit": "drums", "tier": "community", "revs": ["v2"], "display_name": "Drums",
         "description": "The drums kit.", "author": "Ola", "apps": ["mixer", "sampler"]},
        {"kit": "full", "tier": "official", "revs": ["v1", "v2"], "display_name": "Full",
         "description": "The full kit.", "author": "", "apps": ["mixer", "sampler"]}]}


def test_components_are_what_the_default_kit_was_built_from(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    out = tmp_path / "out"
    ci_release_assets.build_release(TAG, dist, out, cells, key, header, profiles)
    assert json.loads((out / f"CrossPad-{TAG}-components.json").read_text()) == {"tag": TAG, "components": {
        "components/crosspad-core": "1" * 40, "components/crosspad-sampler": "2" * 40}}


def test_refuses_default_kit_cells_built_from_different_components(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v1-full" / "components.txt").write_text(COMPONENTS.replace("2" * 40, "3" * 40))
    with pytest.raises(SystemExit, match="different component commits"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_default_kit_cells_differing_only_in_describe_agree(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v1-full" / "components.txt").write_text(COMPONENTS.replace("(heads/main)", "(remotes/origin/main)"))
    out = tmp_path / "out"
    ci_release_assets.build_release(TAG, dist, out, cells, key, header, profiles)
    assert json.loads((out / f"CrossPad-{TAG}-components.json").read_text())["components"] == {
        "components/crosspad-core": "1" * 40, "components/crosspad-sampler": "2" * 40}


def test_refuses_a_default_kit_cell_without_its_components(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v2-full" / "components.txt").unlink()
    with pytest.raises(SystemExit, match="components.txt"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_refuses_a_header_that_cannot_verify(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    _, other = keypair(tmp_path, "other")
    header.write_text(fw_sign.header_text([fw_sign.public_der(other)]))
    with pytest.raises(SystemExit, match="fw_sign_pubkey.h"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_refuses_an_image_of_another_version(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v2-drums" / "CrossPad.bin").write_bytes(image(kit=b"drums", version=b"v1.3.0-rc1-dirty"))
    with pytest.raises(SystemExit, match="version"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_refuses_an_image_of_another_board(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v1-full" / "CrossPad.bin").write_bytes(image(board_rev=2, kit=b"full", version=TAG.encode()))
    out = tmp_path / "out"
    with pytest.raises(SystemExit, match="board v2, the cell is 'v1'"):
        ci_release_assets.build_release(TAG, dist, out, cells, key, header, profiles)
    assert not out.exists() or not list(out.iterdir())


def test_refuses_a_missing_cell(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    cells.append({"rev": "v1", "kit": "drums", "profile": "community/drums", "tier": "community"})
    with pytest.raises(SystemExit, match="build-v1-drums"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_refuses_a_missing_profile(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (profiles / "community" / "drums.json").unlink()
    with pytest.raises(SystemExit, match="drums.json"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


def test_a_failing_cell_leaves_out_empty(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    cells.append({"rev": "v1", "kit": "drums", "profile": "community/drums", "tier": "community"})
    out = tmp_path / "out"
    with pytest.raises(SystemExit):
        ci_release_assets.build_release(TAG, dist, out, cells, key, header, profiles)
    assert not out.exists() or not list(out.iterdir())


def test_refuses_an_image_of_another_kit(tmp_path):
    key, _, header, dist, cells, profiles = setup(tmp_path)
    (dist / "build-v2-drums" / "CrossPad.bin").write_bytes(image(kit=b"lite", version=TAG.encode()))
    with pytest.raises(SystemExit, match="kit"):
        ci_release_assets.build_release(TAG, dist, tmp_path / "out", cells, key, header, profiles)


TAG_RULES = json.loads((Path(__file__).parent / "fw_tag_rules.json").read_text())


@pytest.mark.parametrize("tag, channel", sorted(TAG_RULES["channel"].items()))
def test_signed_channel_is_the_shared_tag_rule(tmp_path, tag, channel):
    key, _, header, _, _, profiles = setup(tmp_path)
    dist = tmp_path / "dist_tag"
    cell(dist, "v2", "full", version=tag.encode())
    out = tmp_path / "out"
    ci_release_assets.build_release(tag, dist, out, [{"rev": "v2", "kit": "full", "profile": "full",
                                                      "tier": "official"}], key, header, profiles)
    assert json.loads((out / f"CrossPad-{tag}-v2-full-manifest.json").read_text())["channel"] == channel


@pytest.mark.parametrize("tag, rev, kit", [("1.3.0", "v2", "full"), ("v1.3.0/../x", "v2", "full"),
                                           ("v1.0.0-abcdefghijklmnopq", "v2", "full"),
                                           ("v1.3.0", "v2/..", "full"), ("v1.3.0", "x2", "full"),
                                           ("v1.3.0", "v2", "../full"), ("v1.3.0", "v2", "v3")])
def test_refuses_bad_names_before_touching_files(tmp_path, tag, rev, kit):
    key, _, header, dist, _, profiles = setup(tmp_path)
    out = tmp_path / "out"
    cells = [{"rev": rev, "kit": kit, "profile": "x", "tier": "official"}]
    with pytest.raises(SystemExit):
        ci_release_assets.build_release(tag, dist, out, cells, key, header, profiles)
    assert not out.exists()
