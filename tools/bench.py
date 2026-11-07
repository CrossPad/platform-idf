#!/usr/bin/env python3
"""One entry point for driving a CrossPad bench, so an agent does not have to
know this machine's quirks.

Every check and every fix in here exists because the trap is real:

  * ``python`` is not installed (only ``python3``), and half the docs say
    ``python tools/...``.
  * ``crosspad-hil`` is not on PATH; it lives in its repo's virtualenv.
  * ``ota_flash.py``/``fs_transfer.py``/``bench.py`` now pick the build dir of
    the connected (or remembered) board via ``crosspad_board`` — a fixed
    ``build/`` or ``build_v2/`` default used to send a week-old binary that
    boots, passes smoke, and "confirms" a change that never reached the board.
  * MIDI+UAC2 is the default profile, and in it there is no CDC endpoint at
    all, so every verb answers "no reply" from a perfectly healthy board.
  * A board can sit with no kit loaded, and the scenarios that need one then
    fail as "bench problem" (exit 2).
  * A blind UI walk can leave the capture route on a DAC->ADC bus that rails,
    which fails every later audio test.
  * The kit scan after boot holds the kit vector, so KIT_LIST refuses for a
    while — that is not a hang.

Usage:
  tools/bench.py check            what is wrong, and the command that fixes it
  tools/bench.py ready            put the bench into a known-good state
  tools/bench.py flash [--assets] flash the build for the connected board and prove it landed
  tools/bench.py test SCENARIO..  run scenarios with the flags each one needs
                                  (the main one: test beat [--depth quick|medium|full])
  tools/bench.py hil ARGS...      raw crosspad-hil, venv resolved for you

Exit codes follow crosspad-hil: 0 fine, 1 the firmware is at fault, 2 the bench
is.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

EXIT_OK, EXIT_FAIL, EXIT_BENCH = 0, 1, 2

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where crosspad-hil may live, most specific first. PATH wins if it is there.
# The project's own .venv is where the CrossPad installer puts it (Linux/macOS
# and Windows layouts) — without it nothing found the board on a machine set
# up by the installer.
HIL_CANDIDATES = (
    os.path.join(REPO, ".venv", "bin", "crosspad-hil"),
    os.path.join(REPO, ".venv", "Scripts", "crosspad-hil.exe"),
    os.path.expanduser("~/GIT/crosspad-hil/.venv/bin/crosspad-hil"),
    os.path.expanduser("~/.venvs/crosspad-hil/bin/crosspad-hil"),
)

# The capture route a bench should idle in: preset MICS = codec 0 LINE2.
# (audio_route_control.cpp k_presets[0]; AUDIO_STATUS reports adc 2 = LINE2.)
ROUTE_HOME_MIC_SRC = 0
ROUTE_HOME_ADC0 = 2

# Kit loads on a 1-bit SD card take much longer than the scenario default, and
# a pitched kit carries a ~5.1 MB bank on top of that. The gate gets the same
# figure for its kit_churn step, and sizes that step's watchdog from it.
SCENARIO_FLAGS = {
    "kit_churn": ["--load-timeout", "30"],
    "release_gate": ["--kit-churn-load-timeout", "30"],
}

# How long `test` lets a scenario run before it is killed and reported as a
# setup problem: one hour by default, longer for the scenarios that carry their
# own duration.
SCENARIO_TIMEOUT_S = {
    "release_gate": 8 * 3600.0,
    "stability": 4 * 3600.0,
    "audio_loopback": 3 * 3600.0,
    "beat": 1800.0,                  # --depth full is ~10 min on this card
}
DEFAULT_TEST_TIMEOUT_S = 3600.0


# ── the interpreter this has to run under ───────────────────────────────────
#
# macOS: **nothing started by Homebrew's python3 can see
# a single CoreMIDI port.** crosspad-hil then prints "- / - / -" for ESP MIDI
# and every SysEx path fails -- `usb-mode set`, `audio route`, `capture` -- so
# a board in the MIDI+UAC2 profile, which has no CDC, cannot be reached or
# brought back at all. It looks exactly like a board that stopped enumerating.
#
# The block is inherited by the whole process tree and **cannot be escaped from
# inside it**: re-exec does not clear it, and neither does nesting Apple's
# python3 underneath. `python3` is Homebrew here, so the entry point is what has
# to change -- `/usr/bin/python3 tools/bench.py …`, or call crosspad-hil direct.
# PYTHONFRAMEWORK does not tell the interpreters apart; all three report
# "Python".
#
# The re-exec below is a *separate* fix and does not cure CoreMIDI: it puts
# pyserial on sys.executable, which `flash` and `fs_transfer` are spawned with
# and which Homebrew's python3 (PEP 668) will not install.
HIL_VENV_PY = os.path.expanduser("~/.venvs/crosspad-hil/bin/python")
HOMEBREW_PY_MARKERS = ("/homebrew/", "/usr/local/opt/python@", "/Cellar/")
# crosspad-hil subcommands that go out over MIDI and so die silently above.
MIDI_SUBCOMMANDS = ("usb-mode", "audio", "capture", "midi", "fl", "devices")


def _is_homebrew_python(path: str) -> bool:
    real = os.path.realpath(path)
    return any(m in real for m in HOMEBREW_PY_MARKERS)


def midi_is_blocked() -> bool:
    """True when this process tree cannot reach CoreMIDI (see above)."""
    if sys.platform != "darwin":
        return False
    return _is_homebrew_python(os.environ.get("BENCH_ORIG_PY", sys.executable))


def warn_if_midi_blocked(args: list[str]) -> None:
    if not midi_is_blocked() or not any(a in MIDI_SUBCOMMANDS for a in args):
        return
    print("warning: this was started by Homebrew's python3, so crosspad-hil "
          "cannot see any MIDI port —\n"
          "         the command will report 'no esp MIDI port'. Run it as:\n"
          f"           crosspad-hil {' '.join(args)}\n"
          "         or start bench.py with /usr/bin/python3.", file=sys.stderr)


def reexec_under_bench_python() -> None:
    """Re-exec so sys.executable carries pyserial. Does NOT restore CoreMIDI."""
    os.environ.setdefault("BENCH_ORIG_PY", sys.executable)
    if sys.platform != "darwin" or os.environ.get("BENCH_REEXEC"):
        return
    if not os.path.exists(HIL_VENV_PY):
        return                      # no venv: carry on, the tools will say so
    if os.path.realpath(sys.executable) == os.path.realpath(HIL_VENV_PY):
        return
    os.environ["BENCH_REEXEC"] = "1"
    os.execv(HIL_VENV_PY, [HIL_VENV_PY, os.path.abspath(__file__)] + sys.argv[1:])


# ── plumbing ────────────────────────────────────────────────────────────────

def hil_path() -> str | None:
    found = shutil.which("crosspad-hil")
    if found:
        return found
    for path in HIL_CANDIDATES:
        if os.path.exists(path):
            return path
    return None


def hil(*args: str, json_out: bool = False, timeout: float = 120.0):
    """Run crosspad-hil. Returns (returncode, stdout, parsed-or-None)."""
    exe = hil_path()
    if exe is None:
        return 127, "", None
    cmd = [exe] + (["--json"] if json_out else []) + list(args)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "", None
    out = (p.stdout or "") + (p.stderr or "")
    parsed = None
    if json_out:
        for line in (p.stdout or "").splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    parsed = json.loads(line)
                except json.JSONDecodeError:
                    pass
    return p.returncode, out, parsed


def cdc(verb: str, timeout_ms: int = 4000) -> str | None:
    """One CDC verb, or None when the board did not answer."""
    rc, out, _ = hil("cdc", "--timeout", str(timeout_ms), *verb.split())
    return out.strip() if rc == 0 else None


def fw_rollback_state(text: str | None) -> bool | None:
    """Parses FW_SLOTS' own "FWSLOT: rollback=0|1" trailer row. None when the
    line never came back — older firmware without it, or no reply at all."""
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("FWSLOT: rollback="):
            return line.endswith("=1")
    return None


def fw_running_slot_state(text: str | None) -> str | None:
    """The `state=` of the FW_SLOTS row marked `running=1`. None when no such
    row came back — firmware without the verb, or no reply at all."""
    for line in (text or "").splitlines():
        line = line.strip()
        if not line.startswith("FWSLOT:") or "running=1" not in line:
            continue
        for field in line.split():
            if field.startswith("state="):
                return field[len("state="):]
    return None


# An OTA over CDC is a trial boot like any switch: the image runs
# ESP_OTA_IMG_PENDING_VERIFY until the confirm timer's first tick, ~10 s in
# (kConfirmDelayMs, main/main.cpp), and any reset before then makes the
# bootloader abort it and boot the *other* slot. Long enough for that tick
# plus a slow boot; a board that is still on trial after it gets one
# FW_CONFIRM rather than a scenario reset.
CONFIRM_WAIT_S = 25.0
CONFIRM_POLL_S = 2.0


def await_confirm(read_state, confirm, sleep,
                  wait_s: float = CONFIRM_WAIT_S,
                  poll_s: float = CONFIRM_POLL_S) -> tuple[str | None, float, str]:
    """Waits for the running slot to leave `trial`. Returns (state, seconds,
    how): `how` is "timer" when the board's own confirm did it, "FW_CONFIRM"
    when the bound expired and we asked, "none" when there was nothing to wait
    for (already confirmed, or firmware with no FW_SLOTS)."""
    state = read_state()
    if state != "trial":
        return state, 0.0, "none"

    waited = 0.0
    while state == "trial" and waited < wait_s:
        sleep(poll_s)
        waited += poll_s
        state = read_state()
    if state != "trial":
        return state, waited, "timer"

    confirm()
    sleep(poll_s)
    waited += poll_s
    return read_state(), waited, "FW_CONFIRM"


def elf_sha256(path: str) -> str | None:
    """SHA-256 of a built .elf, the digest the app descriptor carries."""
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        return h.hexdigest()
    except OSError:
        return None


def app_sha_line(text: str | None) -> str | None:
    """The hex digest out of APP_SHA's `APPSHA: <64 hex>` reply."""
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("APPSHA:"):
            return line.split(maxsplit=1)[1].strip() if len(line.split()) > 1 else None
    return None


