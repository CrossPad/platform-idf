#!/usr/bin/env python3
"""OTA firmware update over USB CDC for CrossPad ESP32-S3

Supports two modes:
  - Full OTA (default): sends complete firmware binary
  - Delta OTA (--delta): generates and sends a compressed delta patch

If the device is in Audio USB mode (MIDI + UAC2, no CDC), the script
auto-detects it via MIDI, sends a SYSEX command to switch to Default
mode (CDC + MIDI), waits for re-enumeration, and then proceeds with OTA.

The image must match the revision of the board being flashed, as crosspad-hil
reports it for the device owning the CDC port. A board of unknown revision is
flashed only when --board names it; --force overrides every check.
"""

from typing import NamedTuple

import serial
import serial.tools.list_ports
import sys
import time
import os
import json
import argparse
import contextlib
import subprocess
import tempfile

import crosspad_board
import fw_image_desc

ESPRESSIF_VID = 0x303A
CROSSPAD_PID = 0x3456
CHUNK_SIZE = 4096
EXIT_OK, EXIT_FAIL, EXIT_BENCH = 0, 1, 2

# SYSEX: switch USB mode to DEFAULT (CDC + MIDI)
# Format: F0 7D 1B 01 F7  (manufacturer=0x7D, cmd=USB_MODE, 0x01=DEFAULT)
# Mode values: 0x01=DEFAULT(CDC+MIDI), 0x02=AUDIO(MIDI+UAC2)  — non-zero to avoid MIDI 0x00 issues
SYSEX_USB_MODE_DEFAULT = [0x7D, 0x1B, 0x01]


def find_crosspad_port():
    """Auto-detect CrossPad USB CDC port by VID:PID"""
    for port in serial.tools.list_ports.comports():
        if port.vid == ESPRESSIF_VID and port.pid == CROSSPAD_PID:
            return port.device
    return None


def find_crosspad_midi():
    """Find CrossPad MIDI output port. Returns (port_index, port_name) or (None, None)."""
    try:
        import rtmidi
    except ImportError:
        print("Installing python-rtmidi...")
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'python-rtmidi'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        import rtmidi

    midiout = rtmidi.MidiOut()
    ports = midiout.get_ports()
    for i, name in enumerate(ports):
        if 'crosspad' in name.lower():
            return i, name
    return None, None


def send_midi_switch_to_cdc():
    """Send SYSEX via MIDI to tell CrossPad to switch to CDC+MIDI mode.
    Returns True if message was sent successfully."""
    try:
        import rtmidi
    except ImportError:
        print("Installing python-rtmidi...")
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'python-rtmidi'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        import rtmidi

    port_idx, port_name = find_crosspad_midi()
    if port_idx is None:
        return False

    print(f"Found CrossPad MIDI: {port_name}")
    print("Sending USB mode switch SYSEX (-> CDC + MIDI)...")

    midiout = rtmidi.MidiOut()
    midiout.open_port(port_idx)
    full_sysex = [0xF0] + SYSEX_USB_MODE_DEFAULT + [0xF7]
    midiout.send_message(full_sysex)
    time.sleep(0.1)
    midiout.close_port()
    del midiout
    return True


