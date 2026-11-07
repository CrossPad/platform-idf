#!/usr/bin/env python3
"""Which board revision to build and flash for — one answer for every tool.

Order: --board, then the connected board (crosspad-hil reports the hardware
revision from the STM bridge and the flashed one from the ESP), then the last
choice in crosspad.local.json. Nothing known → exit 2 and say how to choose.

  tools/crosspad_board.py                 decision and where it came from
  tools/crosspad_board.py --set v2        remember a revision
  tools/crosspad_board.py --idf-args      "-B build_v2 -DSDKCONFIG=sdkconfig.v2"
  tools/crosspad_board.py --json          for crosspad-mcp
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REVS = ("v1", "v2")
DEFAULT_REV = "v2"
LOCAL_CONFIG = "crosspad.local.json"
VSCODE_SETTINGS = Path(".vscode") / "settings.json"
HIL_DEVICES_TIMEOUT_S = 30.0
EXIT_OK, EXIT_BENCH = 0, 2


@dataclass
class Decision:
    rev: str | None
    source: str
    device: str | None = None
    pcb: int | None = None
    fw_rev: str | None = None
    devices: list[dict] = field(default_factory=list)

    @property
    def build_dir(self) -> str | None:
        return f"build_{self.rev}" if self.rev else None

    @property
    def sdkconfig(self) -> str | None:
        return f"sdkconfig.{self.rev}" if self.rev else None

    @property
    def mismatch(self) -> bool:
        return bool(self.rev and self.fw_rev and self.fw_rev != self.rev)

    def idf_args(self) -> list[str]:
        return ["-B", self.build_dir, f"-DSDKCONFIG={self.sdkconfig}"]

    def to_dict(self) -> dict:
        d = asdict(self)
        d.update(build_dir=self.build_dir, sdkconfig=self.sdkconfig, mismatch=self.mismatch)
        return d


def device_rev(dev: dict) -> str | None:
    rev = dev.get("board_rev") or dev.get("fw_rev")
    return rev if rev in REVS else None


def list_devices() -> list[dict]:
    """crosspad-hil owns device I/O; without it there is simply nothing detected."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from bench import hil
    rc, _, parsed = hil("devices", json_out=True, timeout=HIL_DEVICES_TIMEOUT_S)
    if rc != 0 or not parsed:
        return []
    return parsed.get("devices") or []


def _local_path(project_dir: Path) -> Path:
    return Path(project_dir) / LOCAL_CONFIG


def load_memory(project_dir: Path) -> str | None:
    try:
        rev = json.loads(_local_path(project_dir).read_text()).get("board")
    except (OSError, ValueError):
        return None
    return rev if rev in REVS else None


def write_vscode(project_dir: Path, rev: str) -> bool:
    path = Path(project_dir) / VSCODE_SETTINGS
    try:
        data = json.loads(path.read_text())
    except OSError:
        return False
    except ValueError:
        print(f"{path}: not plain JSON, left alone — set idf.buildPath yourself", file=sys.stderr)
        return False
    data["idf.buildPath"] = "${workspaceFolder}/build_" + rev
    data["idf.buildPathWin"] = "${workspaceFolder}\\build_" + rev
    data["idf.sdkconfigFilePath"] = "${workspaceFolder}/sdkconfig." + rev
    path.write_text(json.dumps(data, indent=4) + "\n")
    return True


def commit(project_dir: Path, rev: str) -> None:
    """Remember the revision and point VS Code's ESP-IDF extension at it."""
    path = _local_path(project_dir)
    try:
        data = json.loads(path.read_text())
    except OSError:
        data = {}
    except ValueError:
        print(f"{path}: not valid JSON, board not remembered — fix or delete it", file=sys.stderr)
        write_vscode(project_dir, rev)
        return
    if data.get("board") != rev:
        data["board"] = rev
        path.write_text(json.dumps(data, indent=2) + "\n")
    write_vscode(project_dir, rev)


def resolve(project_dir: Path = REPO, explicit: str | None = None,
            device: str | None = None, devices: list[dict] | None = None) -> Decision:
    if explicit:
        if explicit not in REVS:
            raise ValueError(f"unknown board revision {explicit!r} (known: {', '.join(REVS)})")
        return Decision(explicit, "explicit")
    devs = list_devices() if devices is None else devices
    known = [d for d in devs if device_rev(d) and (device is None or d.get("id") == device)]
    revs = {device_rev(d) for d in known}
    if len(revs) == 1:
        first = known[0]
        return Decision(revs.pop(), "device", device=first.get("id"), pcb=first.get("pcb"),
                        fw_rev=first.get("fw_rev"), devices=known)
    if len(revs) > 1:
        return Decision(None, "ambiguous", devices=known)
    if device is not None:
        return Decision(None, "no-such-device", device=device, devices=devs)
    remembered = load_memory(project_dir)
    if remembered:
        return Decision(remembered, "memory", devices=devs)
    return Decision(None, "none", devices=devs)


def describe(d: Decision) -> str:
    if d.source == "ambiguous":
        boards = ", ".join(f"{x.get('id')} {device_rev(x)}" for x in d.devices)
        return f"boards of different revisions connected ({boards}) — pass --device or --board"
    if d.source == "no-such-device":
        return f"no connected board {d.device} with a known revision — crosspad-hil devices lists them"
    if d.rev is None:
        return (f"no board revision known — connect a board, or "
                f"tools/crosspad_board.py --set {DEFAULT_REV}")
    text = f"{d.rev}  ({d.source}"
    if d.device:
        text += f" {d.device}"
    if d.pcb is not None:
        text += f", PCB {d.pcb}"
    if d.fw_rev:
        text += f", firmware {d.fw_rev}"
    text += ")"
    if d.mismatch:
        text += f"  MISMATCH: a {d.fw_rev} image on a {d.rev} board"
    return text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", choices=REVS, help="use this revision")
    ap.add_argument("--device", help="crosspad-hil device id, when several boards are connected")
    ap.add_argument("--set", dest="set_rev", choices=REVS, help="remember this revision and stop")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--idf-args", action="store_true", help="print the idf.py -B/-DSDKCONFIG arguments")
    args = ap.parse_args(argv)

    if args.set_rev:
        commit(REPO, args.set_rev)
        d = Decision(args.set_rev, "explicit")
    else:
        d = resolve(REPO, explicit=args.board, device=args.device)
        if d.rev and d.source in ("explicit", "device"):
            commit(REPO, d.rev)

    if args.json:
        print(json.dumps(d.to_dict()))
    elif args.idf_args and d.rev:
        print(" ".join(d.idf_args()))
    else:
        print(describe(d), file=sys.stdout if d.rev else sys.stderr)
    return EXIT_OK if d.rev else EXIT_BENCH


if __name__ == "__main__":
    sys.exit(main())