def rollback_advisory(rb: bool | None) -> tuple[bool | None, str]:
    """Maps fw_rollback_state()'s answer to the (ok, detail) `say()` needs for
    the "bootloader rollback" line in `check`. None (the trailer never came
    back — e.g. a CDC read timing out during the kit-load window right after
    `ready`) must stay distinct from False (rollback=0, a genuine "flashed
    bootloader lacks rollback"): collapsing them the same way makes a timeout
    read as "not supported". Only a real False gets
    the "recover" fix hint — the caller adds it, since None has none."""
    if rb is None:
        return None, "no answer (retry tools/bench.py check)"
    return (True if rb else None), ("supported" if rb else "not supported — flashed bootloader lacks rollback")


def board():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import crosspad_board
    return crosspad_board, crosspad_board.resolve(REPO)


def build_hint(d) -> str:
    args = " ".join(d.idf_args()) if d.rev else "-B build_v2 -DSDKCONFIG=sdkconfig.v2"
    return f"idf.py {args} build"


def device() -> dict | None:
    _, _, parsed = hil("devices", json_out=True)
    devices = (parsed or {}).get("devices") or []
    return devices[0] if devices else None


def tty_state(path: str) -> dict | None:
    """The port's saved VMIN and PARMRK, read with DTR/RTS held low so the peek
    itself cannot reset the board. None where it cannot be read (not Linux, no tty)."""
    if not sys.platform.startswith("linux"):
        return None
    try:
        import fcntl
        import struct
        import termios
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS))
        attrs = termios.tcgetattr(fd)
        # termios hands VMIN back as an int or a one-byte bytes, depending on the tty's state.
        raw = attrs[6][termios.VMIN]
        vmin = raw if isinstance(raw, int) else ord(raw)
        return {"vmin": vmin, "parmrk": bool(attrs[0] & termios.PARMRK)}
    except (OSError, termios.error):
        return None
    finally:
        os.close(fd)


