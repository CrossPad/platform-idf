"""tools/help_facts.py: the Help page against the firmware's own facts.

Each drift test replays one that shipped (v1.1.2 / v1.1.3 Help) on the
current page and expects the check to name it.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import help_facts as hf

ROOT = Path(__file__).resolve().parents[2]
HAVE_SOURCES = (ROOT / hf.MODE_MAP).is_file() and (ROOT / hf.KIT_SELECTOR).is_file()
needs_sources = pytest.mark.skipif(not HAVE_SOURCES, reason="crosspad-gui/sampler/core not checked out")

FACTS = {
    "mode_map": {1: "Sampler", 2: "Seq", 3: "Song", 4: "Arrange", 5: "Record", 6: "Slicer",
                 8: "More", 9: "Pitch", 10: "Pad Mix", 11: "Mixer", 12: "DAW", 13: "Kits",
                 14: "Help", 15: "Update", 16: "Setup"},
    "tracks": 8,
    "kit_tabs": ["PERF", "DRUMS", "SLICE", "PITCH", "SONGS", "USER"],
    "kit_keys": {"+ NEW KIT", "+ NEW DRUMS"},
    "off_ms": 2400, "off_max_ms": 3700, "off_opts": ["1s", "1.5s", "2s", "2.4s", "3s"],
    "dim_s": 600, "sleep_s": 1800, "timeout_presets": [0, 30, 60, 300, 600, 1800, 3600],
    "gui_strings": {"System", "Audio", "Output Volume", "USB", "Mode", "Info", "Power Long-Press"},
    "ship_ms": 4200, "wake_ms": 1500, "short_hold_ms": 500,
}

PAGE = """# CrossPad

## Power button

- Switch on: hold 1.5 s, then let go.
- Switch off: hold 2.4 s, then let go. Change it in Settings > System > Power Long-Press.
- Ship mode: on battery, a 4.2 s hold cuts the battery for storage.
- Hold about 0.5 s, then let go: jump to the transport bar.

## MODE map

- 1 Sampler: play
- 2 Seq: x
- 3 Song: x
- 4 Arrange: eight tracks
- 5 Record: x
- 6 Slicer: x
- 7: empty
- 8 More: the other apps
- 9 Pitch: x
- 10 Pad Mix: x
- 11 Mixer: x
- 12 DAW: x
- 13 Kits: x
- 14 Help: x
- 15 Update: x
- 16 Setup: Settings

## Kits

- The tabs on the left sort what is on the card.
- PERF: performance kits. + NEW KIT makes an empty one.
- DRUMS: drum kits. + NEW DRUMS makes an empty one.
- SLICE: x
- PITCH: x
- SONGS: x
- USER: x

## Volume

- Settings > Audio > Output Volume: the sliders.
- Settings > USB > Mode. Settings > Info shows them too.

## Power saving

