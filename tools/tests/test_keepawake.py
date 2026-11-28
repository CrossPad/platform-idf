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

def test_woke_is_a_board_that_is_on_not_one_that_is_dim():
    """Measured on the second v2 board on this bench, 28.11.2026: REG_IN_PWR_STATE =
    0x03 (PWR_STATE_WOKE) with the Sampler running on the display and all sixteen pads
    lit, and it stayed 0x03 the whole time auto-sleep was off in Settings. The rule
    used to group WOKE with LIGHT and tell the operator to wake a board that was
    already on -- harmless to write, misleading to read, which is the part that
    matters at 2 a.m.
    """
    action, why = ka.decide({"state": ka.WOKE}, None)
    assert action == "ok" and "soft-off" in why
    action, why = ka.decide({"state": ka.LIGHT}, None)
    assert action == "wake" and "dim" in why


def test_the_meter_names_every_state_the_gauge_can_answer():
    # These names are what the CSV and the per-state summary group by, so a state that
    # is missing here silently becomes "unknown" and its samples are dropped.
    for value, name in ((0, "awake"), (1, "sleep_pending"), (2, "ship_pending"),
                        (3, "woke"), (4, "light"), (5, "charge")):
        assert ka.STATE_NAMES[value] == name



def _block(pmil, pct=99, flags=0x0F, chg=0x00, mv=4100, ma=0):
    """A 43-byte POWER block, answered the way `STM_REG 0x0200 43` answers it.

    `pmil` is in the units the register actually uses: SoC percent times ten, so
    993 is 99.3 % -- one unit is 0.1 % of the pack, which is 4 mAh of a 4000 mAh
    cell. Getting this wrong is how a "25 permil" difference turns into 25 % of
    the battery on paper."""
    b = [0] * 43
    b[0x00], b[0x01] = (mv >> 8) & 0xFF, mv & 0xFF          # REG_PWR_VBAT_HI/LO
    b[0x07] = chg                                           # REG_PWR_CHG_STATUS
    b[0x23] = pct                                           # REG_PWR_SOC_PCT
    b[0x24], b[0x25] = (pmil >> 8) & 0xFF, pmil & 0xFF      # REG_PWR_SOC_PMIL_HI/LO
    b[0x26] = flags                                         # REG_PWR_SOC_FLAGS
    b[0x29], b[0x2A] = (ma >> 8) & 0xFF, ma & 0xFF          # REG_PWR_IBAT_HI/LO
    return "REG 0200: " + " ".join("%02X" % x for x in b)


def _gauge(units, **kw):
    """A gauge dict, `units` in 0.1 % of the pack."""
    return {"t": kw.get("t", 0.0), "state": kw.get("state", 0), "soc_permil": units,
            "soc_pct": kw.get("pct", 99), "soc_flags": kw.get("flags", 0x0F),
            "chg": kw.get("chg", 0x00), "mv": kw.get("mv", 4100), "ma": kw.get("ma", 0)}


def test_two_gauge_points_become_an_average_current():
    """30 minutes across which the estimate falls from 95.0 % to 92.5 % of a
    4000 mAh pack: 2.5 % is 100 mAh, so 200 mA. This is the arithmetic the
    idle-power ladder is gated on, and nobody has had the number before."""
    res = ka.drain_estimate(_gauge(950, t=1000.0), _gauge(925, t=2800.0))
    assert res["delta_permil"] == 25              # 25 units of 0.1 % = 2.5 % of the pack
    assert res["mah"] == 100.0
    assert abs(res["ma"] - 200.0) < 1e-9
    assert res["why"] == []


def test_a_battery_being_charged_is_reported_as_such_and_not_as_a_negative_load():
    res = ka.drain_estimate(_gauge(800, t=0.0), _gauge(820, t=1800.0))
    assert res["ma"] is not None and res["ma"] < 0
    assert any("charged" in w for w in res["why"])


def test_a_change_the_estimator_cannot_tell_from_its_own_settle_becomes_a_bound():
    # 2 units is 8 mAh: over half an hour that computes to a plausible-sounding
    # 16 mA, built entirely out of the EKF re-converging on a rested pack.
    res = ka.drain_estimate(_gauge(900, t=0.0), _gauge(898, t=1800.0))
    assert res["ma"] is not None
    assert any("upper bound" in w for w in res["why"])


def test_a_mark_taken_with_usb_in_is_flagged_because_that_figure_is_the_chargers():
    mark = _gauge(950, t=0.0, chg=ka.CHG_FLAG_VBUS_PRESENT | 0x02)
    res = ka.drain_estimate(mark, _gauge(925, t=1800.0))
    assert any("USB was present" in w for w in res["why"])


