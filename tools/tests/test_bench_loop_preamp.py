import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402


def test_gain_read_from_reply():
    assert bench.loop_preamp_db("INGAIN: IN1=24 IN2=0 IN3=0") == 0
    assert bench.loop_preamp_db("INGAIN: IN1=24 IN2=24 IN3=0") == 24
    assert bench.loop_preamp_db("INGAIN: IN1=24 IN2=-12") == -12


def test_no_answer_or_no_such_verb_is_unknown():
    assert bench.loop_preamp_db(None) is None
    assert bench.loop_preamp_db("INGAIN") is None
    assert bench.loop_preamp_db("ERR INGAIN none") is None


def test_audio_scenarios_record_the_usb_bus():
    for name in ("sampler_record", "kit_swap_audio"):
        assert bench.SCENARIO_FLAGS[name] == ["--preset", "sampler"]
