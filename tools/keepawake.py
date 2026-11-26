#!/usr/bin/env python3
"""Keep a CrossPad awake while a bench session runs, or wake it if it only dimmed.

The board dims 600 s after the last activity and takes itself into soft-off at
1800 s -- docs/subsystems/power-button.md has the ladder and the measurements.
In the dim state (the STM's PWR_STATE_LIGHT) the ESP is up and the console
answers, but the screen and the pads stay dark: an automated run reads replies
and concludes the device is live. Past the sleep timeout the ESP has restarted
into its soft-off gate and there is no console at all -- the STM wakes on its own
power button or a charger event, and no register a host can write asks it to.

So this does the half that can be done and says plainly when it cannot:

    tools/keepawake.py status                what the board is doing (the PWR line)
    tools/keepawake.py wake                  take it out of the dim state now
    tools/keepawake.py keep --minutes 30     one keep-awake lease, then report
    tools/keepawake.py watch --minutes 30    hold the lease, refreshing and printing
    tools/keepawake.py meter                 log battery current + power state to a CSV

Exit codes: 0 the board is awake, or was dim and became awake; 1 it is past
reach (soft-off, or its ESP in ROM download mode: a hand at the button, or a
reflash); 2 no board or no crosspad-hil to talk through.

Why not simply set REG_CTL_SLEEP_TO_S high: STM_REGW writes the STM's copy and
saves nothing, so the next settings push puts the stored value back -- and a
board that now dims after five seconds because some script poked it is a worse
bench than one that went to sleep honestly. The lease lives on the ESP side and
expires by itself, which is the reversible half. The rule it buys is
main/include/host_keepawake.hpp; the firmware end is `PWR`, `PWR WAKE` and
`PWR KEEPAWAKE <sec>` over CDC.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from shutil import which

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Same shape as tools/bench.py's candidates, with an explicit override read at
# call time (CROSSPAD_HIL) so a caller can point it somewhere after import.
HIL_CANDIDATES = (
    os.path.join(REPO, ".venv", "Scripts", "crosspad-hil.exe"),
    os.path.join(REPO, ".venv", "bin", "crosspad-hil"),
    os.path.join(os.path.dirname(REPO), "crosspad-hil", ".venv", "Scripts", "crosspad-hil.exe"),
    os.path.join(os.path.dirname(REPO), "crosspad-hil", ".venv", "bin", "crosspad-hil"),
    "crosspad-hil",
)

EXIT_OK = 0
EXIT_UNREACHABLE = 1
EXIT_NO_TOOLING = 2

# PWR_STATE_* from crosspad-core/include/crosspad/stm32/reg_map.h.
AWAKE = 0x00
SLEEP_PENDING = 0x01
SHIP_PENDING = 0x02
WOKE = 0x03
LIGHT = 0x04
CHARGE = 0x05

# `key=` -> how to read the value. Unknown keys are skipped, so the firmware can
# add to the line without breaking this.
PWR_FIELDS = {
    "state": lambda s: int(s, 16),
    "cached": lambda s: int(s, 16),
    "link": str,
    "stm_dim": lambda s: int(s.rstrip("s")),
    "stm_sleep": lambda s: int(s.rstrip("s")),
    "stm_auto": int,
    "stored_dim": lambda s: int(s.rstrip("s")),
    "stored_sleep": lambda s: int(s.rstrip("s")),
    "stored_auto": int,
    "lease": lambda s: int(s.rstrip("ms")),
    "host_quiet": lambda s: int(s.rstrip("ms")),
    "cmds": int,
    "relights": int,
    "nudges": int,
    "i2c_ok": int,
    "i2c_fail": int,
    "manual": int,
}


def hil_path():
    """The crosspad-hil entry point, or None. CROSSPAD_HIL wins when it is set."""
    for cand in (os.environ.get("CROSSPAD_HIL", ""),) + HIL_CANDIDATES:
        if not cand:
            continue
        if os.sep in cand or (os.altsep and os.altsep in cand):
            if os.path.exists(cand):
                return cand
        else:
            found = which(cand)
            if found:
                return found
    return None


def run_capture(cmd, timeout=30.0):
    """Run a command, return (returncode, stdout, stderr).

    Pipes are the obvious way; a restricted environment can refuse them, so the
    fallback is a temp file with redirection, which needs nothing but a working
    directory. Both paths mean the same thing to the caller.
    """
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except OSError:
        with tempfile.TemporaryFile("w+") as out, tempfile.TemporaryFile("w+") as err:
            rc = subprocess.call(cmd, stdout=out, stderr=err, timeout=timeout)
            out.seek(0)
            err.seek(0)
            return rc, out.read(), err.read()


def parse_pwr(text):
    """The `PWR:` reply line into a dict; {} when there is none."""
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("PWR:"):
            continue
        info = {}
        for section in line[4:].split("|"):
            prefix = ""
            for token in section.split():
                if "=" not in token:
                    # A bare word is the section's label: `stm` and `stored` use
                    # the same key names for the live and the saved timeouts, so
                    # they are only distinguishable by which side of the `|` they
                    # are on -- and telling those apart is the whole point.
                    prefix = token + "_"
                    continue
                key, _, value = token.partition("=")
                read = PWR_FIELDS.get(prefix + key)
                if read is None:
                    continue
                try:
                    info[prefix + key] = read(value)
                except ValueError:
                    pass
        return info
    return {}


def parse_reg_state(text):
    """`REG 012D: 04` -- the STM's PWR_STATE read straight, for a firmware that
    has no `PWR` verb yet (the verb landed with the keep-awake work)."""
    for line in text.splitlines():
        t = line.strip()
        if not t.startswith("REG 012D:"):
            continue
        # The firmware prints the register and a colon as one token:
        # `REG 012D: 04` -> ["REG", "012D:", "04"].
        parts = t.split()
        for i, part in enumerate(parts):
            if part.upper() == "012D:" and i + 1 < len(parts):
                try:
                    return int(parts[i + 1], 16)
                except ValueError:
                    return None
    return None


def usb_mode_of(devices_json):
    """How the hub classifies the ESP ('default', 'audio', 'bootloader', ...) or None."""
    try:
        devs = json.loads(devices_json).get("devices") or []
        return devs[0].get("usb_mode") if devs else None
    except (ValueError, AttributeError, IndexError):
        return None


def decide(info, mode):
    """(action, why): what the board's answers mean and what to do about it.

    Pure on purpose -- this is the part a person reads at 2 a.m. after a run
    died halfway through, so it is pinned by tests rather than by a reply.
    """
    if info:
        state = info.get("state")
        if state is None:
            state = info.get("cached")
        if state == LIGHT or state == WOKE:
            return "wake", ("dim: the ESP answers and the screen is dark; "
                            "`PWR WAKE` writes REG_CMD_USER_ACTIVITY and the board "
                            "lights back up")
        if state == AWAKE:
            quiet = info.get("host_quiet")
            suffix = "" if quiet is None else " (host quiet %ss)" % quiet
            if info.get("i2c_fail"):
                return "wake", ("awake, but %s of the activity writes failed over I2C -- "
                                "the STM link is failing, so dimming is not being reset"
                                % info["i2c_fail"])
            return "ok", "awake" + suffix
        if state in (SLEEP_PENDING, SHIP_PENDING):
            return "lost", ("the STM is on its way into soft-off / ship mode: the ESP "
                            "restarts into its gate, and after that no host can reach it")
        if state == CHARGE:
            return "ok", "charge-idle: the board is off on a charger and only repowered " \
                         "to show the gauge"
        return "ok", "state 0x%02X" % (state if state is not None else 0xFF)
    if mode == "bootloader":
        return "lost", ("the ESP is in its ROM download mode, so there is no CDC: a tool "
                        "that opened the STM bridge VCP with DTR/RTS asserted does that -- "
                        "the bridge wires those lines to EN and GPIO0. Recovery: flash "
                        "through the download port, or `python tools/bench.py recover`")
    return "lost", ("no CDC console: the ESP is deep-sleeping behind its soft-off gate. "
                    "The STM wakes on its power button or a charger event and nothing a "
                    "host writes asks it to -- a person presses the button once, then "
                    "`keepawake.py wake` works again")


def cdc(base, words, expect=None, timeout=30.0):
    cmd = list(base) + ["cdc"] + list(words)
    if expect:
        cmd += ["--expect", expect]
    rc, out, err = run_capture(cmd, timeout=timeout)
    return rc, out + err


def probe(base):
    """(info, raw reply), sent with an explicit expect prefix on purpose.

    `PWR` is catalogued on a pending crosspad-hil branch (task/cdc-verbs-power-encoder)
    and in no released CLI yet, and the board may be running firmware that predates the
    verb entirely -- so naming the prefix here is the version-independent way to ask,
    and it costs nothing when the catalog does know the verb. The fallback that reads
    `REG_IN_PWR_STATE` straight lives in main(), below.
    """
    rc, text = cdc(base, ["PWR"], expect="PWR:")
    return parse_pwr(text), text


# The POWER block of the STM register map, for `meter`. CrossPad's battery gauge
# is the STM's: REG_PWR_IBAT_* is a signed 16-bit big-endian milliamp reading
# (positive = the battery discharging, negative = charging), REG_PWR_VBAT_* is the
# STM's own direct-ADC millivolts, and REG_PWR_CHG_STATUS says whether VBUS is
# even there. Nothing in the ESP firmware reads IBAT -- the device itself has never
# looked at its own current -- which is why this is a host tool: ask the gauge, log
# it, and the idle-drain question in docs/subsystems/power-button.md gets answered
# by a CSV instead of by a guess.
REG_PWR_STATE = 0x012D          # INPUTS block: PWR_STATE_*
REG_PWR_VBAT_HI = 0x0200        # POWER block starts here
REG_PWR_VBAT_LO = 0x0201
REG_PWR_CHG_STATUS = 0x0207
REG_PWR_IBAT_HI = 0x0229
REG_PWR_IBAT_LO = 0x022A
POWER_BLOCK_LEN = REG_PWR_IBAT_LO - REG_PWR_VBAT_HI + 1   # 0x0200..0x022A, 43: fits STM_REG's len<=64

STATE_NAMES = {0: "awake", 1: "sleep_pending", 2: "ship_pending", 3: "woke", 4: "light", 5: "charge"}


def parse_reg_block(text):
    """`REG 0200: 0E C1 ...` -> {address: value}. {} when the read failed.

    The verb answers one line per call with every byte of the block after the
    address, so one call covers 0x0200..0x022A; `ERR i2c` (the STM link down or
    asleep) and a short reply both give a dict that is simply missing keys, which
    the row builder turns into a gap rather than a zero -- a zero would be a lie
    about the drain.
    """
    for line in text.splitlines():
        t = line.strip()
        if not t.startswith("REG "):
            continue
        parts = t.split()
        if len(parts) < 2:
            continue
        try:
            base = int(parts[1].rstrip(":"), 16)
        except ValueError:
            continue
        regs = {}
        for i, tok in enumerate(parts[2:]):
            try:
                regs[base + i] = int(tok, 16)
            except ValueError:
                break
        return regs
    return {}


def signed16(hi, lo):
    """The gauge's milliamps: two's complement, positive is the battery giving."""
    v = ((hi & 0xFF) << 8) | (lo & 0xFF)
    return v - 0x10000 if v & 0x8000 else v


def meter_row(stamp, state_regs, power_regs):
    state = state_regs.get(REG_PWR_STATE)
    hi, lo = power_regs.get(REG_PWR_IBAT_HI), power_regs.get(REG_PWR_IBAT_LO)
    vhi, vlo = power_regs.get(REG_PWR_VBAT_HI), power_regs.get(REG_PWR_VBAT_LO)
    return {
        "t": stamp,
        "state": state,
        "name": STATE_NAMES.get(state, "unknown"),
        "ma": signed16(hi, lo) if hi is not None and lo is not None else None,
        "mv": ((vhi << 8) | vlo) if vhi is not None and vlo is not None else None,
        "chg": power_regs.get(REG_PWR_CHG_STATUS),
    }


def summarise(rows):
    """Mean/min/max milliamps per power state, and how many samples were gaps.

    Grouping by state is the whole point: the question is what the dim state
    (PWR_STATE_LIGHT) actually costs against awake, and a mean over a run that
    covered both answers neither.
    """
    out = {}
    gaps = 0
    for r in rows:
        if r["ma"] is None:
            gaps += 1
            continue
        g = out.setdefault(r["name"], {"n": 0, "sum": 0, "min": r["ma"], "max": r["ma"], "mv": []})
        g["n"] += 1
        g["sum"] += r["ma"]
        g["min"] = min(g["min"], r["ma"])
        g["max"] = max(g["max"], r["ma"])
        if r["mv"] is not None:
            g["mv"].append(r["mv"])
    for name, g in out.items():
        g["mean"] = g["sum"] / g["n"]
        g["mv"] = (sum(g["mv"]) / len(g["mv"])) if g["mv"] else None
    return out, gaps


def read_sample(base, cdc=cdc):
    """One sample's two register reads. Kept separate so a test can script them."""
    _, a = cdc(base, ["STM_REG", "0x012D"], expect="REG")
    _, b = cdc(base, ["STM_REG", "0x0200", str(POWER_BLOCK_LEN)], expect="REG")
    return parse_reg_block(a), parse_reg_block(b)