def say(ok: bool | None, name: str, detail: str, fix: str = "") -> None:
    mark = {True: "ok  ", False: "FAIL", None: "warn"}[ok]
    print(f"  [{mark}] {name}: {detail}")
    if fix and ok is not True:
        print(f"         fix: {fix}")


# ── check ───────────────────────────────────────────────────────────────────

def cmd_check(_args) -> int:
    """Report what would trip an agent up, and what to type instead."""
    worst = EXIT_OK
    print("bench check")

    exe = hil_path()
    say(exe is not None, "crosspad-hil", exe or "not found",
        "pip install -e ~/GIT/crosspad-hil, or adjust HIL_CANDIDATES in this file")
    if exe is None:
        return EXIT_BENCH
    if shutil.which("crosspad-hil") is None:
        say(None, "PATH", "crosspad-hil is not on PATH — use tools/bench.py hil ...",
            f"export PATH=\"{os.path.dirname(exe)}:$PATH\"")

    # Not fixable without root, and not a blocker — but it is why a copied
    # "python tools/ota_flash.py" dies with "no such file or directory".
    say(True if shutil.which("python") else None, "python",
        shutil.which("python") or "absent — `python tools/...` fails, write python3",
        "use python3 (or tools/bench.py) in every command and doc")

    # crosspad-hil's own doctor reports a missing optional import as [ok] with
    # "missing" in the detail, so an agent reads green and then gets a confusing
    # "no such file" out of a scenario that never managed to record anything.
    # Name the consequence here instead.
    _, _, doc = hil("doctor", json_out=True, timeout=180)
    missing = {c["name"]: c.get("detail", "")
               for c in (doc or {}).get("checks", [])
               if "missing" in (c.get("detail") or "")}
    # A warning, not a failure: the bench itself is fine, only the scenarios that
    # record are out of reach, and the fix needs root.
    if "import:sounddevice" in missing:
        # Not a blocker: capture falls back to arecord, which works. It only costs the overrun counter.
        say(None, "host audio capture", "sounddevice absent (PortAudio) — arecord backend "
            "is used instead, no overrun counter",
            "sudo apt install libportaudio2 if you want the primary backend")
    # The real capture blocker on this machine: PipeWire owns the UAC2 card
    # whenever the DAW nodes are up, and an exclusive open then fails. Recording
    # falls back to the PipeWire device, which works — but say so, because the
    # take then goes through PipeWire's graph.
    _, nodes_out, _ = hil("audio", "nodes", "status", timeout=60)
    if "mic=up" in nodes_out:
        say(None, "DAW audio nodes", nodes_out.strip().splitlines()[-1]
            if nodes_out.strip() else "up",
            "captures record through PipeWire while these hold the card; stop them "
            "only if a measurement must come straight off the card")
    if "import:bleak" in missing:
        say(None, "host BLE", "bleak is not installed",
            "pip install bleak into the crosspad-hil venv — without it ble_midi "
            "reports ENV, which is not the same as the firmware having BLE off")

    # FL's factory drum WAVs are Ogg Vorbis in a RIFF wrapper (format code
    # 0x674f, "OggS" right there in the header), so converting an FPC preset
    # needs a decoder. Without it `kit from-fpc` resolves all sixteen paths and
    # then skips every one of them, which reads as a broken feature.
    if shutil.which("ffmpeg") is None:
        say(None, "host audio decode", "ffmpeg absent",
            "sudo apt install ffmpeg — without it `kit from-fpc` cannot convert FL "
            "factory kits (their WAVs are Ogg-in-RIFF); kits of plain PCM WAVs are fine")

    cb, d = board()
    say(d.rev is not None and not d.mismatch, "board", cb.describe(d),
        "tools/bench.py flash   (flashes the image built for this board)" if d.mismatch
        else f"tools/crosspad_board.py --set {cb.DEFAULT_REV}")
    fw = os.path.join(REPO, d.build_dir, "CrossPad.bin") if d.rev else None
    fw = fw if fw and os.path.exists(fw) else None
    assets = os.path.join(REPO, d.build_dir, "assets.bin") if d.rev else None
    assets = assets if assets and os.path.exists(assets) else None
    say(fw is not None, "build", os.path.relpath(fw, REPO) if fw else "nothing built", build_hint(d))

    dev = device()
    if dev is None:
        say(False, "device", "no CrossPad on USB", "check the cable / power the board")
        return EXIT_BENCH
    mode = dev.get("usb_mode")
    in_audio = mode == "audio"
    say(not in_audio, "usb profile", str(mode),
        "tools/bench.py ready   (switches back to the CDC profile)")
    if in_audio:
        return EXIT_BENCH
    cdc_path = ((dev.get("ports") or {}).get("cdc") or {}).get("path")
    tty = tty_state(cdc_path) if cdc_path else None
    if tty is not None:
        vmin = tty["vmin"]
        say(True if vmin >= 1 else None, "tty VMIN", f"{cdc_path} min={vmin}",
            f"stty -F {cdc_path} min 1   (a tty keeps its termios across reopen; VMIN=0 "
            "left by a pyserial tool makes Chromium's Web Serial read 0 bytes and report "
            "\"The device has been lost\" — the browser companion cannot connect)")
        say(True if not tty["parmrk"] else None, "tty PARMRK", f"{cdc_path} parmrk={'on' if tty['parmrk'] else 'off'}",
            f"stty -F {cdc_path} -parmrk   (left behind by Chromium's Web Serial; with it the line "
            "discipline doubles every 0xFF byte, so a binary pull through a reader that does not "
            "clear it arrives 60-odd bytes long — crosspad-hil clears it since 2026-09-11)")

    # crosspad-hil names the USB profile after the ports that enumerate, and
    # transfer mode keeps its CDC — so the line above says "default" for a board
    # whose card is on the host and whose engines never started. Ask the board.
    msc = cdc("USB_MSC --expect USB_MSC:")
    if msc and "active=1" in msc:
        say(False, "usb transfer mode", msc.replace("USB_MSC: ", ""),
            "tools/bench.py hil cdc 'USB_MSC 0' --expect OK   (reboots into the normal profile; "
            "no audio, no kits and the SD card belongs to the host until then)")
        return EXIT_BENCH

    # Network mode keeps CDC too, but has no engine, no kits and no APP_LIST:
    # every scenario would fail as a bench problem. Ask the board.
    net = cdc("NET_MODE --expect NET_MODE:")
    if net and "active=1" in net:
        say(False, "network mode", net.replace("NET_MODE: ", ""),
            "tools/bench.py hil cdc 'NET_MODE 0' --expect OK   (reboots into a normal boot; "
            "no audio and no kits until then)")
        return EXIT_BENCH

    libver = cdc("APP_VERSIONS", 6000)
    line = next((l for l in (libver or "").splitlines() if l.startswith("LIBVER:")), None)
    say(line is not None, "cdc", line or "no LIBVER line in APP_VERSIONS",
        "tools/bench.py ready")
    if line is None:
        return EXIT_BENCH

    route = cdc("AUDIO_STATUS")
    home = f"mic_src={ROUTE_HOME_MIC_SRC}" in (route or "") and \
           f"adc=[{ROUTE_HOME_ADC0}," in (route or "")
    say(home, "capture route", (route or "unknown").replace("AUDIO: ", ""),
        "tools/bench.py ready   (a railing DAC->ADC route fails every audio test)")
    worst = worst if home else EXIT_BENCH

    kit = cdc("KIT_STATUS", 8000) or ""
    loaded = "current=-1" not in kit
    say(loaded, "kit", kit.replace("KITSTATUS: ", "") or "no answer",
        "tools/bench.py ready   (scenarios needing a kit exit 2 without one)")
    worst = worst if loaded else EXIT_BENCH

    # Advisory, not a failure: rollback=0 means only that the flashed
    # bootloader itself lacks rollback support (fw_rollback_supported() reads
    # its own descriptor version, which is below the minimum) — deterministic,
    # independent of whether this device has ever switched slots.
    rb = fw_rollback_state(cdc("FW_SLOTS"))
    ok, detail = rollback_advisory(rb)
    say(ok, "bootloader rollback", detail,
        "" if rb is None else "tools/bench.py recover   (reflashes the bootloader with app rollback support)")

    if assets:
        say(None, "assets", f"{os.path.relpath(assets, REPO)} — OTA never touches this "
            "partition", "tools/bench.py flash --assets   (then FS_LIST /assets to prove it)")
    return worst