def wait_for_cdc(timeout=10):
    """Poll for CrossPad CDC port to appear after USB re-enumeration."""
    print(f"Waiting for CDC port (up to {timeout}s)...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_crosspad_port()
        if port:
            print(f"CrossPad CDC available on {port}")
            return port
        time.sleep(0.5)
    return None


def generate_delta_patch(base_fw_path, new_fw_path, patch_path):
    """Generate a delta patch using detools with heatshrink compression.
    Returns the patch file size in bytes."""
    try:
        import detools
    except ImportError:
        print("Installing detools...")
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'detools'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        import detools

    print(f"Generating delta patch...")
    print(f"  Base: {base_fw_path} ({os.path.getsize(base_fw_path)} bytes)")
    print(f"  New:  {new_fw_path} ({os.path.getsize(new_fw_path)} bytes)")

    with open(base_fw_path, 'rb') as base_f, \
         open(new_fw_path, 'rb') as new_f, \
         open(patch_path, 'wb') as patch_f:
        detools.create_patch(base_f, new_f, patch_f,
                             compression='heatshrink')

    patch_size = os.path.getsize(patch_path)
    base_size = os.path.getsize(base_fw_path)
    new_size = os.path.getsize(new_fw_path)
    ratio = patch_size / new_size * 100
    print(f"  Patch: {patch_path} ({patch_size} bytes, {ratio:.1f}% of full firmware)")
    return patch_size


def send_data_to_device(ser, data, data_size, label="firmware"):
    """Send binary data to device in chunks with progress reporting.
    Used by both full OTA and delta OTA modes."""
    sent = 0
    start_time = time.time()
    last_decile = -1

    while sent < data_size:
        chunk_end = min(sent + CHUNK_SIZE, data_size)
        chunk = data[sent:chunk_end]
        ser.write(chunk)
        sent = chunk_end

        # Read all available responses (ESP may batch them)
        while True:
            response = ser.readline().decode(errors='ignore').strip()

            if not response:
                # Timeout - no response
                print(f"\nTimeout waiting for response (sent {sent}/{data_size})")
                ser.close()
                return False
            elif response.startswith("OK "):
                progress = response[3:]
                received = int(progress.split('/')[0])
                elapsed = time.time() - start_time
                speed = received / elapsed / 1024 if elapsed > 0 else 0
                percent = received * 100 // data_size
                line = f"Progress: {percent}% ({received}/{data_size}) {speed:.0f} KB/s"
                if sys.stdout.isatty():
                    print(f"\r{line}", end='', flush=True)
                elif percent // 10 != last_decile:
                    # A pipe keeps every \r-overwritten update: 500 of them per
                    # image. One line per 10 % is what a log wants.
                    last_decile = percent // 10
                    print(line, flush=True)
                break  # Send next chunk
            elif response == "OTA_OK":
                elapsed = time.time() - start_time
                speed = data_size / elapsed / 1024
                print(f"\n\nOTA complete! ({elapsed:.1f}s, {speed:.0f} KB/s)")
                print("Device restarting...")
                ser.close()
                return True
            elif response.startswith("OTA_ERROR"):
                print(f"\nError: {response}")
                ser.close()
                return False
            else:
                # Skip unexpected data (stale echo, log output, etc.)
                continue

    # All data sent - wait for final OTA_OK (delta may take extra time to finalize)
    ser.timeout = 60  # extend timeout for delta finalization
    for _ in range(30):
        response = ser.readline().decode(errors='ignore').strip()
        if response == "OTA_OK":
            elapsed = time.time() - start_time
            speed = data_size / elapsed / 1024
            print(f"\n\nOTA complete! ({elapsed:.1f}s, {speed:.0f} KB/s)")
            print("Device restarting...")
            ser.close()
            return True
        elif response.startswith("OK "):
            continue  # Progress update, keep waiting
        elif response.startswith("OTA_ERROR"):
            print(f"\nError: {response}")
            ser.close()
            return False
        elif not response:
            break

    print(f"\nFinal response: '{response}'")
    ser.close()
    return False


# A session left half-open (the host lost the port after OTA_BEGIN) keeps the
# device in OTA receive mode until its CDC loop has been idle this long
# (main.cpp aborts it on the idle tick, ~10 s measured): a retry sent sooner is
# taken as firmware bytes, and its answer is that session's "OTA_ERROR timeout".
OTA_STALE_S = 12
BEGIN_TRIES = 2
# ota_begin()'s answer when the board will not take this image at all: trying
# again cannot help, so open_and_begin() stops at once.
REFUSED = "refused"


def read_begin_reply(ser):
    """The first line that answers an OTA begin; a line left over from before (a
    progress "OK", a half-open session's error) is not it."""
    while True:
        response = ser.readline().decode(errors='ignore').strip()
        if not response or response in ("OTA_READY", "OTA_WAIT") or response.startswith("OTA_ERROR"):
            return response
        print(f"(skipping {response!r})")


def open_and_begin(port, command, serial_factory=None):
    """The port opened and the OTA begun, tried again once after a half-open
    session has run out (OTA_STALE_S): the serial port, or None."""
    factory = serial_factory or (lambda: serial.Serial(port, 115200, timeout=30))
    for attempt in range(BEGIN_TRIES):
        ser = None
        try:
            ser = factory()
            time.sleep(0.5)
            ser.reset_input_buffer()
            began = ota_begin(ser, command)
            if began is True:
                return ser
            if began == REFUSED:
                return None
        except serial.SerialException as e:
            print(f"Error: the port went away during OTA begin: {e}")
            if ser is not None:
                ser.close()
        if attempt + 1 < BEGIN_TRIES:
            print(f"Waiting {OTA_STALE_S} s for the device to drop the half-open session...")
            time.sleep(OTA_STALE_S)
    return None


def explain_refusal(response):
    """What to do about an OTA_ERROR the board answers to every attempt, or
    None for one worth another try."""
    if response.startswith("OTA_ERROR would_overwrite_main"):
        kit = response.split()[2] if len(response.split()) > 2 else "main"
        return (f"The board is running a firmware kit of its own (a game, a single-purpose\n"
                f"firmware) from its second slot. This update would overwrite the '{kit}'\n"
                f"firmware in the other slot -- the one Quit and holding pad 16 at start-up\n"
                f"go back to. Go back to it first (Quit in the app, or FW_SWITCH over the\n"
                f"console), then run this again.")
    return None


def ota_begin(ser, command):
    """Send OTA begin command and wait for device ready.
    Returns True if device is ready, REFUSED when the board will not take this
    image (no retry helps), False on any other error."""
    print(f"Sending: {command.strip()}")
    ser.write(command.encode())

    # Wait for OTA_READY or OTA_WAIT (esp_ota_begin erases flash, can take seconds)
    response = read_begin_reply(ser)

    if response == "OTA_WAIT":
        print("Waiting for user confirmation on device...")
        ser.timeout = 90  # extend timeout for user interaction
        response = ser.readline().decode(errors='ignore').strip()
        ser.timeout = 30  # restore normal timeout

    if response != "OTA_READY":
        why = explain_refusal(response)
        if why:
            print(f"Refused: {response}\n\n{why}")
            ser.close()
            return REFUSED
        print(f"Error: expected OTA_READY, got: '{response}'")
        ser.close()
        return False

    return True


def get_firmware_version(firmware_path):
    """Extract ESP-IDF app version from the binary (0x30 offset length 32)"""
    try:
        with open(firmware_path, 'rb') as f:
            f.seek(48)
            version = f.read(32).split(b'\0')[0].decode('utf-8', 'ignore')
            return version
    except Exception:
        return ""


def ota_flash(port, firmware_path):
    """Full firmware OTA flash."""
    with open(firmware_path, 'rb') as f:
        firmware = f.read()

    fw_size = len(firmware)
    print(f"Firmware: {firmware_path} ({fw_size} bytes, {fw_size / 1024 / 1024:.2f} MB)")

    print(f"Connecting to {port}...")
    version = get_firmware_version(firmware_path)
    ser = open_and_begin(port, f"OTA_BEGIN {fw_size} {version}\n")
    if ser is None:
        return False

    print("Device ready. Sending firmware...")
    return send_data_to_device(ser, firmware, fw_size)


def ota_flash_delta(port, patch_data, firmware_path):
    """Delta OTA flash using a pre-generated patch."""
    patch_size = len(patch_data)
    print(f"Patch size: {patch_size} bytes ({patch_size / 1024:.1f} KB)")

    print(f"Connecting to {port}...")
    version = get_firmware_version(firmware_path) if firmware_path else ""
    ser = open_and_begin(port, f"OTA_DELTA {patch_size} {version}\n")
    if ser is None:
        return False

    print("Device ready. Sending delta patch...")
    return send_data_to_device(ser, patch_data, patch_size, label="patch")


# The image just written boots ESP_OTA_IMG_PENDING_VERIFY and confirms itself
# ~10 s in (kConfirmDelayMs, main/main.cpp); a reset before that makes the
# bootloader abort it and boot the other slot. Long enough for the board to
# re-enumerate and answer, not so long that the trial is over before we look.
TRIAL_REPORT_DELAY_S = 6


def running_slot_state(text):
    """The `state=` of the FW_SLOTS row marked `running=1`, or None when no
    such row came back (firmware without the verb, or no reply)."""
    for line in (text or "").splitlines():
        line = line.strip()
        if not line.startswith("FWSLOT:") or "running=1" not in line:
            continue
        for field in line.split():
            if field.startswith("state="):
                return field[len("state="):]
    return None


def report_trial_state(port):
    """One line on what the freshly booted image's slot says about itself.
    Best-effort and never fatal: an older firmware has no FW_SLOTS."""
    time.sleep(TRIAL_REPORT_DELAY_S)
    port = find_crosspad_port() or port
    rows = []
    try:
        ser = serial.Serial(port, 115200, timeout=3)
    except Exception:
        return
    try:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"FW_SLOTS\n")
        deadline = time.time() + 5
        while time.time() < deadline:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
            if line.startswith("FWSLOT:"):
                rows.append(line)
            if line.startswith("FWSLOT: rollback="):
                break
    except Exception:
        return
    finally:
        ser.close()

    state = running_slot_state("\n".join(rows))
    if state is None:
        return
    print(f"Running slot: {state}")
    if state == "trial":
        print("  do not reset the board for ~10 s, the image is on trial —"
              " a reset before it confirms rolls it back to the other slot")


