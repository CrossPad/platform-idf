import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402


def test_no_solo():
    assert bench.soloed_channels("MIXSOLO: ch=-") == []


def test_soloed_channels():
    assert bench.soloed_channels("MIXSOLO: ch=0") == [0]
    assert bench.soloed_channels("noise\nMIXSOLO: ch=0,6\n") == [0, 6]


def test_firmware_without_the_verb_is_unknown():
    # An older firmware echoes the line; a board that did not answer gives None.
    assert bench.soloed_channels("MIX_SOLO") is None
    assert bench.soloed_channels(None) is None


def test_channel_names_from_mix_lvl():
    text = ("MIXLVL: ch=0 name=Sampler pk=0.0000/0.0000 vol=1.00 trim=1.00 mute=0 route=111\n"
            "MIXLVL: ch=2 name=USB pk=0.0000/0.0000 vol=1.00 trim=1.00 mute=0 route=110\n"
            "MIXLVL: out=0 pk=0.0000/0.0000 vol=1.00 mute=0 lim=1.000 ceil=0.980\n")
    assert bench.channel_names(text) == {0: "Sampler", 2: "USB"}
