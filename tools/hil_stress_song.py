#!/usr/bin/env python3
"""Stress the four-source song: PERF_ALL (Drums + Slice + Pitch + audio, made
by hil_perf_song.py) played while the board is pushed around it.

Every round:
  1. PERF_ALL opens and plays; its Pitch instrument loads beside the kits.
  2. While it plays, the pads' kit is swapped back and forth (--swaps): each
     swap is a native take, whose arena reset must leave the Pitch bank
     playing. Bursts of finger hits land on the pads between swaps.
  3. It stops, and Song (no Pitch track) opens: the bank must go.
After every round: the heap's integrity (HEAP_CHECK), no reset and no core
dump since the start, no engine or ring underrun, no unpaced pass, the bank
present while PERF_ALL played, PSRAM at Song within --leak-kb of round one.

Exit 0 passed, 1 the firmware failed a check, 2 the bench (no board, no
PERF_ALL: run hil_perf_song.py --only C first).

  python tools/hil_stress_song.py --rounds 10
"""
from __future__ import annotations

import argparse
import re
import sys
import time

from hil_perf_song import BenchError, Board, asong, current_kit, fields, kit_ids, load_kit, num, song_name, wait_song_ready


def reset_log_head(b: Board) -> tuple[int, int]:
    """(newest reset's number, core dump sequence). RESET_LOG reads flash and
    can take ~0.9 s while the card and the song are busy: a reply missed by
    a short wait read as a reset (hil_torture_song seed 21, step 157)."""
    text = ""
    for _ in range(3):
        text = b.cmd("RESET_LOG", "RSTLOG:", 3.0)
        if text:
            break
    cd = int(num(fields(text.splitlines()[0]).get("coredump_seq"))) if text else -1
    m = re.search(r"#(\d+) ", text)
    return (int(m.group(1)) if m else -1, cd)


def wait_inst(b: Board, beside: bool, timeout_s: float = 60.0) -> dict[str, str]:
    end = time.time() + timeout_s
    st: dict[str, str] = {}
    while time.time() < end:
        st = fields(b.cmd("INST_STATUS", "INST:"))
        if beside and st.get("state") == "ready" and st.get("beside") == "1":
            return st
        if not beside and st.get("beside") == "0":
            return st
        time.sleep(0.5)
    return st


def open_song(b: Board, name: str) -> None:
    b.cmd(f"ASONG OPEN {name}", "ASONG:")
    wait_song_ready(b, name)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--swaps", type=int, default=4, help="pads' kit swaps while PERF_ALL plays, per round")
    ap.add_argument("--kits", default="POP_ICONIC,808_MACHINE_1", help="the two kits swapped on route 0")
    ap.add_argument("--leak-kb", type=int, default=96)
    ap.add_argument("--listen", action="store_true",
                    help="leave the speakers on: someone is listening to the run")
    args = ap.parse_args()

    failures: list[str] = []
    try:
        b = Board()
        song0 = song_name(b)
        kit0 = current_kit(b)
        ids = kit_ids(b)
        names = args.kits.split(",")
        for n in names:
            if n not in ids:
                raise BenchError(f"no kit called {n}")
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        b.cmd("APP_START Arrange", "OK")
        time.sleep(2)
        rst0 = reset_log_head(b)
        base_psram = None
        under0 = int(num(fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5)).get("underrun")))
        song_under0 = int(num(asong(b).get("under")))
        try:
            for rnd in range(1, args.rounds + 1):
                t0 = time.time()
                open_song(b, "PERF_ALL")
                inst = wait_inst(b, beside=True)
                if inst.get("beside") != "1":
                    failures.append(f"round {rnd}: the instrument did not load beside the kits ({inst})")
                b.cmd("ASONG PLAY 0", "ASONG:")
                time.sleep(2)
                bank_gone = 0
                for s in range(args.swaps):
                    load_kit(b, ids[names[s % len(names)]])
                    for pad in (0, 1, 2, 3, 0, 1, 2, 3):           # a finger burst
                        b.cmd(f"PAD_PRESS {pad}", "OK", 0.05)
                        b.cmd(f"PAD_RELEASE {pad}", "OK", 0.03)
                    p = fields(b.cmd("PITCHED_STATUS", "PITCHED:"))
                    if p.get("active") != "1" or num(p.get("bank_kb")) <= 0:
                        bank_gone += 1
                    if asong(b).get("play") != "1":
                        failures.append(f"round {rnd}: the song stopped during swap {s + 1}")
                        b.cmd("ASONG PLAY 0", "ASONG:")
                    time.sleep(1.5)
                if bank_gone:
                    failures.append(f"round {rnd}: the Pitch bank was gone after {bank_gone} of {args.swaps} kit swaps")
                peak = fields(b.cmd("SMPL_PEAK", "SMPLPEAK:"))
                b.cmd("ASONG STOP", "ASONG:")
                open_song(b, "Song")
                inst = wait_inst(b, beside=False)
                if inst.get("beside") != "0":
                    failures.append(f"round {rnd}: the beside bank stayed with no Pitch track ({inst})")
                # The stream pool gives its pages back after 4 s of quiet (engine_arena).
                time.sleep(6)
                heap = fields(b.cmd("HEAP_CHECK", "HEAPCHK:"))
                mem = fields(b.cmd("MEM", "MEM:"))
                psram = int(num(mem.get("psram_free")) // 1024)
                base_psram = psram if base_psram is None else base_psram
                rst = reset_log_head(b)
                eng = fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5))
                song = asong(b)
                line = (f"round {rnd:2}: {time.time() - t0:5.1f} s  heap_ok={heap.get('ok')}  psram@Song={psram} kB "
                        f"(drift {psram - base_psram:+d})  int_min={mem.get('int_min')}  unpaced={peak.get('unpaced')} "
                        f"overruns={peak.get('overruns')}  eng_under={int(num(eng.get('underrun'))) - under0} "
                        f"song_under={int(num(song.get('under'))) - song_under0}")
                print(line, flush=True)
                if heap.get("ok") != "1":
                    failures.append(f"round {rnd}: HEAP_CHECK failed")
                if rst != rst0:
                    failures.append(f"round {rnd}: the board reset or dumped core (log {rst0} -> {rst})")
                    break
                if int(num(peak.get("unpaced"))):
                    failures.append(f"round {rnd}: unpaced render passes")
                if int(num(eng.get("underrun"))) - under0:
                    failures.append(f"round {rnd}: sample engine underruns")
                if int(num(song.get("under"))) - song_under0:
                    failures.append(f"round {rnd}: song ring underruns")
                if base_psram - psram > args.leak_kb:
                    failures.append(f"round {rnd}: PSRAM at Song {base_psram - psram} kB below round one")
        finally:
            b.cmd("ASONG STOP", "ASONG:")
            if song0:
                b.cmd(f"ASONG OPEN {song0}", "ASONG:")
            if kit0 >= 0:
                b.cmd(f"KIT_LOAD {kit0}", "OK")
            if not args.listen:
                b.cmd("AUDIO_MUTE 0 0", "OK")
                b.cmd("AUDIO_MUTE 1 0", "OK")
    except BenchError as e:
        print(f"BENCH: {e}")
        return 2

    print("PASS" if not failures else "FAIL:\n  " + "\n  ".join(dict.fromkeys(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
