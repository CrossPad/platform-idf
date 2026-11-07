import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import factory_provision as fp  # noqa: E402


def dev(console_path):
    return {"id": "dev_1", "ports": {"console": None if console_path is None
                                     else {"path": console_path, "serial": "CP1234"}}}


def test_flash_port_is_the_stm_bridge():
    assert fp.bridge_port([dev("/dev/ttyACM0")]) == "/dev/ttyACM0"


def test_no_bridge_no_port():
    # STM in DFU (or not enumerated): no console VCP, nothing to flash through
    assert fp.bridge_port([dev(None)]) is None
    assert fp.bridge_port([]) is None
    assert fp.bridge_port([dev("/dev/ttyACM0"), dev("/dev/ttyACM2")]) is None


def test_rom_usb_otg_port_resets_with_the_watchdog():
    assert fp.esptool_after(0x303A, 0x0009, "hard_reset") == "watchdog_reset"


def test_other_ports_keep_the_build_dirs_after():
    assert fp.esptool_after(0x0483, 0x5740, "hard_reset") == "hard_reset"
    assert fp.esptool_after(None, None, "hard_reset") == "hard_reset"


def test_esptool_cmd_takes_the_after_override(tmp_path):
    args = {"extra_esptool_args": {"chip": "esp32s3", "before": "default_reset", "after": "hard_reset"},
            "write_flash_args": [], "flash_files": {"0x0": "bootloader.bin"}}
    cmd = fp.build_esptool_cmd(tmp_path, args, "/dev/ttyACM0", 460800, "watchdog_reset")
    assert cmd[cmd.index("--after") + 1] == "watchdog_reset"
    cmd = fp.build_esptool_cmd(tmp_path, args, "/dev/ttyACM0", 460800)
    assert cmd[cmd.index("--after") + 1] == "hard_reset"
