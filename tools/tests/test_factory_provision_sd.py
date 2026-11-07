import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import factory_provision as fp  # noqa: E402

GB = 1000 ** 3

BOOT_4BIT = """\
I (1482) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
I (1488) bsp_sdmmc: SDMMC SLOT_0 pin map: CLK=6 CMD=7 D0=5 D1=4 D2=16 D3=15 width=4
I (1539) bsp_sdmmc: SD Card mounted successfully at /sdcard
"""

BOOT_1BIT = """\
I (1482) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
I (1488) bsp_sdmmc: SDMMC SLOT_0 pin map: CLK=6 CMD=7 D0=5 D1=4 D2=16 D3=15 width=4
W (1633) bsp_sdmmc: SD 4-bit init failed (ESP_ERR_TIMEOUT), retrying 1-bit
I (1633) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
I (1700) bsp_sdmmc: SD Card mounted successfully at /sdcard
"""

BOOT_FAILED = """\
I (1482) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
W (1633) bsp_sdmmc: SD 4-bit init failed (ESP_ERR_TIMEOUT), retrying 1-bit
E (1771) bsp_sdmmc: SD Card mount failed, error: ESP_ERR_TIMEOUT
"""

BOOT_4BIT_SLOW = BOOT_4BIT.replace("max_freq=40000kHz", "max_freq=20000kHz")

# bsp_sdmmc of release/esp-1010-pack: "host prepared" once per attempt, after
# the pin map and with the clock that attempt asks for; at boot a second 4-bit
# attempt at 20 MHz before 1-bit; the mount line names the bus.
PIN_MAP = "SDMMC SLOT_0 pin map: CLK=6 CMD=7 D0=5 D1=4 D2=16 D3=15 width=4"

BOOT_PACK_4BIT = f"""\
I (1482) bsp_sdmmc: {PIN_MAP}
I (1488) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
I (1539) bsp_sdmmc: SD Card mounted successfully at /sdcard (4-bit, 40000 kHz)
"""

BOOT_PACK_RETRY_20MHZ = f"""\
I (1482) bsp_sdmmc: {PIN_MAP}
I (1488) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
W (1633) bsp_sdmmc: SD 4-bit init failed (ESP_ERR_TIMEOUT), retrying 4-bit 20 MHz
I (1683) bsp_sdmmc: {PIN_MAP}
I (1684) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=20000kHz
I (1741) bsp_sdmmc: SD Card mounted successfully at /sdcard (4-bit, 20000 kHz)
"""

BOOT_PACK_1BIT_UP_TO_1BIT = f"""\
I (1482) bsp_sdmmc: {PIN_MAP}
I (1488) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
W (1633) bsp_sdmmc: SD 4-bit init failed (ESP_ERR_TIMEOUT), retrying 4-bit 20 MHz
I (1683) bsp_sdmmc: {PIN_MAP}
I (1684) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=20000kHz
E (1830) sdmmc_sd: sdmmc_init_sd_ssr: sdmmc_send_cmd returned 0x107
W (1831) bsp_sdmmc: SD 4-bit init failed (ESP_ERR_TIMEOUT), retrying 1-bit
I (1831) bsp_sdmmc: SDMMC host prepared: slot=0 max_freq=40000kHz
"""

BOOT_PACK_1BIT = BOOT_PACK_1BIT_UP_TO_1BIT + \
    "I (1902) bsp_sdmmc: SD Card mounted successfully at /sdcard (1-bit, 40000 kHz)\n"

BOOT_PACK_FAILED = BOOT_PACK_1BIT_UP_TO_1BIT + \
    "E (1990) bsp_sdmmc: SD Card mount failed, error: ESP_ERR_TIMEOUT\n"

ROOTS = """\
FS_ROOTS: /assets 1048576 913408
FS_ROOTS: /spiflash - -
FS_ROOTS: /sdcard 31902400512 2147483648
FS_END
"""


def test_sd_bus_4bit():
    bus = fp.sd_bus_from_console(BOOT_4BIT)
    assert bus == {"width": 4, "freq_khz": 40000, "fell_back_1bit": False, "mounted": True}


def test_sd_bus_fallback_to_1bit():
    bus = fp.sd_bus_from_console(BOOT_1BIT)
    assert bus["fell_back_1bit"] is True and bus["mounted"] is True and bus["width"] == 1