def ensure_port(args_port):
    """Resolve the serial port (auto-detect or use provided)."""
    port = args_port
    if not port:
        port = find_crosspad_port()

        # CDC not found — try MIDI fallback to switch USB mode
        if not port:
            print("CDC port not found. Checking MIDI...")
            if send_midi_switch_to_cdc():
                port = wait_for_cdc(timeout=10)

        if not port:
            print("Error: CrossPad not found (neither CDC nor MIDI). Connect device or specify --port")
            sys.exit(1)

        print(f"Auto-detected CrossPad on {port}")
    return port


def board_build_artifact(name):
    """<build dir of the resolved board>/<name>, or None -- says which and why."""
    d = crosspad_board.resolve()
    if d.rev is None:
        print(crosspad_board.describe(d))
        return None
    path = os.path.join(d.build_dir, name)
    print(f"Board {crosspad_board.describe(d)} -> {path}")
    return path if os.path.exists(path) else None


def image_rev(path):
    """The revision an image was built for: its own CPFI block, else its build dir's sdkconfig.json."""
    try:
        desc = fw_image_desc.read(path)
    except OSError:
        desc = None
    if desc and desc["board_rev"] is not None:
        return f"v{desc['board_rev']}"
    cfg = os.path.join(os.path.dirname(os.path.abspath(path)), "config", "sdkconfig.json")
    try:
        with open(cfg) as f:
            return json.load(f).get("BSP_BOARD_REV_STR") or None
    except (OSError, ValueError):
        return None


