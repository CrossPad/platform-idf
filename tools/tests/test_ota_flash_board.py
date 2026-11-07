import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_flash  # noqa: E402
from test_fw_image_desc import image  # noqa: E402

REPO = Path("/repo")
V1_PORT, V2_PORT = "/dev/ttyACM1", "/dev/ttyACM3"


def dev(id_, cdc, board_rev=None, fw_rev=None, pcb=None):
    return {"id": id_, "board_rev": board_rev, "fw_rev": fw_rev, "pcb": pcb,
            "ports": {"cdc": {"path": cdc} if cdc else None}}


def choose(port, devices, explicit=None, board=None, force=False, device_id=None,
           image_revs=None, memory="v1"):
    revs = image_revs or {}
    return ota_flash.choose_image(port, devices, explicit, board, force, device_id,
                                  image_rev_fn=lambda p: revs.get(p),
                                  memory_fn=lambda: memory, project_dir=REPO)


def default_image(rev):
    return os.path.join(REPO, f"build_{rev}", "CrossPad.bin")


def test_board_without_revision_refuses_the_remembered_default():
    c = choose(V2_PORT, [dev("a", V2_PORT)])
    assert (c.path, c.code) == (None, ota_flash.EXIT_BENCH)
    assert "--board" in c.message and "advertises no revision" in c.message


def test_board_without_revision_takes_the_named_one():
    c = choose(V2_PORT, [dev("a", V2_PORT)], board="v2")
    assert (c.path, c.rev, c.code) == (default_image("v2"), "v2", ota_flash.EXIT_OK)


def test_board_without_revision_refuses_an_unverified_explicit_image():
    c = choose(V2_PORT, [dev("a", V2_PORT)], explicit="/b/CrossPad.bin",
               image_revs={"/b/CrossPad.bin": "v2"})
    assert (c.path, c.code) == (None, ota_flash.EXIT_BENCH)


def test_port_without_a_listed_board_is_unknown():
    c = choose(V2_PORT, [dev("a", "/dev/ttyACM9", board_rev="v2")])
    assert c.code == ota_flash.EXIT_BENCH and "CDC port" in c.message


def test_hil_missing_refuses_without_board():
    c = choose(V2_PORT, [])
    assert (c.path, c.code) == (None, ota_flash.EXIT_BENCH)
    assert "crosspad-hil" in c.message
    assert choose(V2_PORT, [], board="v2").path == default_image("v2")


def test_two_boards_v2_image_on_the_v1_port_is_refused():
    boards = [dev("a", V1_PORT, board_rev="v1"), dev("b", V2_PORT, board_rev="v2")]
    c = choose(V1_PORT, boards, explicit="/b2/CrossPad.bin",
               image_revs={"/b2/CrossPad.bin": "v2"})
    assert (c.path, c.code) == (None, ota_flash.EXIT_BENCH)
    assert "v1 board" in c.message and "image is v2" in c.message


def test_two_boards_default_follows_the_flashed_one():
    boards = [dev("a", V1_PORT, board_rev="v1"), dev("b", V2_PORT, board_rev="v2")]
    assert choose(V2_PORT, boards).path == default_image("v2")
    assert choose(V1_PORT, boards, memory="v2").path == default_image("v1")


def test_matching_board_takes_its_default_image():
    c = choose(V2_PORT, [dev("a", V2_PORT, board_rev="v2", fw_rev="v2", pcb=20)])
    assert (c.path, c.rev, c.code) == (default_image("v2"), "v2", ota_flash.EXIT_OK)


def test_firmware_revision_stands_in_for_the_hardware_one():
    c = choose(V2_PORT, [dev("a", V2_PORT, fw_rev="v2")], board="v1")
    assert c.code == ota_flash.EXIT_BENCH and "v2 board" in c.message


def test_board_option_naming_another_revision_is_refused():
    c = choose(V2_PORT, [dev("a", V2_PORT, board_rev="v2")], board="v1")
    assert (c.path, c.code) == (None, ota_flash.EXIT_BENCH)


def test_mismatch_forced_goes_through():
    c = choose(V2_PORT, [dev("a", V2_PORT, board_rev="v2")], explicit="/b1/CrossPad.bin",
               force=True, image_revs={"/b1/CrossPad.bin": "v1"})
    assert (c.path, c.code) == ("/b1/CrossPad.bin", ota_flash.EXIT_OK)
    assert "forced" in c.message


def test_force_on_an_unknown_board_falls_back_to_memory():
    c = choose(V2_PORT, [], force=True, memory="v2")
    assert c.path == default_image("v2")


def test_board_option_and_image_disagreeing_is_refused():
    c = choose(V2_PORT, [], explicit="/b1/CrossPad.bin", board="v2",
               image_revs={"/b1/CrossPad.bin": "v1"})
    assert c.code == ota_flash.EXIT_BENCH


def test_device_option_must_own_the_port():
    boards = [dev("a", V1_PORT, board_rev="v1"), dev("b", V2_PORT, board_rev="v2")]
    assert choose(V1_PORT, boards, device_id="b").code == ota_flash.EXIT_BENCH
    assert choose(V2_PORT, boards, device_id="b").path == default_image("v2")
    assert ota_flash.device_cdc_port("b", boards) == V2_PORT


def _with_sdkconfig(tmp_path, image_bytes, rev_str):
    build = tmp_path / "build_x"; build.mkdir()
    img = build / "CrossPad.bin"; img.write_bytes(image_bytes)
    config = build / "config"; config.mkdir()
    (config / "sdkconfig.json").write_text(json.dumps({"BSP_BOARD_REV_STR": rev_str}))
    return img


def test_image_rev_prefers_its_own_cpfi_block_over_sdkconfig(tmp_path):
    img = _with_sdkconfig(tmp_path, image(board_rev=1), "v2")
    assert ota_flash.image_rev(img) == "v1"


def test_image_rev_falls_back_to_sdkconfig_without_a_cpfi_block(tmp_path):
    img = _with_sdkconfig(tmp_path, image(with_desc=False), "v2")
    assert ota_flash.image_rev(img) == "v2"