def test_an_estimate_the_gauge_does_not_vouch_for_is_said_so_at_both_ends():
    mark = _gauge(950, t=0.0, flags=ka.SOC_FLAG_CONVERGED)   # the valid bit is missing
    res = ka.drain_estimate(mark, _gauge(925, t=1800.0, flags=0x00))
    assert sum(1 for w in res["why"] if "invalid" in w) == 2


def test_a_missing_reading_ends_the_window_instead_of_inventing_a_number():
    res = ka.drain_estimate(_gauge(950, t=0.0), None)
    assert res["ma"] is None and res["mah"] is None
    assert res["why"] == ["the gauge did not answer at both ends of the window"]
    res2 = ka.drain_estimate(_gauge(950, t=500.0), _gauge(925, t=500.0))
    assert res2["ma"] is None and "not positive" in res2["why"][0]


def test_the_constants_the_note_quotes_are_the_ones_the_code_uses():
    # "one unit of this register is 0.1 % of the pack, which is 4 mAh of a 4000 mAh
    # cell" is the claim a person decides on; it has to be true of the arithmetic.
    assert ka.BATTERY_MAH == 4000
    assert 1 / 1000.0 * ka.BATTERY_MAH == 4.0
    assert ka.SOC_GRANULARITY / 1000.0 * ka.BATTERY_MAH == 12.0


def test_mark_then_calc_round_trips_against_a_scripted_console(tmp_path):
    """The whole two-command flow with no board: what `drain mark` writes has to be
    what `drain calc` reads, and the number out has to be the number the two
    readings imply."""
    blocks = iter([_block(950, chg=0x00), _block(925, chg=ka.CHG_FLAG_VBUS_PRESENT)])
    calls = []

    def fake(base, words, expect=None, timeout=30.0):
        calls.append(words)
        if words[1] == "0x012D":
            return True, "REG 012D: 00"
        return True, next(blocks)

    path = str(tmp_path / "mark.json")
    assert ka.drain_main(["hil"], "mark", path, 30, cdc=fake, clock=lambda: 1000.0) == ka.EXIT_OK
    assert ka.drain_main(["hil"], "calc", path, 30, cdc=fake, clock=lambda: 2800.0) == ka.EXIT_OK
    with open(path.replace(".json", "-result.json"), encoding="utf-8") as fh:
        import json as _json
        out = _json.load(fh)
    assert out["mark"]["soc_permil"] == 950 and out["later"]["soc_permil"] == 925
    assert abs(out["result"]["ma"] - 200.0) < 1e-9
    assert len(calls) == 4                      # two reads at each end, and no more


def test_calc_without_a_mark_says_where_to_start_rather_than_guessing(tmp_path):
    def fake(base, words, expect=None, timeout=30.0):
        return True, "REG 012D: 00" if words[1] == "0x012D" else _block(940)
    rc = ka.drain_main(["hil"], "calc", str(tmp_path / "nothing-here.json"), 30, cdc=fake)
    assert rc == ka.EXIT_UNREACHABLE


def test_an_unreachable_gauge_stops_the_flow_at_the_near_end(tmp_path):
    def deaf(base, words, expect=None, timeout=30.0):
        return False, "ERR i2c"
    mark = str(tmp_path / "m.json")
    assert ka.drain_main(["hil"], "mark", mark, 30, cdc=deaf) == ka.EXIT_UNREACHABLE
    import os
    assert not os.path.exists(mark)             # nothing written when nothing answered


def test_the_mark_remembers_the_state_the_board_was_in(tmp_path):
    """A drain measured while the board sits dim is not the drain of the same state,
    so the mark has to carry the power state it was taken in: 0x012D on the wire, its
    number in the file."""
    blocks = iter([_block(950, chg=0x00)])

    def fake(base, words, expect=None, timeout=30.0):
        if words[1] == "0x012D":
            return True, "REG 012D: 04"              # PWR_STATE_LIGHT: the dim state
        return True, next(blocks)

    path = str(tmp_path / "m.json")
    assert ka.drain_main(["hil"], "mark", path, 5, cdc=fake, clock=lambda: 100.0) == ka.EXIT_OK
    import json as _json
    with open(path, encoding="utf-8") as fh:
        mark = _json.load(fh)
    assert mark["state"] == 4 and mark["soc_permil"] == 950 and mark["t"] == 100.0