# ── ready ───────────────────────────────────────────────────────────────────

# Right after a reset the SD card takes ~15 s to mount and the kit scan longer
# to list it; until then KIT_LOAD answers "bad kit id" (or "scanning"). That is
# a card still coming up, not a missing kit.
KIT_WAIT_S = 45.0
KIT_RETRY_S = 3.0
_KIT_NOT_YET = ("bad kit id", "scanning", "kit scan")


def load_home_kit(note, hil_fn=None, sleep=time.sleep, clock=time.monotonic,
                  wait_s: float = KIT_WAIT_S) -> int:
    """KIT_LOAD 0, waiting out a card that is still mounting; EXIT_BENCH with
    the board's own words when it never comes."""
    hil_fn = hil_fn or hil
    deadline = clock() + wait_s
    while True:
        rc, out, _ = hil_fn("kit", "load", "0", "--wait", timeout=180)
        if rc == 0:
            return EXIT_OK
        text = out.strip()
        if not any(s in text.lower() for s in _KIT_NOT_YET) or clock() >= deadline:
            print(text)
            if any(s in text.lower() for s in _KIT_NOT_YET):
                print(f"kit: the card never offered kit 0 within {wait_s:.0f} s of the "
                      "reset — is the SD card in, and does it hold a kit?")
            return EXIT_BENCH
        note(f"kit: the card is not listed yet ({text.splitlines()[-1] if text else '?'})"
             f" — waiting {KIT_RETRY_S:.0f} s for the mount and the kit scan")
        sleep(KIT_RETRY_S)