def test_sd_bus_mount_failed():
    assert fp.sd_bus_from_console(BOOT_FAILED)["mounted"] is False


def test_sd_bus_no_boot_in_log():
    assert fp.sd_bus_from_console("I (5) other: nothing")["width"] is None


def test_sd_total_bytes():
    assert fp.sd_total_bytes(ROOTS) == 31902400512
    assert fp.sd_total_bytes(ROOTS.replace("/sdcard 31902400512 2147483648", "/sdcard - -")) is None
    assert fp.sd_total_bytes("") is None


def test_check_sd_card_good():
    errs, warns = fp.check_sd_card(fp.sd_bus_from_console(BOOT_4BIT), 32 * GB - 98 * 10**6, kits=5)
    assert errs == [] and warns == []


def test_check_sd_card_1bit_is_a_reject():
    errs, _ = fp.check_sd_card(fp.sd_bus_from_console(BOOT_1BIT), 32 * GB, kits=5)
    assert any("1-bit" in e and "socket" in e for e in errs)


def test_check_sd_card_not_mounted():
    errs, _ = fp.check_sd_card(fp.sd_bus_from_console(BOOT_FAILED), None, kits=0)
    assert any("not mounted" in e for e in errs)


def test_check_sd_card_wrong_size_or_missing_kits():
    bus = fp.sd_bus_from_console(BOOT_4BIT)
    errs, _ = fp.check_sd_card(bus, 16 * GB, kits=5)
    assert any("32 GB" in e for e in errs)
    errs, _ = fp.check_sd_card(bus, 32 * GB, kits=1)
    assert any("kit" in e for e in errs)


def test_check_sd_card_slow_4bit_is_a_warning():
    errs, warns = fp.check_sd_card(fp.sd_bus_from_console(BOOT_4BIT_SLOW), 32 * GB, kits=5)
    assert errs == [] and any("20000" in w for w in warns)


def test_check_sd_card_unknown_boot_is_an_error():
    errs, _ = fp.check_sd_card(fp.sd_bus_from_console(""), 32 * GB, kits=5)
    assert any("boot log" in e for e in errs)


def test_pack_log_4bit_40mhz_passes():
    bus = fp.sd_bus_from_console(BOOT_PACK_4BIT)
    assert bus == {"width": 4, "freq_khz": 40000, "fell_back_1bit": False, "mounted": True}
    assert fp.check_sd_card(bus, 32 * GB, kits=5) == ([], [])


def test_pack_log_20mhz_retry_is_a_warning():
    # The retry line says "retrying 4-bit 20 MHz", not "retrying 1-bit"; the
    # clock is the second attempt's, logged after it was set.
    bus = fp.sd_bus_from_console(BOOT_PACK_RETRY_20MHZ)
    assert bus == {"width": 4, "freq_khz": 20000, "fell_back_1bit": False, "mounted": True}
    errs, warns = fp.check_sd_card(bus, 32 * GB, kits=5)
    assert errs == [] and any("20000" in w for w in warns)


def test_pack_log_1bit_after_both_4bit_attempts_is_a_reject():
    bus = fp.sd_bus_from_console(BOOT_PACK_1BIT)
    assert bus == {"width": 1, "freq_khz": 40000, "fell_back_1bit": True, "mounted": True}
    errs, _ = fp.check_sd_card(bus, 32 * GB, kits=5)
    assert any("1-bit" in e and "socket" in e for e in errs)


def test_pack_log_mount_failed():
    bus = fp.sd_bus_from_console(BOOT_PACK_FAILED)
    assert bus["mounted"] is False
    errs, _ = fp.check_sd_card(bus, None, kits=0)
    assert any("not mounted" in e for e in errs)


def test_kit_count_handles_both_reply_shapes():
    kits = [{"id": 0, "name": "BLACK TESLA"}, {"id": 1, "name": "BASIC"}]
    assert fp.kit_count({"kits": kits, "current": 0}) == 2
    assert fp.kit_count({"ok": True, "result": {"kits": kits}}) == 2
    assert fp.kit_count({"ok": False, "error": {"code": "NO_CDC_IN_AUDIO_MODE"}}) == 0
    assert fp.kit_count(None) == 0