class Choice(NamedTuple):
    path: str | None
    rev: str | None
    code: int
    message: str


def device_on_port(port, devices):
    """The crosspad-hil device whose ESP CDC tty is `port`, or None."""
    want = os.path.realpath(port)
    for dev in devices:
        cdc = (dev.get("ports") or {}).get("cdc") or {}
        if cdc.get("path") and os.path.realpath(cdc["path"]) == want:
            return dev
    return None


def device_cdc_port(device_id, devices):
    for dev in devices:
        if dev.get("id") == device_id:
            return ((dev.get("ports") or {}).get("cdc") or {}).get("path")
    return None


def choose_image(port, devices, explicit=None, board=None, force=False, device_id=None,
                 image_rev_fn=image_rev, memory_fn=None, project_dir=crosspad_board.REPO):
    """Which image goes to the board on `port`.

    The board's revision is the one crosspad-hil reports for the device owning
    the port (hardware, else the flashed image). The image's revision is
    `board`, else the explicit image's build config, else the board's. A known
    mismatch is refused, and so is a board whose revision nobody can tell --
    remembered choices describe no particular board -- unless the user names
    the revision with `board`. `force` overrides both.
    """
    owner = device_on_port(port, devices)
    if device_id and (owner or {}).get("id") != device_id:
        return Choice(None, None, EXIT_BENCH,
                      f"Error: {port} is not the CDC port of {device_id} "
                      f"(crosspad-hil devices lists each board's ports)")
    board_rev = None
    if owner:
        board_rev = crosspad_board.resolve(project_dir, device=owner.get("id"), devices=devices).rev
    built_for = image_rev_fn(explicit) if explicit else None
    if board and built_for and built_for != board and not force:
        return Choice(None, None, EXIT_BENCH,
                      f"Error: {explicit} is a {built_for} image, not {board}. --force if you mean it.")
    rev = board or built_for or board_rev

    if board_rev is None and not board and not force:
        if not devices:
            why = "crosspad-hil lists no board (not installed? tools/bench.py check)"
        elif owner is None:
            why = f"no board crosspad-hil lists has {port} as its CDC port"
        else:
            why = (f"{owner.get('id')} advertises no revision -- neither the STM bridge "
                   f"(hardware) nor the ESP (flashed image)")
        return Choice(None, None, EXIT_BENCH,
                      f"Error: the revision of the board on {port} is unknown: {why}. "
                      f"The image cannot be checked against it -- pass --board v1|v2 "
                      f"(the board's revision), or --force.")
    if board_rev and rev and rev != board_rev and not force:
        who = owner.get("id") if owner else port
        return Choice(None, None, EXIT_BENCH,
                      f"Error: {who} on {port} is a {board_rev} board and the image is {rev} "
                      f"-- the wrong pinout leaves it rebooting. --force if you mean it.")

    if rev is None:
        rev = memory_fn() if memory_fn else crosspad_board.load_memory(project_dir)
    if rev is None and not explicit:
        return Choice(None, None, EXIT_BENCH,
                      "Error: no revision to take an image for -- pass --board v1|v2 or a path")
    path = explicit or os.path.join(project_dir, f"build_{rev}", "CrossPad.bin")
    board_text = f"{owner.get('id')} {board_rev}" if owner and board_rev else "board revision unknown"
    note = "  (forced)" if force and (board_rev is None or rev != board_rev) else ""
    return Choice(path, rev, EXIT_OK, f"Board: {board_text} -> image {rev or '?'}: {path}{note}")