def ensure(verbose: bool = True, kit: bool = True) -> int:
    """Put the bench where a test can trust it: CDC profile, MICS, a kit.

    ``kit=False`` skips the kit: flashing does not need one, and right after a
    reset the card is not even mounted yet.
    """
    def note(msg: str) -> None:
        if verbose:
            print(msg)

    if hil_path() is None:
        print("crosspad-hil not found — see tools/bench.py check")
        return EXIT_BENCH

    dev = device()
    if dev is None:
        print("no CrossPad on USB")
        return EXIT_BENCH

    msc = cdc("USB_MSC --expect USB_MSC:")
    if msc and "active=1" in msc:
        # Nothing a scenario needs exists in this mode: no engine, no kit list,
        # and the card is the host's. Leaving it is a reboot.
        note("usb transfer mode: active -> leaving (reboots into the normal profile)")
        cdc("USB_MSC 0 --expect OK")
        time.sleep(12)
        dev = device() or {}

    net = cdc("NET_MODE --expect NET_MODE:")
    if net and "active=1" in net:
        # Nothing a scenario needs exists in this mode. Leaving it is a reboot.
        note("network mode: active -> leaving (reboots into a normal boot)")
        cdc("NET_MODE 0 --expect OK")
        time.sleep(12)
        dev = device() or {}

    if dev.get("usb_mode") == "audio":
        note("usb profile: audio -> default (the UAC2 profile has no CDC endpoint)")
        rc, out, _ = hil("usb-mode", "set", "default", timeout=180)
        if rc != 0:
            # A board can wedge in the audio profile and stop acting on the
            # switch; a hard reset over the STM bridge is the documented way out
            # (console --reset pulses DTR/RTS).
            note(f"switch refused ({out.strip().splitlines()[0] if out.strip() else 'no output'})"
                 " — hard-resetting over the STM bridge")
            hil("console", "--reset", "--timeout", "25", timeout=120)
            time.sleep(8)
            dev = device() or {}
            if dev.get("usb_mode") == "audio":
                rc, out, _ = hil("usb-mode", "set", "default", timeout=180)
            if (device() or {}).get("usb_mode") == "audio":
                print("still stuck in the audio profile after a hard reset — "
                      "this one needs the cable replugged (TODO: 'ESP potrafi zniknąć "
                      "z USB hosta po zawieszeniu w profilu audio')")
                return EXIT_BENCH
        time.sleep(6)

    route = cdc("AUDIO_STATUS") or ""
    if f"mic_src={ROUTE_HOME_MIC_SRC}" not in route or f"adc=[{ROUTE_HOME_ADC0}," not in route:
        note(f"capture route: {route.replace('AUDIO: ', '') or 'unknown'} -> MICS")
        # Input before the source, the order audio_route_apply_preset uses.
        cdc("AUDIO_ADC 0 LINE2")
        cdc(f"AUDIO_MIC_SRC {ROUTE_HOME_MIC_SRC}")

    if not kit:
        return EXIT_OK
    current = cdc("KIT_STATUS", 8000) or ""
    if "current=-1" in current or not current:
        # KIT_LIST refuses while the boot scan walks the card; that is not a hang.
        note("kit: none loaded -> loading kit 0")
        return load_home_kit(note)

    return EXIT_OK


