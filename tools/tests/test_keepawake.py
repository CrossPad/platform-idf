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


# -- meter: the STM's own gauge, which nothing in the ESP firmware reads ------

def _power_reply(pairs):
    """A `STM_REG 0x0200 <len>` block reply, with the named registers set."""
    vals = [0x00] * ka.POWER_BLOCK_LEN
    for addr, v in pairs.items():
        vals[addr - ka.REG_PWR_VBAT_HI] = v
    return "REG 0200: " + " ".join("%02X" % v for v in vals) + "\r\n"


def test_parse_reg_block_reads_a_block_or_returns_nothing():
    regs = ka.parse_reg_block("REG 0200: 0E C1 00 00 00 00 00 08\r\n")
    assert regs[0x0200] == 0x0E and regs[0x0201] == 0xC1         # VBAT hi, lo
    assert regs[0x0207] == 0x08                                 # CHG_STATUS: VBUS present
    assert ka.parse_reg_block("ERR i2c\r\n") == {}              # the link is down
    assert ka.parse_reg_block("") == {}


def test_the_gauge_is_signed_both_ways():
    assert ka.signed16(0x01, 0x2C) == 300        # discharging
    assert ka.signed16(0xFF, 0xF6) == -10        # charging: the battery is taking
    assert ka.signed16(0x00, 0x00) == 0
    assert ka.signed16(0x7F, 0xFF) == 32767
    assert ka.signed16(0x80, 0x00) == -32768     # the wrap that a uint16 read gets wrong


def test_the_block_asked_for_covers_the_gauge_and_fits_the_verb():
    # hil_control.cpp refuses an STM_REG longer than 64 bytes; the POWER block span
    # has to keep both gauge registers inside it and stay under that cap.
    assert ka.POWER_BLOCK_LEN == ka.REG_PWR_IBAT_LO - ka.REG_PWR_VBAT_HI + 1
    assert ka.POWER_BLOCK_LEN <= 64
    assert (ka.REG_PWR_IBAT_LO - ka.REG_PWR_VBAT_HI) < ka.POWER_BLOCK_LEN
    assert (ka.REG_PWR_CHG_STATUS - ka.REG_PWR_VBAT_HI) < ka.POWER_BLOCK_LEN


def test_meter_row_names_the_state_and_keeps_a_gap_a_gap():
    row = ka.meter_row("t", {ka.REG_PWR_STATE: 0x04},
                       {ka.REG_PWR_VBAT_HI: 0x0E, ka.REG_PWR_VBAT_LO: 0xC1,
                        ka.REG_PWR_IBAT_HI: 0x00, ka.REG_PWR_IBAT_LO: 0x3C,
                        ka.REG_PWR_CHG_STATUS: 0x08})
    assert row["name"] == "light"
    assert row["ma"] == 60 and row["mv"] == 0x0EC1 and row["chg"] == 0x08
    gap = ka.meter_row("t", {}, {})
    assert gap["ma"] is None and gap["mv"] is None and gap["name"] == "unknown"


def test_summarise_groups_by_state_and_says_how_many_samples_were_gaps():
    rows = [
        {"name": "awake", "ma": 300, "mv": 3900},
        {"name": "awake", "ma": 340, "mv": 3890},
        {"name": "light", "ma": 210, "mv": 3880},
        {"name": "unknown", "ma": None, "mv": None},
    ]
    groups, gaps = ka.summarise(rows)
    assert gaps == 1
    assert groups["awake"]["n"] == 2 and groups["awake"]["mean"] == 320
    assert groups["awake"]["min"] == 300 and groups["awake"]["max"] == 340
    assert groups["light"]["mean"] == 210
    assert "unknown" not in groups              # a gap is not a sample of a state


def test_meter_main_logs_the_run_and_prints_the_states_apart(tmp_path, capsys):
    states = ["REG 012D: 04\r\n", "REG 012D: 04\r\n", "REG 012D: 00\r\n"]
    powers = [
        _power_reply({0x0200: 0x0E, 0x0201: 0xC1, 0x0207: 0x00, 0x0229: 0x00, 0x022A: 0x5A}),
        _power_reply({0x0200: 0x0E, 0x0201: 0xC0, 0x0207: 0x00, 0x0229: 0x00, 0x022A: 0x3C}),
        _power_reply({0x0200: 0x0E, 0x0201: 0xBF, 0x0207: 0x00, 0x0229: 0x01, 0x022A: 0x2C}),
    ]

    def fake_cdc(base, words, expect=None, timeout=30.0):
        return 0, (states.pop(0) if words[1] == "0x012D" else powers.pop(0))

    out = tmp_path / "m.csv"
    rc = ka.meter_main(["hil"], 30, 600, str(out), cdc=fake_cdc,
                       clock=lambda: 0.0, pause=lambda s: None, now="test")
    assert rc == ka.EXIT_OK
    lines = out.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "time,pwr_state,state,ibat_ma,vbat_mv,chg_flags"
    assert len(lines) == 4                                   # header + 3 samples
    assert lines[1].count(",") == 5 and ",light," in lines[1]
    assert lines[1].split(",")[1] == "04"                    # the state, as the STM reports it
    assert lines[1].split(",")[3] == "90"                    # 0x005A = 90 mA giving, dim
    assert lines[3].split(",")[3] == "300"                   # 0x012C = 300 mA giving, awake
    printed = capsys.readouterr().out
    assert "light" in printed and "awake" in printed
    assert "awake minus dim" in printed
    assert not states and not powers                          # both reads used, per sample


def test_meter_main_stops_when_the_gauge_answers_nothing(tmp_path, capsys):
    def dead_cdc(base, words, expect=None, timeout=30.0):
        return 0, "ERR i2c\r\n"

    out = tmp_path / "dead.csv"
    rc = ka.meter_main(["hil"], 30, 600, str(out), cdc=dead_cdc,
                       clock=lambda: 0.0, pause=lambda s: None, now="test")
    assert rc == ka.EXIT_UNREACHABLE
    printed = capsys.readouterr().out
    assert "answers nothing" in printed
    # Only the header: a file of nothing but zeros would read like a measurement.
    assert out.read_text(encoding="utf-8").splitlines() == ["time,pwr_state,state,ibat_ma,vbat_mv,chg_flags"]