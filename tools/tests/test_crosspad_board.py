import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import crosspad_board as cb  # noqa: E402


def dev(id_, board_rev=None, fw_rev=None, pcb=None):
    return {"id": id_, "board_rev": board_rev, "fw_rev": fw_rev, "pcb": pcb}


def test_explicit_wins_over_everything(tmp_path):
    d = cb.resolve(tmp_path, explicit="v1", devices=[dev("a", board_rev="v2")])
    assert (d.rev, d.source) == ("v1", "explicit")


def test_hardware_wins_over_firmware_and_flags_mismatch(tmp_path):
    d = cb.resolve(tmp_path, devices=[dev("a", board_rev="v2", fw_rev="v1", pcb=20)])
    assert (d.rev, d.source, d.device, d.pcb, d.fw_rev) == ("v2", "device", "a", 20, "v1")
    assert d.mismatch


def test_firmware_used_when_hardware_unknown(tmp_path):
    d = cb.resolve(tmp_path, devices=[dev("a", fw_rev="v1")])
    assert (d.rev, d.source, d.mismatch) == ("v1", "device", False)


def test_memory_when_nothing_connected(tmp_path):
    (tmp_path / cb.LOCAL_CONFIG).write_text(json.dumps({"board": "v1"}))
    d = cb.resolve(tmp_path, devices=[])
    assert (d.rev, d.source) == ("v1", "memory")


def test_none_when_nothing_known(tmp_path):
    d = cb.resolve(tmp_path, devices=[dev("a")])
    assert (d.rev, d.source, d.build_dir) == (None, "none", None)


def test_two_revisions_connected_is_ambiguous(tmp_path):
    d = cb.resolve(tmp_path, devices=[dev("a", board_rev="v1"), dev("b", board_rev="v2")])
    assert (d.rev, d.source, len(d.devices)) == (None, "ambiguous", 2)


def test_device_filter_picks_one_of_two(tmp_path):
    d = cb.resolve(tmp_path, device="b",
                   devices=[dev("a", board_rev="v1"), dev("b", board_rev="v2")])
    assert (d.rev, d.device) == ("v2", "b")


def test_same_revision_twice_is_not_ambiguous(tmp_path):
    d = cb.resolve(tmp_path, devices=[dev("a", board_rev="v2"), dev("b", board_rev="v2")])
    assert d.rev == "v2"


def test_paths_and_idf_args():
    d = cb.Decision("v2", "explicit")
    assert d.build_dir == "build_v2" and d.sdkconfig == "sdkconfig.v2"
    assert d.idf_args() == ["-B", "build_v2", "-DSDKCONFIG=sdkconfig.v2"]


def test_commit_keeps_other_local_keys(tmp_path):
    (tmp_path / cb.LOCAL_CONFIG).write_text(json.dumps({"apps": {"x": {"track": "local"}}}))
    cb.commit(tmp_path, "v2")
    data = json.loads((tmp_path / cb.LOCAL_CONFIG).read_text())
    assert data == {"apps": {"x": {"track": "local"}}, "board": "v2"}


def test_write_vscode_points_the_extension_at_the_revision(tmp_path):
    vs = tmp_path / ".vscode"
    vs.mkdir()
    (vs / "settings.json").write_text(json.dumps({"idf.port": "/dev/ttyACM0"}))
    assert cb.write_vscode(tmp_path, "v1")
    data = json.loads((vs / "settings.json").read_text())
    assert data["idf.buildPath"] == "${workspaceFolder}/build_v1"
    assert data["idf.sdkconfigFilePath"] == "${workspaceFolder}/sdkconfig.v1"
    assert data["idf.port"] == "/dev/ttyACM0"


def test_write_vscode_without_settings_is_a_no(tmp_path):
    assert not cb.write_vscode(tmp_path, "v2")


def test_main_exit_codes(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cb, "REPO", tmp_path)
    monkeypatch.setattr(cb, "list_devices", lambda: [])
    assert cb.main(["--json"]) == cb.EXIT_BENCH
    assert json.loads(capsys.readouterr().out)["rev"] is None
    assert cb.main(["--set", "v2"]) == cb.EXIT_OK
    capsys.readouterr()
    assert cb.main(["--idf-args"]) == cb.EXIT_OK
    assert capsys.readouterr().out.strip() == "-B build_v2 -DSDKCONFIG=sdkconfig.v2"


def test_corrupt_local_config_survives_commit(tmp_path, capsys):
    corrupt_content = '{"apps": {"x": {"track": "local"}}, BROKEN'
    (tmp_path / cb.LOCAL_CONFIG).write_text(corrupt_content)
    cb.commit(tmp_path, "v2")
    # Corrupt content should survive byte-for-byte
    assert (tmp_path / cb.LOCAL_CONFIG).read_text() == corrupt_content
    # Error should be printed to stderr
    assert "not valid JSON" in capsys.readouterr().err


def test_unknown_device_with_memory_fallback(tmp_path):
    (tmp_path / cb.LOCAL_CONFIG).write_text(json.dumps({"board": "v1"}))
    d = cb.resolve(tmp_path, device="nonexistent",
                   devices=[dev("a", board_rev="v2")])
    # Should NOT fall back to memory; should report no-such-device
    assert (d.rev, d.source, d.device) == (None, "no-such-device", "nonexistent")


def test_main_unknown_device_json_exit_bench(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cb, "REPO", tmp_path)
    monkeypatch.setattr(cb, "list_devices", lambda: [dev("a", board_rev="v2")])
    (tmp_path / cb.LOCAL_CONFIG).write_text(json.dumps({"board": "v1"}))
    assert cb.main(["--device", "TYPO", "--json"]) == cb.EXIT_BENCH
    result = json.loads(capsys.readouterr().out)
    assert result["rev"] is None and result["source"] == "no-such-device"
