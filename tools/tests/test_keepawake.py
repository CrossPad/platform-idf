import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import keepawake as ka  # noqa: E402

# A real reply shape from main/hil_control.cpp's PWR verb. The board was in
# LIGHT sleep when this was read, and the STM's timeouts had been poked.
LINE = ("PWR: state=0x04 cached=0x04 link=ok | stm dim=5s sleep=10s auto=1 | "
        "stored dim=600s sleep=1800s auto=1 | lease=0ms host_quiet=31000ms cmds=42 "
        "relights=3 nudges=5 i2c_ok=5 i2c_fail=0 manual=1")


def test_parse_reads_the_pwr_line():
    info = ka.parse_pwr("air>\r\n" + LINE + "\r\n")
    assert info["state"] == 0x04                      # hex, as the STM publishes it
    assert info["stm_dim"] == 5 and info["stm_sleep"] == 10
    assert info["stored_dim"] == 600 and info["stored_sleep"] == 1800
    assert info["host_quiet"] == 31000 and info["nudges"] == 5
    assert info["link"] == "ok"


def test_parse_tolerates_a_line_that_is_not_there_and_new_keys():
    assert ka.parse_pwr("OK\r\n") == {}
    info = ka.parse_pwr("PWR: state=0x00 brand_new=7 lease=1ms")
    assert info["state"] == 0x00 and "brand_new" not in info


def test_a_dim_board_is_one_write_away_from_working():
    action, why = ka.decide(ka.parse_pwr(LINE), None)
    assert action == "wake"
    assert "USER_ACTIVITY" in why          # what to do, not just that it is bad
    assert "dark" in why


def test_an_awake_board_needs_nothing():
    action, why = ka.decide({"state": ka.AWAKE, "host_quiet": 9000}, None)
    assert action == "ok" and "quiet 9000s" in why


def test_waking_from_soft_off_is_beyond_a_host_and_says_who_can():
    action, why = ka.decide({}, None)      # no CDC at all
    assert action == "lost"
    assert "power button" in why and "soft-off" in why


def test_a_board_in_download_mode_is_a_tool_that_touched_the_reset_lines():
    action, why = ka.decide({}, "bootloader")
    assert action == "lost"
    assert "DTR/RTS" in why and "recover" in why


def test_the_state_of_an_older_firmware_is_read_straight():
    # A board flashed before the PWR verb still answers STM_REG.
    assert ka.parse_reg_state("air>\r\nREG 012D: 04\r\n") == 0x04
    assert ka.parse_reg_state("ERR i2c\r\n") is None      # the bus hang, not a state
    assert ka.parse_reg_state("REG 0301: 01") is None     # the wrong register


def test_sleep_pending_is_lost_before_the_console_goes():
    for state in (ka.SLEEP_PENDING, ka.SHIP_PENDING):
        action, why = ka.decide({"state": state}, None)
        assert action == "lost", state
        assert "soft-off" in why or "ship" in why


def test_a_link_that_fails_is_reported_as_the_bus_not_as_sleep():
    action, why = ka.decide({"state": ka.AWAKE, "i2c_fail": 4}, None)
    assert action == "wake" and "I2C" in why


def test_hil_path_prefers_the_override_and_exists_or_none():
    import os
    os.environ["CROSSPAD_HIL"] = str(Path(__file__).resolve())     # any real file
    assert ka.hil_path() == str(Path(__file__).resolve())
    os.environ["CROSSPAD_HIL"] = str(Path(__file__).resolve().parent / "nope-not-here")
    # The candidate is gone; the fallbacks may still find a real install, so all
    # this may assert is that a path that does not exist is not returned.
    found = ka.hil_path()
    assert found is None or Path(found).exists()