- Without use, the screen dims after 10 minutes and the CrossPad switches off after 30 minutes.
- Change both in Settings > System: Dim After and Sleep After.
"""


def test_a_matching_page_passes():
    assert hf.check(PAGE, FACTS) == []


def test_old_hold_times_fail():  # v1.1.2 Help: 4 s / 7 s
    page = PAGE.replace("hold 2.4 s", "hold 4 s").replace("a 4.2 s hold", "a 7 s hold")
    problems = "\n".join(hf.check(page, FACTS))
    assert "off after 4 s, default is 2.4 s" in problems
    assert "4 s is not a Power Long-Press choice" in problems
    assert "ship after 7 s, the STM ships at 4.2 s" in problems


def test_without_the_stm_only_the_order_of_holds_is_checked():
    facts = dict(FACTS, ship_ms=None, wake_ms=None, short_hold_ms=None)
    assert hf.check(PAGE.replace("a 4.2 s hold", "a 5 s hold"), facts) == []
    problems = hf.check(PAGE.replace("a 4.2 s hold", "a 3 s hold"), facts)
    assert any("not above the longest power-off hold" in p for p in problems)


def test_switch_on_and_short_hold_follow_the_stm():
    page = PAGE.replace("Switch on: hold 1.5 s", "Switch on: hold 1 s").replace(
        "Hold about 0.5 s", "Hold about 1 s")
    problems = "\n".join(hf.check(page, FACTS))
    assert "on after 1 s, the STM wakes at 1.5 s" in problems
    assert "hold 1 s for the transport bar" in problems


def test_legacy_tab_fails():  # v1.1.3 Help
    page = PAGE.replace("- USER: x", "- LEGACY: x\n- USER: x")
    problems = hf.check(page, FACTS)
    assert any("PERF / DRUMS / SLICE / PITCH / SONGS / LEGACY / USER" in p for p in problems)


def test_tabs_out_of_order_fail():
    page = PAGE.replace("- SONGS: x\n- USER: x", "- USER: x\n- SONGS: x")
    assert any(p.startswith("Kits: Help lists tabs") for p in hf.check(page, FACTS))


def test_unknown_new_key_fails():
    page = PAGE.replace("+ NEW DRUMS", "+ NEW PITCH")
    assert any("'+ NEW PITCH'" in p for p in hf.check(page, FACTS))


def test_pad_8_empty_fails():  # v1.1.3 Help: pads 7 and 8 empty
    page = PAGE.replace("- 8 More: the other apps", "- 8: empty")
    assert hf.check(page, FACTS) == ["MODE map: pad 8 is 'More' in the firmware, Help says 'empty'"]


def test_empty_pad_given_a_label_fails():
    page = PAGE.replace("- 7: empty", "- 7 Doom: a game")
    assert hf.check(page, FACTS) == ["MODE map: pad 7 is empty in the firmware, Help says 'Doom'"]


def test_missing_and_duplicate_pads_fail():
    page = PAGE.replace("- 12 DAW: x\n", "").replace("- 13 Kits: x", "- 13 Kits: x\n- 13 Kits: y")
    problems = hf.check(page, FACTS)
    assert "MODE map: pad 12 missing (firmware: DAW)" in problems
    assert "MODE map: pad 13 listed twice" in problems


def test_four_tracks_fails():  # v1.1.3 Help
    page = PAGE.replace("eight tracks", "four tracks")
    assert hf.check(page, FACTS) == ["Help says four tracks, AudioSong::kTracks = 8"]


def test_dim_and_sleep_defaults():
    page = PAGE.replace("dims after 10 minutes", "dims after 5 minutes")
    assert hf.check(page, FACTS) == ["Power saving: Help says dim after 5 min, default is 10 min (600 s)"]
    facts = dict(FACTS, sleep_s=1000)
    page = PAGE.replace("after 30 minutes", "after 16 minutes")
    problems = "\n".join(hf.check(page, facts))
    assert "default sleep 1000 s is not a Settings choice" in problems


def test_renamed_settings_row_fails():
    facts = dict(FACTS, gui_strings=FACTS["gui_strings"] - {"Power Long-Press"} | {"Power Hold"})
    assert hf.check(PAGE, facts) == [
        "Help names 'Settings > System > Power Long-Press': no 'Power Long-Press' in crosspad-gui"]


def test_a_missing_statement_is_reported():
    page = PAGE.replace("- Switch off: hold 2.4 s, then let go.", "- Switch off: hold it.")
    assert any("no power-off hold found" in p for p in hf.check(page, FACTS))


def test_source_parsers():
    src = '''const ModeEntry kDefaultMap[] = {
    {  1, "Sampler",  "play",  kTheme, ModeKind::Core, "Sampler", nullptr, nullptr },
    { 8, "More", "apps", 0x7B828B, ModeKind::More, nullptr, nullptr, nullptr },
};'''
    assert hf.mode_map(src) == {1: "Sampler", 8: "More"}
    assert hf.kit_tabs('const char* const kTypeName[kTypes] = {"PERF", "USER"};') == ["PERF", "USER"]
    assert hf.u16_list("static const uint16_t kTimeoutSecs[] = { 0, 30, 60 }; // 0", "kTimeoutSecs") == [0, 30, 60]
    assert hf.opts(r'static const char* kLongPressOpts = "1s\n1.5s";', "kLongPressOpts") == ["1s", "1.5s"]
    assert hf.define_ms("#define LONG_PRESS_SHIP_MS     4200U   /* x */", "LONG_PRESS_SHIP_MS") == 4200
    with pytest.raises(hf.SourceError):
        hf.mode_map("nothing here")


@needs_sources
def test_the_shipped_help_page_matches_this_checkout():
    facts = hf.read_facts(ROOT, None)
    assert hf.check((ROOT / hf.HELP).read_text(), facts) == []


def test_bundled_stm_tag(tmp_path):
    (tmp_path / "main/stm32_fw").mkdir(parents=True)
    (tmp_path / hf.STM_BUNDLE).write_bytes(bytes(0x100) + b"CPFW" + bytes([1, 16, 0x12, 0x14]))
    assert hf.bundled_stm_tag(tmp_path) == "v1.16"
    (tmp_path / hf.STM_BUNDLE).write_bytes(bytes(0x200))
    with pytest.raises(hf.SourceError):
        hf.bundled_stm_tag(tmp_path)
