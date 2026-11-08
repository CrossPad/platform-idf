import sys
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_flash  # noqa: E402


class FakePort:
    """Answers OTA_BEGIN with @p lines; a line that is an exception is raised."""

    def __init__(self, lines):
        self.lines = list(lines)
        self.written = []
        self.closed = False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written.append(data)

    def readline(self):
        line = self.lines.pop(0) if self.lines else b""
        if isinstance(line, Exception):
            raise line
        return line

    def close(self):
        self.closed = True


def test_a_port_lost_after_begin_is_opened_again_once_the_half_open_session_ran_out(monkeypatch):
    slept = []
    monkeypatch.setattr(ota_flash.time, "sleep", lambda s: slept.append(s))
    lost = FakePort([serial.SerialException("device reports readiness to read but returned no data")])
    ready = FakePort([b"OOTA_ERROR timeout\n", b"OTA_READY\n"])   # the half-open one's answer first
    ports = [lost, ready]
    ser = ota_flash.open_and_begin("/dev/ttyACM1", "OTA_BEGIN 10 v\n", serial_factory=lambda: ports.pop(0))
    assert ser is ready
    assert lost.closed
    assert ota_flash.OTA_STALE_S in slept


def test_begin_gives_up_after_its_tries(monkeypatch):
    monkeypatch.setattr(ota_flash.time, "sleep", lambda s: None)
    ports = [FakePort([b"OTA_ERROR busy\n"]) for _ in range(ota_flash.BEGIN_TRIES)]
    assert ota_flash.open_and_begin("/dev/x", "OTA_BEGIN 10 v\n", serial_factory=lambda: ports.pop(0)) is None
    assert not ports


def test_a_board_running_a_kit_of_its_own_refuses_without_a_retry(monkeypatch, capsys):
    slept = []
    monkeypatch.setattr(ota_flash.time, "sleep", lambda s: slept.append(s))
    refused = FakePort([b"OTA_ERROR would_overwrite_main full\n"])
    ports = [refused]
    assert ota_flash.open_and_begin("/dev/x", "OTA_BEGIN 10 v\n", serial_factory=lambda: ports.pop(0)) is None
    assert refused.closed
    assert ota_flash.OTA_STALE_S not in slept      # no second try
    out = capsys.readouterr().out
    assert "'full'" in out and "Quit" in out