def meter_main(base, minutes, interval, out_path, cdc=cdc, clock=time.time, pause=time.sleep,
               now=None):
    """Log current and state to a CSV, then print what the states cost."""
    now = now or time.strftime("%Y%m%d-%H%M%S")
    path = out_path or ("crosspad-meter-%s.csv" % now)
    samples = max(1, int(minutes * 60 // max(1, interval)))
    print("metering %d samples every %g s into %s (Ctrl-C stops it; what is written "
          "stays written)" % (samples, interval, path))
    print("two console reads per sample, so a long run is a slow log -- and let the "
          "board dim by itself while it runs; `wake` in another window is how you "
          "compare the states on purpose")
    rows = []
    last = None
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write("time,pwr_state,state,ibat_ma,vbat_mv,chg_flags\n")
        for _ in range(samples):
            started = clock()
            st, pw = read_sample(base, cdc=cdc)
            row = meter_row(time.strftime("%Y-%m-%dT%H:%M:%S"), st, pw)
            if not st and not pw:
                print("the STM answers nothing: either its link to the ESP is hung "
                      "(`STM_RESET`, then a power cycle if that fails) or the board "
                      "is in soft-off, where the ESP itself is asleep. Metering stops.")
                return EXIT_UNREACHABLE
            rows.append(row)
            fh.write("%s,%s,%s,%s,%s,%s\n" % (row["t"],
                                              ("%02X" % row["state"]) if row["state"] is not None else "",
                                              row["name"],
                                              "" if row["ma"] is None else row["ma"],
                                              "" if row["mv"] is None else row["mv"],
                                              ("%02X" % row["chg"]) if row["chg"] is not None else ""))
            fh.flush()
            if last is not None and row["state"] != last:
                print("  %s: %s -> %s" % (row["t"], STATE_NAMES.get(last, "?"), row["name"]))
            last = row["state"]
            wait = interval - (clock() - started)
            if wait > 0:
                pause(wait)
    groups, gaps = summarise(rows)
    print("\n%s samples, %d of them gaps (the gauge answered nothing)" % (len(rows), gaps))
    for name in sorted(groups, key=lambda k: -groups[k]["mean"]):
        g = groups[name]
        mv = "" if g["mv"] is None else "  at %d mV" % round(g["mv"])
        print("  %-14s n=%-4d mean %+7.1f mA   min %+7d  max %+7d%s"
              % (name, g["n"], g["mean"], g["min"], g["max"], mv))
    if "light" in groups and "awake" in groups:
        d = groups["awake"]["mean"] - groups["light"]["mean"]
        print("  awake minus dim: %+0.1f mA -- that is what the dim state saves, if "
              "anything. On a 4000 mAh cell 100 mA is about 40 h, so a small number "
              "here is still the number that matters for a set." % d)
    print("wrote %s" % path)
    return EXIT_OK


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-d", "--device", default=None, help="passed through to crosspad-hil")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("status", help="print the PWR line and a verdict")
    sub.add_parser("wake", help="ask the STM for activity now")
    keep = sub.add_parser("keep", help="take one keep-awake lease")
    keep.add_argument("--minutes", type=int, default=30)
    watch = sub.add_parser("watch", help="hold a lease down, refreshing and reporting")
    watch.add_argument("--minutes", type=int, default=30)
    meter = sub.add_parser("meter", help="log the STM battery gauge and power state to a CSV")
    meter.add_argument("--minutes", type=int, default=20)
    meter.add_argument("--interval", type=float, default=10.0,
                       help="seconds between samples (default 10; each sample is two console reads)")
    meter.add_argument("-o", "--out", default=None, help="CSV path")
    args = ap.parse_args(argv)

    hil = hil_path()
    if not hil:
        print("crosspad-hil not found: install it, or set CROSSPAD_HIL to its entry point")
        return EXIT_NO_TOOLING
    base = [hil] + (["-d", args.device] if args.device else [])

    if args.mode == "meter":
        return meter_main(base, args.minutes, args.interval, args.out)

    info, raw = probe(base)

    if not info:
        # No `PWR` reply: either this firmware predates the verb, or the console
        # is gone. Ask for the state the way the older boards answer it before
        # concluding anything -- a board that answers MEM but not PWR is awake,
        # not asleep, and saying otherwise would send someone to the button.
        _, txt = cdc(base, ["STM_REG", "0x012D"], expect="REG")
        state = parse_reg_state(txt)
        if state is not None:
            info = {"state": state, "link": "ok", "legacy": True}
            raw = ""
            print("no `PWR` verb in this firmware (it predates the keep-awake work); "
                  "STM state read directly: 0x%02X" % state)
        else:
            # Distinguish "no console" (deep sleep, a hand needed) from "console
            # there, STM gone quiet" (the shared-bus hang: an ESP that restarted
            # mid-transaction loses the bus for the rest of the session). They
            # look the same to a caller that only asks for registers.
            _, mem = cdc(base, ["MEM"], expect="MEM:")
            if "MEM:" in mem:
                print("the console answers and the ESP is up, but every STM register "
                      "reads ERR i2c: that is the shared-bus hang, not sleep. `STM_RESET` "
                      "over CDC pulses the ESP's reset line for the STM; if the bus stays "
                      "down the board needs a power cycle (unplug USB, and hold the power "
                      "button 4.2 s on battery for ship mode)")
                return EXIT_UNREACHABLE
    mode = None if info else usb_mode_of(run_capture(base + ["devices", "--json"])[1])
    action, why = decide(info, mode)
    if info and raw:
        print([ln for ln in raw.splitlines() if ln.strip().startswith("PWR:")][0].strip())
    if info and info.get("stm_dim") != info.get("stored_dim"):
        print("NOTE: the STM is running dim=%ss sleep=%ss while the stored settings say "
              "dim=%ss sleep=%ss -- someone poked the registers and nothing saved it"
              % (info.get("stm_dim"), info.get("stm_sleep"),
                 info.get("stored_dim"), info.get("stored_sleep")))
    print("%s: %s" % (action, why))

    if args.mode == "status":
        return EXIT_OK if action in ("ok", "wake") else EXIT_UNREACHABLE
    if action == "lost":
        return EXIT_UNREACHABLE

    minutes = max(1, args.minutes)
    if args.mode == "wake":
        _, text = cdc(base, ["PWR", "WAKE"], expect="PWRW:")
        print(text.strip() or "no reply to PWR WAKE")
        if "ERR i2c" in text:
            print("the ESP is up but its I2C link to the STM is down: that is the bus "
                  "hang, not sleep. `STM_RESET` over CDC pulses the ESP's reset line for "
                  "the STM; if the bus stays down the board needs a power cycle")
            return EXIT_UNREACHABLE
        info2, raw2 = probe(base)
        action2, why2 = decide(info2, None)
        print("%s: %s" % (action2, why2))
        return EXIT_OK if action2 in ("ok", "wake") or info2.get("state") == AWAKE \
            else EXIT_UNREACHABLE

    if args.mode == "keep":
        _, text = cdc(base, ["PWR", "KEEPAWAKE", str(minutes * 60)], expect="PWRKA:")
        print(text.strip() or "no reply to PWR KEEPAWAKE")
        return EXIT_OK

    # watch: refresh the lease before it lapses, and keep reporting where the board is.
    step = min(600, max(60, minutes * 60 // 8))
    print("watching %d min, lease refreshed and state read every %d s (Ctrl-C stops)"
          % (minutes, step))
    until = time.time() + minutes * 60
    while time.time() < until:
        cdc(base, ["PWR", "KEEPAWAKE", str(minutes * 60)], expect="PWRKA:")
        info, _ = probe(base)
        action, why = decide(info, None)
        print(why)
        if action == "lost":
            return EXIT_UNREACHABLE
        time.sleep(step)
        if time.time() >= until:
            break
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
