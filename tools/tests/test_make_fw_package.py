import hashlib, json, re, shutil, subprocess, sys
from pathlib import Path

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import make_fw_package
from test_fw_image_desc import image


def test_builds_package(tmp_path):
    build = tmp_path / "build_v2"; build.mkdir()
    (build / "CrossPad.bin").write_bytes(image())
    (build / "assets.bin").write_bytes(b"A" * 4096)
    (build / "assets_id.txt").write_text(bytes(range(32)).hex())
    out = make_fw_package.build(build, tmp_path / "out", "sampler-1.2.3")
    m = json.loads((out / "manifest.json").read_text())
    assert m["name"] == "sampler-1.2.3" and m["board_rev"] == 2 and m["kit"] == "sampler"
    assert m["files"]["CrossPad.bin"]["sha256"] == hashlib.sha256(image()).hexdigest()
    assert (out / "assets.bin").read_bytes() == b"A" * 4096


def test_refuses_mismatched_assets(tmp_path):
    build = tmp_path / "b"; build.mkdir()
    (build / "CrossPad.bin").write_bytes(image())
    (build / "assets.bin").write_bytes(b"A")
    (build / "assets_id.txt").write_text("00" * 32)
    try:
        make_fw_package.build(build, tmp_path / "out", "x")
    except SystemExit as e:
        assert "assets" in str(e)
    else:
        raise AssertionError("expected refusal")


import fw_sign
from test_fw_sign import keypair


def _build_dir(tmp_path, version=b"1.3.0"):
    build = tmp_path / "build_v2"
    build.mkdir(parents=True)
    (build / "CrossPad.bin").write_bytes(image(kit=b"full", version=version))
    (build / "assets.bin").write_bytes(b"A" * 4096)
    (build / "assets_id.txt").write_text(bytes(range(32)).hex())
    return build


def test_channel_follows_the_tag(tmp_path):
    out = make_fw_package.build(_build_dir(tmp_path, b"v1.3.0-rc1"), tmp_path / "o", "full-1.3.0-rc1",
                                tag="v1.3.0-rc1")
    m = json.loads((out / "manifest.json").read_text())
    assert m["channel"] == "beta" and m["tier"] == "official" and m["tag"] == "v1.3.0-rc1"
    out = make_fw_package.build(_build_dir(tmp_path / "s", b"v1.3.0"), tmp_path / "o2", "full-1.3.0",
                                tag="v1.3.0", tier="community")
    m = json.loads((out / "manifest.json").read_text())
    assert m["channel"] == "stable" and m["tier"] == "community"


TAG_RULES = json.loads((Path(__file__).parent / "fw_tag_rules.json").read_text())


@pytest.mark.parametrize("tag, channel", sorted(TAG_RULES["channel"].items()))
def test_channel_rule_is_the_shared_one(tmp_path, tag, channel):
    assert make_fw_package.tag_channel(tag) == channel
    out = make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tag=tag)
    assert json.loads((out / "manifest.json").read_text())["channel"] == channel


def _core_fw_version_max():
    header = Path(__file__).resolve().parents[2] / "components/crosspad-core/include/crosspad/fw/Semver.hpp"
    return int(re.search(r"kFwVersionMax\s*=\s*(\d+)", header.read_text()).group(1))


def test_tag_limit_is_what_a_board_holds():
    # The manifest keeps the tag's 'v'; fwVersionValid allows kFwVersionMax - 1 characters.
    assert make_fw_package.TAG_MAX_LEN == _core_fw_version_max() - 1
    assert make_fw_package.TAG_MAX_LEN == TAG_RULES["tag_max"]
    assert len(max(TAG_RULES["accepted"], key=len)) == TAG_RULES["tag_max"]


@pytest.mark.parametrize("tag", TAG_RULES["accepted"])
def test_shared_accepted_tags(tmp_path, tag):
    make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tag=tag)


@pytest.mark.parametrize("tag", TAG_RULES["refused"])
def test_shared_refused_tags(tmp_path, tag):
    with pytest.raises(SystemExit, match="tag"):
        make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tag=tag)


def test_local_package_without_a_tag(tmp_path):
    out = make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "bench")
    m = json.loads((out / "manifest.json").read_text())
    assert m["channel"] == "stable" and m["tag"] == "" and not (out / "manifest.sig").exists()


def test_signed_package_verifies(tmp_path):
    key, pub = keypair(tmp_path)
    out = make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "full-1.3.0", tag="v1.3.0", sign=key)
    assert fw_sign.verify(out / "manifest.json", (out / "manifest.sig").read_bytes(), pub)


def test_rejects_unknown_tier_and_channel(tmp_path):
    with pytest.raises(SystemExit):
        make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tier="vendor")
    with pytest.raises(SystemExit):
        make_fw_package.build(_build_dir(tmp_path / "c"), tmp_path / "o2", "x", channel="nightly")