def cmd_ready(args) -> int:
    rc = ensure(verbose=True)
    if rc != EXIT_OK:
        return rc
    print()
    return cmd_check(args)


# ── flash ───────────────────────────────────────────────────────────────────

def cmd_flash(args) -> int:
    """Flash the build for the connected board and prove on-device that it is the one."""
    # No kit: the flash does not need one, and a board reset a moment ago has
    # no card mounted yet -- "KIT_LOAD bad kit id" used to end the run here,
    # before anything was flashed.
    if ensure(kit=False) != EXIT_OK:
        print("bench not ready — nothing was flashed")
        return EXIT_BENCH

    cb, d = board()
    if d.rev is None:
        print(cb.describe(d))
        return EXIT_BENCH
    fw = os.path.join(REPO, d.build_dir, "CrossPad.bin")
    if not os.path.exists(fw):
        print("nothing built — " + build_hint(d))
        return EXIT_BENCH
    print(f"\nflashing {os.path.relpath(fw, REPO)}")
    # crosspad-hil's own python, when there is one: ota_flash.py then takes the
    # hub's exclusive op, and the hub -- which held the port for 5 s after the
    # checks above and read OTA_READY itself -- lets go of it first.
    hil_exe = hil_path()
    hil_py = os.path.join(os.path.dirname(hil_exe), "python") if hil_exe else ""
    py = hil_py if os.path.exists(hil_py) else sys.executable
    p = subprocess.run([py, os.path.join(REPO, "tools", "ota_flash.py"), fw],
                       cwd=REPO)
    if p.returncode != 0:
        return EXIT_BENCH
    time.sleep(10)

    libver = next((l for l in (cdc("APP_VERSIONS", 8000) or "").splitlines()
                   if l.startswith("LIBVER:")), None)
    print(f"on the board: {libver or 'no LIBVER line — the board did not answer'}")

    # An OTA over CDC is a trial boot: leave now and the first scenario reset
    # aborts it, the bootloader boots the other slot, and everything below
    # passes against the *previous* firmware.
    state, waited, how = await_confirm(
        lambda: fw_running_slot_state(cdc("FW_SLOTS", 8000)),
        lambda: cdc("FW_CONFIRM", 8000),
        time.sleep)
    if state is None:
        print("trial: firmware without FW_SLOTS — not waiting for a confirm (older image)")
    elif state == "confirmed" and how == "none":
        print("trial: the running slot was already confirmed")
    elif state == "confirmed":
        print(f"trial: confirmed after {waited:.0f} s"
              + (" by FW_CONFIRM" if how == "FW_CONFIRM" else ""))
    else:
        print(f"trial: the running slot is still {state} after {waited:.0f} s —"
              " a reset from here rolls this image back")

    # "Flashed, nothing changed" detected rather than inferred: LIBVER and the
    # app descriptor's compile time both read identical across two builds of
    # one commit, the ELF digest does not.
    want = elf_sha256(os.path.join(REPO, d.build_dir, "CrossPad.elf"))
    got = app_sha_line(cdc("APP_SHA", 8000))
    if want and got:
        print(f"image: {'matches' if got == want else 'MISMATCH — the board is running another build'}"
              f" {os.path.join(d.build_dir, 'CrossPad.elf')} ({got[:9]}…)")
        if got != want:
            return EXIT_BENCH
    elif want and got is None:
        print("image: no APP_SHA reply — cannot prove which build is running (older firmware)")

    if args.assets:
        image = os.path.join(REPO, d.build_dir, "assets.bin")
        print(f"\nflashing assets from {os.path.relpath(image, REPO) if image else '?'}"
              " (OTA does not touch this partition)")
        p = subprocess.run([sys.executable, os.path.join(REPO, "tools", "fs_transfer.py"),
                            "assets"], cwd=REPO)
        if p.returncode != 0:
            return EXIT_BENCH
        time.sleep(12)
        listing = cdc("FS_LIST /assets", 8000) or ""
        print(f"/assets holds {len(listing.splitlines())} entries")

    rc, out, _ = hil("run", "smoke", timeout=300)
    print(out.strip().splitlines()[-1] if out.strip() else "smoke produced no output")
    return EXIT_OK if rc == 0 else EXIT_FAIL


