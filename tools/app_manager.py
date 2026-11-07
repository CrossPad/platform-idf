#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CrossPad App Manager — ESP-IDF platform wrapper.

Downloads the shared core module from CrossPad/crosspad-apps on first use,
then delegates all commands to it with ESP-IDF-specific configuration.
"""

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

CORE_REPO = "CrossPad/crosspad-apps"
CORE_FILE = "crosspad_app_manager.py"
# CI pins the core to a commit (tools/app_manager.core-ref via
# CROSSPAD_APPS_CORE_REF) so a PR build and a later tag build run the same
# manager. With the variable unset (a developer's machine) the core follows
# main, refreshed at most hourly; once it is set it must be a full sha, and
# is downloaded on every run.
CORE_REF_ENV = "CROSSPAD_APPS_CORE_REF"


def _pinned_ref() -> str | None:
    """The pinned commit, None when unpinned. A set but malformed value is
    fatal: falling back to main would lose the pin without a word."""
    ref = os.environ.get(CORE_REF_ENV)
    if ref is None:
        return None
    if not re.fullmatch(r"[0-9a-f]{40}", ref):
        print(f"{CORE_REF_ENV} must be a full 40-hex crosspad-apps commit sha, got {ref!r}.")
        sys.exit(1)
    return ref


def _core_url(ref: str) -> str:
    return f"https://raw.githubusercontent.com/{CORE_REPO}/{ref or 'main'}/{CORE_FILE}"


CORE_CACHE = Path(__file__).parent / CORE_FILE
CORE_STAMP = CORE_CACHE.with_name(".crosspad_app_manager.downloaded")
CORE_PREV = CORE_CACHE.with_name(CORE_FILE + ".prev")
CORE_TTL_S = 3600  # re-check upstream at most hourly


def _digest(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _locally_modified() -> bool:
    """Is the cached copy someone's work in progress rather than our download?

    Without this the hourly refresh silently replaced a manager being edited
    in a sibling checkout — the TUI under test was upstream's, and every
    change appeared to do nothing.
    """
    if not CORE_CACHE.exists() or not CORE_STAMP.exists():
        return False
    try:
        return _digest(CORE_CACHE) != CORE_STAMP.read_text().strip()
    except OSError:
        return False


def _fetch_core(ref: str = "") -> str | None:
    """Anonymous HTTPS first — reading a public file needs no GitHub account —
    then gh for machines where only gh gets through (a proxy set up for it)."""
    import urllib.request
    try:
        req = urllib.request.Request(_core_url(ref), headers={"User-Agent": "crosspad-apps"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode("utf-8")
    except (OSError, ValueError):
        pass
    try:
        import base64
        r = subprocess.run(
            ["gh", "api", f"repos/{CORE_REPO}/contents/{CORE_FILE}?ref={ref or 'main'}", "--jq", ".content"],
            capture_output=True, text=True, check=True, timeout=15)
        return base64.b64decode(r.stdout.strip()).decode()
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired, ValueError):
        return None


def _ensure_core():
    """Refresh the shared core from crosspad-apps, at most once an hour.

    A download replaces the cached copy only if it compiles, and the copy it
    replaces is kept as .prev — a broken or truncated upstream file must not
    leave a machine without a working manager. Offline, the cached copy runs.
    """
    pinned = _pinned_ref()
    if pinned:
        text = _fetch_core(pinned)
        if not text or "class AppManager" not in text:
            print(f"{CORE_FILE} @ {pinned[:12]}: cannot download it ({CORE_REF_ENV}).")
            sys.exit(1)
        CORE_CACHE.parent.mkdir(parents=True, exist_ok=True)
        CORE_CACHE.write_text(text, encoding="utf-8")
        CORE_STAMP.write_text(_digest(CORE_CACHE) + "\n")
        return
    refresh = os.environ.get("CROSSPAD_MANAGER_REFRESH")
    if _locally_modified() and not refresh:
        print(f"Using your edited {CORE_FILE} "
              f"(CROSSPAD_MANAGER_REFRESH=1 to take upstream's again).")
        return
    if CORE_CACHE.exists() and not refresh:
        import time
        if time.time() - CORE_CACHE.stat().st_mtime < CORE_TTL_S:
            return
    text = _fetch_core()
    try:
        compile(text or "", CORE_FILE, "exec")
        usable = bool(text) and "class AppManager" in text
    except SyntaxError:
        usable = False
    if usable:
        CORE_CACHE.parent.mkdir(parents=True, exist_ok=True)
        if CORE_CACHE.exists():
            CORE_PREV.write_bytes(CORE_CACHE.read_bytes())
        CORE_CACHE.write_text(text, encoding="utf-8")
        CORE_STAMP.write_text(_digest(CORE_CACHE) + "\n")
        return
    if CORE_CACHE.exists():
        # The cached copy is the normal offline case; the screens say
        # "no connection" once, so this line stays out of their way.
        CORE_CACHE.touch()
        return
    print(f"No connection, and {CORE_FILE} has never been downloaded here.")
    print("  Connect to the internet once and start this again.")
    sys.exit(1)


def _load_core():
    _ensure_core()
    for path in (CORE_CACHE, CORE_PREV):
        if not path.exists():
            continue
        try:
            # An explicit loader: ".py.prev" has no suffix importlib recognises.
            from importlib.machinery import SourceFileLoader
            loader = SourceFileLoader("crosspad_app_manager", str(path))
            spec = importlib.util.spec_from_loader("crosspad_app_manager", loader)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
        except Exception as e:  # noqa: BLE001 - fall back to the previous copy
            print(f"{path.name} failed to load ({type(e).__name__}: {e}); trying the previous copy.")
            continue
        if path == CORE_PREV:
            CORE_CACHE.write_bytes(CORE_PREV.read_bytes())
            CORE_STAMP.write_text(_digest(CORE_CACHE) + "\n")
        return mod
    print(f"No working {CORE_FILE}. Connect to the internet and start this again.")
    sys.exit(1)


core = _load_core()

ESP_IDF_CONFIG = core.PlatformConfig(
    platform="esp-idf",
    lib_dir="components",
    official_org="CrossPad",
    lib_prefix="crosspad-",
)

import crosspad_board  # noqa: E402  (tools/ is on sys.path: this file's own dir)

ESP_IDF_CONFIG.board_resolve = lambda: crosspad_board.resolve().to_dict()
ESP_IDF_CONFIG.board_set = lambda rev: crosspad_board.commit(crosspad_board.REPO, rev)
ESP_IDF_CONFIG.board_revs = crosspad_board.REVS


def _device_probe():
    """First connected CrossPad as crosspad-hil reports it; None without hil or board."""
    devices = crosspad_board.list_devices()
    return devices[0] if devices else None


def _stream(cmd: list[str], on_line) -> int:
    proc = subprocess.Popen(cmd, cwd=str(crosspad_board.REPO), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1)
    for line in proc.stdout:
        on_line(line.rstrip("\r\n"))
    return proc.wait()


def _flash_ota(rev, on_line, device=None, image=None) -> int:
    # ota_flash.py switches an audio-profile board to CDC over MIDI SysEx by
    # itself, refuses an image of another revision, and exits 2 when no board
    # answers — the pipeline maps 2 to "the board isn't answering". `device`
    # picks one of several connected boards; `image` is a downloaded release
    # image standing in for build_<rev>/CrossPad.bin.
    cmd = [sys.executable, str(crosspad_board.REPO / "tools" / "ota_flash.py")]
    if image:
        cmd.append(image)
    if rev:
        cmd += ["--board", rev]
    if device:
        cmd += ["--device", device]
    return _stream(cmd, on_line)


def _flash_uart(rev, console, on_line) -> int:
    # A board with no CDC left: esptool through the STM bridge VCP, whose
    # DTR/RTS reach EN and GPIO0 (what `bench.py recover` does).
    idf = shutil.which("idf.py")
    if idf is None:
        return 127
    return _stream([idf, "-B", f"build_{rev}", f"-DSDKCONFIG=sdkconfig.{rev}",
                    "-p", console, "-b", "460800", "flash"], on_line)


ESP_IDF_CONFIG.device_probe = _device_probe
ESP_IDF_CONFIG.flash_ota = _flash_ota
ESP_IDF_CONFIG.flash_uart = _flash_uart


class AppManager(core.AppManager):
    """ESP-IDF-specific AppManager with default config."""

    def __init__(self, project_dir: str):
        super().__init__(project_dir, ESP_IDF_CONFIG)


def main():
    # The idf.py extension (idf_ext.py) imports AppManager directly; this entry
    # point is for running the wrapper standalone, the way the crosspad-pc and
    # Arduino wrappers already work. Without it `python3 tools/app_manager.py
    # sync` parsed nothing, did nothing and exited 0.
    os.chdir(Path(__file__).resolve().parent.parent)
    core.cli_main(ESP_IDF_CONFIG)


if __name__ == "__main__":
    main()