@pytest.mark.parametrize("tag", ["v1.3.0", "v1.3.0-rc1", "v10.0.1-beta.2+x"])
def test_good_tags(tmp_path, tag):
    make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tag=tag)


@pytest.mark.parametrize("tag", ["", "1.3.0", "v1.3", "v1.3.0 x", "v1.3.0\n", "v1.3.0/../x", "v1.3.0-" + "a" * 40])
def test_bad_tags(tmp_path, tag):
    with pytest.raises(SystemExit, match="tag"):
        make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "x", tag=tag)


def test_failed_signing_leaves_no_stale_signature(tmp_path):
    key, _ = keypair(tmp_path)
    build = _build_dir(tmp_path)
    out = make_fw_package.build(build, tmp_path / "o", "x", sign=key)
    assert (out / "manifest.sig").exists()
    with pytest.raises(SystemExit):
        make_fw_package.build(build, tmp_path / "o", "x", sign=tmp_path / "missing.pem")
    assert not (out / "manifest.sig").exists()


CORE = Path(__file__).resolve().parents[2] / "components/crosspad-core"
ARDUINOJSON = Path(__file__).resolve().parents[2] / "managed_components/bblanchon__ArduinoJson/src"
HARNESS = """#include "crosspad/fw/FwCatalog.hpp"
#include <cstdio>
#include <iostream>
#include <string>
int main() { std::string l; while (std::getline(std::cin, l)) std::printf("%d\\n", crosspad::fwVersionValid(l.c_str())); }
"""


def _tag_corpus():
    tokens = ("-", ".", "+", "a", "1", "-dirty", "-5-gabcd")
    tails = [""]
    for _ in range(4):
        tails = [t + k for t in tails for k in tokens] + tails
    tags = {"v1.2.3" + t for t in tails}
    tags |= {"v65535.0.0", "v65536.0.0", "v0.0.0", "v01.2.3", "v1.2.99999", "v1.2.3-99999-gabcd", "v1.2.3-a.-99999-gabcd"}
    return sorted(tags | set(TAG_RULES["accepted"]) | {t for t in TAG_RULES["refused"] if "\n" not in t})


def test_tag_rule_is_core_fw_version_valid(tmp_path):
    """tag_valid() is fwVersionValid() for every v<N>.<N>.<N> string: core's own code
    judges a corpus of identifier shapes, describe and -dirty tails included."""
    cxx = shutil.which("g++")
    if not cxx or not (ARDUINOJSON / "ArduinoJson.h").is_file():
        pytest.skip("needs g++ and the managed ArduinoJson component (a configured build)")
    src = tmp_path / "h.cpp"
    src.write_text(HARNESS)
    exe = tmp_path / "h"
    subprocess.run([cxx, "-std=c++17", f"-I{CORE / 'include'}", f"-I{ARDUINOJSON}", str(src),
                    *(str(CORE / "src/fw" / f) for f in ("FwCatalog.cpp", "Semver.cpp", "FwKit.cpp")),
                    "-o", str(exe)], check=True)
    corpus = _tag_corpus()
    out = subprocess.run([str(exe)], input="\n".join(corpus) + "\n", capture_output=True, text=True, check=True)
    core = dict(zip(corpus, (line == "1" for line in out.stdout.split())))
    shaped = [t for t in corpus if re.match(r"v[0-9]+\.[0-9]+\.[0-9]+", t)]
    assert [t for t in shaped if make_fw_package.tag_valid(t) != core[t]] == []
    assert all(core[t] for t in TAG_RULES["accepted"])


import fw_icon
from test_ci_matrix import profile


def test_profile_adds_display_fields_and_a_signed_icon(tmp_path):
    prof = profile(tmp_path / "profiles" / "full.json", display_name="Full", description="The full kit.",
                   apps={"sampler": {"track": "registry"}, "mixer": {"track": "registry"}})
    out = make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "full-1.3.0", tag="v1.3.0", profile=prof)
    m = json.loads((out / "manifest.json").read_text())
    assert (m["display_name"], m["description"], m["author"], m["apps"]) == ("Full", "The full kit.", "",
                                                                           ["mixer", "sampler"])
    icon = (out / "icon.bin").read_bytes()
    assert len(icon) == fw_icon.ICON_BYTES
    assert m["files"]["icon.bin"] == {"size": len(icon), "sha256": hashlib.sha256(icon).hexdigest()}


def test_without_a_profile_no_icon_and_no_display_fields(tmp_path):
    out = make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "bench")
    m = json.loads((out / "manifest.json").read_text())
    assert not (out / "icon.bin").exists()
    assert "icon.bin" not in m["files"] and "display_name" not in m


def test_cli_profile_icon_path_is_validated(tmp_path):
    prof = profile(tmp_path / "profiles" / "full.json", icon="../elsewhere.png")
    with pytest.raises(SystemExit, match="icon"):
        make_fw_package.build(_build_dir(tmp_path), tmp_path / "o", "full-1.3.0", tag="v1.3.0", profile=prof)