# ── test ────────────────────────────────────────────────────────────────────

def cmd_test(args) -> int:
    """Run scenarios, applying the flags each one needs to be fair."""
    worst = EXIT_OK
    for name in args.scenarios:
        # Before *each* scenario, not once: a scenario that resets the board
        # (smoke does) leaves it with no kit loaded, and the next one then fails
        # as a bench problem rather than telling you anything about the firmware.
        if ensure(verbose=False) != EXIT_OK:
            print(f"{name}: skipped — bench not ready (tools/bench.py check)")
            return EXIT_BENCH
        flags = SCENARIO_FLAGS.get(name, [])
        if flags:
            print(f"\n{name} (+{' '.join(flags)} needed)")
        else:
            print(f"\n{name}")
        timeout = args.timeout or SCENARIO_TIMEOUT_S.get(name, DEFAULT_TEST_TIMEOUT_S)
        rc, out, _ = hil("run", name, *flags, *args.rest, timeout=timeout)
        if rc == 124:
            # Killed by this script, not by the firmware: say so, and count it
            # as a bench problem rather than letting "(no output)" pass for one.
            print(f"{name}: killed by bench.py after {timeout:.0f} s "
                  f"(pass --timeout SECONDS to extend)")
            worst = max(worst, EXIT_BENCH)
            continue
        tail = [l for l in out.strip().splitlines() if l.strip()]
        print("\n".join(tail[-3:]) if tail else "(no output)")
        worst = max(worst, rc if rc in (EXIT_FAIL, EXIT_BENCH) else EXIT_OK)
    return worst


