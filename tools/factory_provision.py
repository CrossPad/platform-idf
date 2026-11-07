#!/usr/bin/env python3
"""Factory provisioning for a single CrossPad ESP32-S3 unit.

Flow:
  1. Flash bootloader + partition table + app (+otadata/assets) over UART via
     esptool, using the flasher_args.json produced by a per-rev build
     directory (default: the connected board's, via crosspad_board). The port
     is the STM bridge VCP (0x0483:5740) of the board found in step 0: its
     DTR/RTS drive the ESP's EN and GPIO0, so esptool's own reset sequence
     parks the ESP in ROM and its hard reset afterwards is a real EN pulse —
     the path tools/bench.py recover proved. --port overrides it; an ESP ROM
     USB-OTG port (0x303A:0x0009) gets --after watchdog_reset, because an RTS
     reset over USB-OTG left the ESP enumerated as a dead ROM device.
     The ESP's own BOOTLOADER_REQUEST is never used: on rev2 it also sends the
     STM into DFU (REG_CMD_DFU), which takes the bridge away.
  2. Post-flash smoke test: `crosspad-hil run smoke --json` against real
     hardware. Exit code 0 = pass, 1 = firmware failed, 2 = bench problem
     (no board, port busy, missing dependency) — the crosspad-hil convention,
     reused verbatim here.
  3. Read `APP_VERSIONS` over CDC via `crosspad-hil app versions --json` —
     never opens the serial port directly, goes through the crosspad-hil CLI
     like every other tool in this repo.
  0. Read the PCB serial (CP1234) the STM reports as its USB iSerial — burned
     into OTP by CrossPad_STM32_r2/tools/otp_provision.py. A board without
     one, or one that differs from --serial, fails before anything is flashed.
  4. Append a PASS/FAIL summary to factory/<serial>.log.

Requires: esptool (pip install esptool) and crosspad-hil
(pip install -r tools/requirements-hil.txt) in this interpreter.

Exit codes (same convention as crosspad-hil): 0 pass, 1 firmware/board
failed, 2 bench/setup problem (missing tool, no device, bad args).
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

REPO_ROOT = Path(__file__).resolve().parent.parent
FACTORY_LOG_DIR = REPO_ROOT / "factory"


def log(msg: str) -> None:
    print(msg, flush=True)


# --------------------------------------------------------------- crosspad-hil

def hil_cmd() -> list[str]:
    """Base argv for invoking the crosspad-hil CLI (console script, or -m fallback)."""
    exe = shutil.which("crosspad-hil")
    if exe:
        return [exe]
    return [sys.executable, "-m", "crosspad_hil.cli"]


def run_hil(args: list[str], dry_run: bool, timeout: float | None = None) -> tuple[int, dict[str, Any] | None, str]:
    """Run a crosspad-hil subcommand. Returns (exit_code, parsed_json_or_None, raw_text)."""
    cmd = hil_cmd() + args
    if dry_run:
        log(f"[dry-run] would run: {' '.join(cmd)}")
        return 0, None, "(dry-run)"
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        return 2, None, "crosspad-hil not found (pip install -r tools/requirements-hil.txt)"
    except subprocess.TimeoutExpired:
        return 2, None, f"crosspad-hil timed out: {' '.join(cmd)}"
    out = proc.stdout.strip()
    data = None
    if "--json" in args and out:
        try:
            data = json.loads(out)
        except json.JSONDecodeError:
            pass
    text = out or proc.stderr.strip()
    return proc.returncode, data, text


def rollback_supported(text: str | None) -> bool:
    """Parses FW_SLOTS' own "FWSLOT: rollback=0|1" trailer row (the same one
    tools/bench.py check reads). False on anything else — no line, no reply,
    older firmware without it — a unit must not ship without confirmed proof
    of app rollback support."""
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("FWSLOT: rollback="):
            return line.endswith("=1")
    return False


# ------------------------------------------------------------------- flashing

def load_flasher_args(build_dir: Path) -> dict[str, Any]:
    path = build_dir / "flasher_args.json"
    if not path.exists():
        raise FileNotFoundError(
            f"{path} not found — build first: idf.py -B {build_dir.name} "
            f"-DSDKCONFIG=sdkconfig.{build_dir.name.replace('build_', '')} build"
        )
    return json.loads(path.read_text())


def build_esptool_cmd(build_dir: Path, flasher_args: dict[str, Any], port: str, baud: int,
                      after: str | None = None) -> list[str]:
    extra = flasher_args.get("extra_esptool_args", {})
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", extra.get("chip", "esp32s3"),
        "-p", port,
        "-b", str(baud),
        "--before", extra.get("before", "default_reset"),
        "--after", after or extra.get("after", "hard_reset"),
        "write_flash",
    ]
    cmd += flasher_args.get("write_flash_args", [])
    for offset, relpath in flasher_args["flash_files"].items():
        cmd += [offset, str(build_dir / relpath)]
    return cmd


ESP_ROM_USB_OTG = (0x303A, 0x0009)


def bridge_port(devices: list[dict]) -> str | None:
    """The STM bridge VCP of the one board on hardware (its DTR/RTS reach the
    ESP's EN and GPIO0)."""
    if len(devices) != 1:
        return None
    console = (devices[0].get("ports") or {}).get("console") or {}
    return console.get("path")


def port_ids(port: str) -> tuple[int | None, int | None]:
    """(VID, PID) of a serial port, (None, None) when unknown."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None, None
    for p in list_ports.comports():
        if p.device == port:
            return p.vid, p.pid
    return None, None


def esptool_after(vid: int | None, pid: int | None, default: str) -> str:
    """The ESP ROM's USB-OTG port cannot be left with an RTS reset: esptool's
    hard_reset there is a software restart that kept the ROM's USB connection
    alive with nothing behind it. The RTC watchdog resets the chip instead."""
    return "watchdog_reset" if (vid, pid) == ESP_ROM_USB_OTG else default


def wait_for_app(serial: str, timeout_s: float = 40.0) -> dict[str, Any] | None:
    """After the flash: the board again, with the ESP out of ROM (any USB mode
    but bootloader/unknown), matched by the STM's PCB serial."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        _, data, _ = run_hil(["devices", "--json"], False, timeout=20)
        for d in (data or {}).get("devices") or []:
            console = (d.get("ports") or {}).get("console") or {}
            if (console.get("serial") or "").upper() == serial and \
                    d.get("usb_mode") not in (None, "bootloader", "unknown"):
                return d
        time.sleep(2.0)
    return None


def flash_firmware(build_dir: Path, port: str, baud: int, dry_run: bool) -> tuple[int, str]:
    try:
        flasher_args = load_flasher_args(build_dir)
    except FileNotFoundError as e:
        return 2, str(e)
    default_after = flasher_args.get("extra_esptool_args", {}).get("after", "hard_reset")
    cmd = build_esptool_cmd(build_dir, flasher_args, port, baud,
                            esptool_after(*port_ids(port), default_after))
    if dry_run:
        log(f"[dry-run] would run: {' '.join(cmd)}")
        return 0, "(dry-run)"
    proc = subprocess.run(cmd)
    return (0, "flash OK") if proc.returncode == 0 else (2, f"esptool exit {proc.returncode}")


# ---------------------------------------------------------------------- main

_PCB_SERIAL = re.compile(r"^CP\d{4}$")


def board_serial(devices: list[dict], device_id: str | None) -> tuple[str | None, str | None]:
    """The PCB serial the STM reports as its USB iSerial (burned by
    CrossPad_STM32_r2/tools/otp_provision.py). (serial, error)."""
    if not devices:
        return None, "no CrossPad found"
    # One board on hardware: the flash port is taken from the only board
    # crosspad-hil reports, so --device alone cannot keep it on this board.
    if len(devices) > 1:
        return None, "more than one CrossPad connected — provision one board at a time"
    if device_id and devices[0].get("id") != device_id:
        return None, f"no CrossPad {device_id} (connected: {devices[0].get('id')})"
    console = (devices[0].get("ports") or {}).get("console") or {}
    serial = (console.get("serial") or "").upper()
    if not _PCB_SERIAL.match(serial):
        return None, (f"board has no PCB serial (STM reports {serial or 'nothing'}) — "
                      "run CrossPad_STM32_r2/tools/otp_provision.py first")
    return serial, None


def check_serial(board: str | None, expected: str | None) -> str | None:
    if expected and board != expected.strip().upper():
        return f"PCB label says {expected.strip().upper()}, board reports {board}"
    return None


# ------------------------------------------------------------------- SD card
# Customer cards: 32 GB, FAT32 (the firmware's FATFS has no exFAT, so a mount
# is FAT), kits on them. The socket must carry all four data lines: a unit
# whose card only comes up in 1-bit mode is a reject / socket repair, because
# 1-bit cannot feed the sample streamer and bounce in a full live setup.
SD_MIN_BYTES = 29 * 1000 ** 3
SD_MAX_BYTES = 33 * 1000 ** 3
SD_MIN_KITS = 2
SD_FULL_FREQ_KHZ = 40000


def sd_bus_from_console(text: str) -> dict[str, Any]:
    """The SD bus the firmware came up with, from its boot log (bsp_sdmmc)."""
    width = None
    freq = None
    fell_back = False
    mounted = False
    for line in text.splitlines():
        if "bsp_sdmmc" not in line:
            continue
        m = re.search(r"max_freq=(\d+)kHz", line)
        if m:
            freq = int(m.group(1))
        m = re.search(r"width=(\d)", line)
        if m:
            width = int(m.group(1))
        if "retrying 1-bit" in line:
            fell_back = True
            width = 1
        if "SD Card mounted successfully" in line:
            mounted = True
        if "SD Card mount failed" in line:
            mounted = False
    return {"width": width, "freq_khz": freq, "fell_back_1bit": fell_back, "mounted": mounted}


def sd_total_bytes(fs_roots_text: str) -> int | None:
    """Total bytes of /sdcard from an FS_ROOTS reply; None when not mounted."""
    for line in fs_roots_text.splitlines():
        m = re.match(r"FS_ROOTS:\s+/sdcard\s+(\d+)\s+\d+", line.strip())
        if m:
            return int(m.group(1))
    return None


def check_sd_card(bus: dict[str, Any], total_bytes: int | None, kits: int,
                  min_kits: int = SD_MIN_KITS) -> tuple[list[str], list[str]]:
    """(errors, warnings) for the customer card. Any error fails the unit."""
    errors: list[str] = []
    warnings: list[str] = []
    if bus.get("width") is None:
        errors.append("SD: no bsp_sdmmc lines in the boot log — cannot tell the bus width")
    elif not bus.get("mounted"):
        errors.append("SD: card not mounted at boot — card missing or socket fault")
    elif bus.get("fell_back_1bit") or bus.get("width") != 4:
        errors.append("SD: card came up in 1-bit mode — reject: check/repair the TF socket "
                      "(DAT lines, contact pressure), then provision again")
    elif bus.get("freq_khz") and bus["freq_khz"] < SD_FULL_FREQ_KHZ:
        warnings.append(f"SD: 4-bit but at {bus['freq_khz']} kHz, not {SD_FULL_FREQ_KHZ} kHz")
    if total_bytes is None:
        if bus.get("mounted"):
            errors.append("SD: FS_ROOTS reports /sdcard not mounted")
    elif not SD_MIN_BYTES <= total_bytes <= SD_MAX_BYTES:
        errors.append(f"SD: {total_bytes / 1000 ** 3:.1f} GB card, expected a 32 GB FAT32 card")
    if kits < min_kits:
        errors.append(f"SD: {kits} kit(s) on the card, expected at least {min_kits}")
    return errors, warnings


def kit_count(data: dict[str, Any] | None) -> int:
    """Kits in a `crosspad-hil kit list --json` reply (bare or daemon-wrapped)."""
    data = data or {}
    if "result" in data and isinstance(data["result"], dict):
        data = data["result"]
    return len(data.get("kits") or [])


def check_sd_on_board(smoke_data: dict[str, Any] | None,
                      device_args: list[str]) -> tuple[list[str], list[str], list[str]]:
    """Gather the three SD facts from the board and judge them. (errors, warnings, log lines)."""
    console = ""
    for art in (smoke_data or {}).get("artifacts") or []:
        if art.get("role") == "console":
            try:
                console = Path(art["path"]).read_text(errors="replace")
            except OSError:
                pass
    bus = sd_bus_from_console(console)
    _, _, roots = run_hil(["cdc", "FS_ROOTS"] + device_args, False, timeout=15)
    total = sd_total_bytes(roots)
    _, kit_data, _ = run_hil(["kit", "list", "--json"] + device_args, False, timeout=60)
    kits = kit_count(kit_data)
    errors, warnings = check_sd_card(bus, total, kits)
    lines = [f"sd_bus: width={bus['width']} freq_khz={bus['freq_khz']} "
             f"fell_back_1bit={bus['fell_back_1bit']} mounted={bus['mounted']}",
             f"sd_card: total_bytes={total} kits={kits}",
             f"sd_check: {'OK' if not errors else 'FAIL ' + '; '.join(errors)}"
             + (f" (warnings: {'; '.join(warnings)})" if warnings else "")]
    return errors, warnings, lines


def write_log(serial: str, lines: list[str]) -> Path:
    FACTORY_LOG_DIR.mkdir(parents=True, exist_ok=True)
    path = FACTORY_LOG_DIR / f"{serial}.log"
    with open(path, "a") as f:
        f.write("\n".join(lines) + "\n---\n")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=None,
                         help="per-rev build directory to flash from (default: the connected board's)")
    parser.add_argument("--port", default=None,
                         help="UART port for esptool (auto: the board's STM bridge VCP, 0x0483:5740)")
    parser.add_argument("--baud", type=int, default=460800,
                         help="esptool flash baud rate (460800: what bench.py recover uses on the bridge)")
    parser.add_argument("--device", default=None,
                         help="crosspad-hil device id (dev_xxxx) to target for smoke/APP_VERSIONS, if ambiguous")
    parser.add_argument("--serial", default=None,
                         help="PCB label serial (CP1234); must match what the board reports")
    parser.add_argument("--dry-run", action="store_true", help="print the plan, run nothing")
    args = parser.parse_args()

    if args.build_dir is None:
        import crosspad_board
        d = crosspad_board.resolve(REPO_ROOT)
        if d.rev is None:
            log(crosspad_board.describe(d))
            return 2
        args.build_dir = d.build_dir

    build_dir = (REPO_ROOT / args.build_dir).resolve()
    log(f"CrossPad factory provisioning — build dir: {build_dir}")

    # 0. the PCB serial the STM carries in OTP names the unit and its log
    serial = "DRYRUN"
    devices: list[dict] = []
    if not args.dry_run:
        _, data, _ = run_hil(["devices", "--json"], False)
        devices = (data or {}).get("devices") or []
        serial, err = board_serial(devices, args.device)
        mismatch = check_serial(serial, args.serial)
        if err or mismatch:
            log(err or mismatch)
            # no board / two boards is the bench; no serial or the wrong one is the unit
            return 2 if serial is None and ("no CrossPad" in err or "one board" in err) else 1
        log(f"Board serial: {serial}")

    # 1. flash over the STM bridge
    port = args.port or bridge_port(devices)
    if not args.dry_run and not port:
        log("The board shows no STM bridge VCP (0x0483:5740). Check the STM is running "
            "its app (not DFU), or pass --port.")
        return 2
    flash_rc, flash_msg = flash_firmware(build_dir, port or "(dry-run)", args.baud, args.dry_run)
    log(f"Flash: {flash_msg}")

    lines = [
        f"CrossPad factory provisioning log",
        f"timestamp: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"build_dir: {build_dir}",
        f"port: {port}",
        f"flash: rc={flash_rc} {flash_msg}",
    ]

    if flash_rc != 0:
        lines.append("verdict: FAIL (flash step)")
        log_path = write_log(serial, lines)
        log(f"FAIL — see {log_path}")
        return 2

    # 1a. The hard reset boots the new image; every step below needs the app.
    if not args.dry_run:
        booted = wait_for_app(serial)
        lines.append(f"boot: {'usb_mode=' + str(booted.get('usb_mode')) if booted else 'ESP not back from ROM'}")
        if not booted:
            lines.append("verdict: FAIL (bench: ESP did not boot the app after the flash)")
            log_path = write_log(serial, lines)
            log("ESP did not come back after the flash — `tools/bench.py hil console --reset`, "
                f"then run again. Log: {log_path}")
            return 2

    # 2. smoke
    device_args = ["-d", args.device] if args.device else []

    # 2a. A freshly flashed board has no settings, and the default USB profile
    # is MIDI+UAC2, which carries no CDC endpoint — smoke and APP_VERSIONS both
    # speak CDC. The switch goes over SysEx on USB MIDI and persists, so step 3b
    # has to put the shipping profile back.
    mode_rc, _, mode_text = run_hil(["usb-mode", "set", "default"] + device_args,
                                    args.dry_run, timeout=180)
    lines.append(f"usb_mode_default: rc={mode_rc} {mode_text}")
    log(f"USB profile -> default (CDC): rc={mode_rc}")
    smoke_rc, smoke_data, smoke_text = run_hil(["run", "smoke", "--json"] + device_args, args.dry_run, timeout=60)
    lines.append(f"smoke: rc={smoke_rc} {smoke_text}")
    log(f"Smoke: rc={smoke_rc}")

    # 2b. The customer SD card: 4-bit bus at boot (from smoke's own boot log),
    # a mounted 32 GB FAT32 card, kits on it. Any error fails the unit.
    sd_errors: list[str] = []
    if smoke_rc == 0 and not args.dry_run:
        sd_errors, sd_warnings, sd_lines = check_sd_on_board(smoke_data, device_args)
        lines.extend(sd_lines)
        for w in sd_warnings:
            log(f"warning: {w}")
        log("SD card: " + ("OK" if not sd_errors else "; ".join(sd_errors)))

    # 3. APP_VERSIONS over CDC, via crosspad-hil (best-effort, non-fatal)
    if smoke_rc == 0 or args.dry_run:
        ver_rc, ver_data, ver_text = run_hil(["app", "versions", "--json"] + device_args, args.dry_run, timeout=15)
        lines.append(f"app_versions: rc={ver_rc} {ver_text}")
        log(f"APP_VERSIONS: rc={ver_rc}")
    else:
        lines.append("app_versions: skipped (smoke did not pass)")

    # 3a. Bootloader rollback gate — a unit must not leave the factory without
    # app rollback protection (fw_slots.h): the bootloader that returns to the
    # previous OTA slot on its own when a firmware never confirms itself.
    if smoke_rc == 0 or args.dry_run:
        rb_rc, _, rb_text = run_hil(["cdc", "FW_SLOTS", "--expect", "FWSLOT: rollback"] + device_args,
                                    args.dry_run, timeout=15)
        rb_ok = True if args.dry_run else rollback_supported(rb_text)
        lines.append(f"rollback: rc={rb_rc} ok={rb_ok} {rb_text}")
        log(f"Bootloader rollback: {'OK' if rb_ok else 'MISSING'}")
    else:
        rb_ok = False
        lines.append("rollback: skipped (smoke did not pass)")

    # 3b. Ship in the default profile: USB Audio. Done after every CDC step, and
    # non-fatal — a board that will not switch back is still a provisioned board,
    # it just leaves the bench on the wrong profile, and the log says so.
    back_rc, _, back_text = run_hil(["usb-mode", "set", "audio"] + device_args,
                                    args.dry_run, timeout=180)
    lines.append(f"usb_mode_ship: rc={back_rc} {back_text}")
    log(f"USB profile -> audio (shipping default): rc={back_rc}")

    # 4. serial + verdict + log
    if args.dry_run:
        verdict = "DRY-RUN (no hardware touched)"
        exit_code = 0
    elif smoke_rc == 0 and sd_errors:
        verdict = "FAIL (SD card: " + "; ".join(sd_errors) + ")"
        exit_code = 1
    elif smoke_rc == 0 and not rb_ok:
        verdict = "FAIL (no bootloader rollback)"
        exit_code = 1
    elif smoke_rc == 0:
        verdict = "PASS"
        exit_code = 0
    elif smoke_rc == 1:
        verdict = "FAIL (firmware)"
        exit_code = 1
    else:
        verdict = "FAIL (bench)"
        exit_code = 2
    lines.append(f"verdict: {verdict}")

    log_path = write_log(serial, lines)
    log(f"{verdict} — log: {log_path}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