def hub_released(purpose: str):
    """The crosspad-hil MIDI hub, when it runs, keeps the CDC port open for 5 s
    after each verb it carries and reads it: its reader took OTA_READY, and the
    flash waited 30 s for a reply it never saw. Its exclusive op makes it close
    the port until the flash is over. Without crosspad-hil importable here (run
    by a python outside its venv), nothing to ask."""
    try:
        from crosspad_hil.hub_link import exclusive
    except ImportError:
        return contextlib.nullcontext()
    return exclusive(purpose)


def main():
    parser = argparse.ArgumentParser(description='OTA firmware update over USB CDC for CrossPad')
    parser.add_argument('firmware', nargs='?', default=None,
                        help="Path to firmware binary (default: build_<rev>/CrossPad.bin of the board being flashed)")
    parser.add_argument('-p', '--port', default=None,
                        help='Serial port (auto-detect if not specified)')
    parser.add_argument('--board', choices=crosspad_board.REVS, default=None,
                        help="revision of the image to take; needed when the board's own is unknown")
    parser.add_argument('--device', default=None,
                        help='crosspad-hil device id to flash, when several boards are connected')
    parser.add_argument('--delta', action='store_true',
                        help='Use delta OTA (requires --base-fw)')
    parser.add_argument('--base-fw', default=None,
                        help='Path to base firmware currently on device (required for --delta)')
    parser.add_argument('--patch', default=None,
                        help='Path to pre-generated delta patch file (skips patch generation)')
    parser.add_argument('--force', action='store_true',
                        help='flash an image built for another revision, or for a board of unknown revision')
    args = parser.parse_args()

    port = args.port
    if args.device and not port:
        port = device_cdc_port(args.device, crosspad_board.list_devices())
    port = ensure_port(port)
    choice = choose_image(port, crosspad_board.list_devices(), args.firmware, args.board,
                          args.force, args.device)
    print(choice.message)
    if choice.path is None:
        sys.exit(choice.code)
    if not args.firmware and not os.path.exists(choice.path):
        build = crosspad_board.Decision(choice.rev, "explicit")
        print(f"Error: {choice.path} is not built -- idf.py {' '.join(build.idf_args())} build")
        sys.exit(EXIT_FAIL)
    args.firmware = choice.path

    if args.delta or args.patch:
        # Delta OTA mode
        if args.patch:
            # Use pre-generated patch
            if not os.path.exists(args.patch):
                print(f"Error: patch file not found: {args.patch}")
                sys.exit(1)
            with open(args.patch, 'rb') as f:
                patch_data = f.read()
        else:
            # Generate patch from base + new firmware
            if not args.base_fw:
                print("Error: --base-fw is required for delta OTA (path to firmware currently on device)")
                sys.exit(1)
            if not os.path.exists(args.base_fw):
                print(f"Error: base firmware not found: {args.base_fw}")
                sys.exit(1)
            if not os.path.exists(args.firmware):
                print(f"Error: new firmware not found: {args.firmware}")
                sys.exit(1)

            # Generate patch to a temp file
            patch_path = tempfile.mktemp(suffix='.patch')
            try:
                generate_delta_patch(args.base_fw, args.firmware, patch_path)
                with open(patch_path, 'rb') as f:
                    patch_data = f.read()
            finally:
                if os.path.exists(patch_path):
                    os.unlink(patch_path)

        with hub_released("ota"):
            success = ota_flash_delta(port, patch_data, args.firmware if not args.patch else None)
    else:
        # Full OTA mode (default)
        if not os.path.exists(args.firmware):
            print(f"Error: firmware file not found: {args.firmware}")
            sys.exit(1)
        with hub_released("ota"):
            success = ota_flash(port, args.firmware)

    if success:
        report_trial_state(port)
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