def cmd_recover(args) -> int:
    """Reflash over UART through the STM bridge, for a board with no CDC left.

    A firmware that panics before USB comes up cannot be OTA-flashed: there is
    no CDC endpoint to talk to, and no window either — a bootloop can die
    under a second in. The bridge VCP (0x0483:5740) wires DTR/RTS to EN
    and GPIO0, so esptool can put the ESP in download mode from there, and a full
    `idf.py flash` also rewrites ota_data_initial so the boot selection stops
    pointing at the bad slot.
    """
    console = None
    dev = device()
    if dev:
        ports = dev.get("ports") or {}
        console = (ports.get("console") or {}).get("path")
    console = args.port or console or "/dev/ttyACM0"

    cb, d = board()
    build = args.build or d.build_dir
    if not build:
        print(cb.describe(d))
        return EXIT_BENCH
    if not os.path.exists(os.path.join(REPO, build, "CrossPad.bin")):
        print("nothing built — " + build_hint(d))
        return EXIT_BENCH
    sdkconfig = args.sdkconfig or ("sdkconfig." + os.path.basename(build).replace("build_", "")
                                   if os.path.basename(build).startswith("build_") else "sdkconfig")
    print(f"recovering over {console} from {build} (sdkconfig {sdkconfig})")
    idf = shutil.which("idf.py")
    if idf is None:
        print("idf.py not on PATH — source the ESP-IDF export script first")
        return EXIT_BENCH
    p = subprocess.run([idf, "-B", build, f"-DSDKCONFIG={sdkconfig}",
                        "-p", console, "-b", "460800", "flash"], cwd=REPO)
    if p.returncode != 0:
        print("UART flash failed — if esptool could not sync, the board needs its "
              "BOOT button held while it is reset")
        return EXIT_BENCH
    time.sleep(14)
    return cmd_ready(args)


def cmd_hil(args) -> int:
    exe = hil_path()
    if exe is None:
        print("crosspad-hil not found — see tools/bench.py check")
        return EXIT_BENCH
    warn_if_midi_blocked(args.rest)
    return subprocess.run([exe] + args.rest).returncode


def main() -> int:
    reexec_under_bench_python()
    # `hil` is a passthrough, so argparse must not see the arguments at all:
    # a leading option (crosspad-hil --json ...) would otherwise be rejected as
    # unknown before argparse.REMAINDER ever gets a look at it.
    # `test` takes scenario names plus whatever flags the scenarios take
    # (--rounds, --load-timeout, …). argparse cannot tell those from its own, so
    # split them by hand rather than making the caller remember a `--`.
    if len(sys.argv) > 1 and sys.argv[1] == "test":
        # A flag's value follows it and looks like a scenario name, so the two
        # have to be kept together rather than filtered apart.
        names: list[str] = []
        rest: list[str] = []
        argv = sys.argv[2:]
        i = 0
        while i < len(argv):
            if argv[i].startswith("-"):
                rest.append(argv[i])
                if i + 1 < len(argv) and not argv[i + 1].startswith("-") \
                        and "=" not in argv[i]:
                    rest.append(argv[i + 1])
                    i += 1
            else:
                names.append(argv[i])
            i += 1
        if not names:
            print("usage: bench.py test SCENARIO [SCENARIO…] [scenario flags]")
            return EXIT_BENCH
        # --timeout is bench.py's own, not a scenario flag: pull it out of rest.
        timeout = None
        if "--timeout" in rest:
            i = rest.index("--timeout")
            timeout = float(rest[i + 1])
            del rest[i:i + 2]
        return cmd_test(argparse.Namespace(scenarios=names, rest=rest,
                                           timeout=timeout, assets=False))

    if len(sys.argv) > 1 and sys.argv[1] == "hil":
        exe = hil_path()
        if exe is None:
            print("crosspad-hil not found — see tools/bench.py check")
            return EXIT_BENCH
        warn_if_midi_blocked(sys.argv[2:])
        return subprocess.run([exe] + sys.argv[2:]).returncode

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check", help="what is wrong and how to fix it").set_defaults(fn=cmd_check)
    sub.add_parser("ready", help="CDC profile, MICS route, a kit loaded").set_defaults(fn=cmd_ready)

    f = sub.add_parser("flash", help="flash the build for the connected board, prove it landed")
    f.add_argument("--assets", action="store_true", help="also reflash the assets partition")
    f.set_defaults(fn=cmd_flash)

    t = sub.add_parser("test", help="run scenarios with the flags each one needs")
    t.add_argument("scenarios", nargs="+",
                   help="scenario names; anything starting with - is passed to each")
    t.set_defaults(fn=cmd_test, assets=False)

    r = sub.add_parser("recover", help="UART-reflash through the STM bridge (no CDC left)")
    r.add_argument("--port", default=None, help="bridge VCP (default: the console port)")
    r.add_argument("--build", default=None, help="build dir (default: the connected/remembered board's)")
    r.add_argument("--sdkconfig", default=None, help="sdkconfig (default: from the build dir)")
    r.set_defaults(fn=cmd_recover, assets=False)

    h = sub.add_parser("hil", help="raw crosspad-hil with the venv resolved")
    h.add_argument("rest", nargs=argparse.REMAINDER)
    h.set_defaults(fn=cmd_hil)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
