import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import factory_provision as fp  # noqa: E402


def dev(console_serial, id_="dev_1"):
    return {"id": id_, "serial": "AA:BB:CC:DD:EE:FF",
            "ports": {"console": None if console_serial is None else {"serial": console_serial}}}


def test_board_serial_from_stm_console():
    assert fp.board_serial([dev("CP1234")], None) == ("CP1234", None)


def test_board_serial_not_provisioned():
    s, err = fp.board_serial([dev("0123456789AB")], None)
    assert s is None and "otp_provision.py" in err


def test_board_serial_two_boards_refused_even_with_device():
    # the flash port is the only board's STM bridge, so naming one board
    # among two would still flash through whichever crosspad-hil lists
    two = [dev("CP1234", "dev_1"), dev("CP1235", "dev_2")]
    for device in (None, "dev_2"):
        s, err = fp.board_serial(two, device)
        assert s is None and "one board" in err


def test_board_serial_device_must_match():
    s, err = fp.board_serial([dev("CP1234", "dev_1")], "dev_9")
    assert s is None and "dev_9" in err


def test_board_serial_no_board():
    s, err = fp.board_serial([], None)
    assert s is None and err


def test_check_serial():
    assert fp.check_serial("CP1234", None) is None
    assert fp.check_serial("CP1234", "cp1234") is None
    assert "CP1235" in fp.check_serial("CP1234", "CP1235")


def test_write_log_appends(tmp_path, monkeypatch):
    monkeypatch.setattr(fp, "FACTORY_LOG_DIR", tmp_path)
    fp.write_log("CP1234", ["first"])
    fp.write_log("CP1234", ["second"])
    text = (tmp_path / "CP1234.log").read_text()
    assert "first" in text and "second" in text
