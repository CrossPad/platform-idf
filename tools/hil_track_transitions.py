#!/usr/bin/env python3
"""What a full song survives while it plays: CAP_MIXED (hil_track_capacity.py,
eight tracks) looped with pads struck from the STM (STM_VPAD, the physical pads' path), and
each of the changes a player makes mid-song done --reps times. After each kind, what broke: song ring,
sample engine and second-kit underruns, voices refused a ring, render passes
over their block, the song stopping; the least the song ring held, the longest
the song's worker was off it and its longest card read; the card's
transactions by task (SD_TRACE).

  kit_swap    the pads' kit loaded anew (KIT_LOAD) -- route 0
  arm_kit2    ARM moved to the second kit track and back (the kits' engine handed over)
  trackkit    the second kit track given another kit (TRACKKIT)
  edit        a block copied, moved, deleted (DUP/MOVE/DELBLOCK) on an audio track
  punch       a DUB punched in and out on the armed track

Exit 0 nothing broke, 1 something did, 2 the bench (no CAP_MIXED).

  python tools/hil_track_transitions.py --reps 5
  python tools/hil_track_transitions.py --tracks 5     # the same with three audio tracks muted
"""
from __future__ import annotations

import argparse
import sys
import time

from hil_perf_song import BenchError, Board, asong, current_kit, fields, kit_ids, load_kit, num, song_name, wait_song_ready
from hil_track_capacity import counters, tracks

KITS = ["POP_ICONIC", "808_MACHINE_1", "909_MACHINE_1"]
DRUM_PATHS = ["/crosspad/drums/808_MACHINE_1/kit.json", "/crosspad/drums/POP_ICONIC/kit.json"]
HITS_PER_S = 8
REP_S = 5.0                   # each change, then this long played over


def under_at(dbg_line: str) -> int:
    """The song's underruns since boot, by what its worker was doing (ASONG_DBG)."""
    return sum(int(x) for x in fields(dbg_line).get("under_at", "0").split("/") if x.isdigit())


def fingers(b: Board, seconds: float) -> None:
    """Pad hits for @p seconds from the STM, the physical pads' own path: hits
    sent over CDC queue behind a busy core 0 and arrive late, in a burst."""
    b.cmd(f"STM_VPAD {HITS_PER_S} {int(seconds * HITS_PER_S)} 120", "VPAD:")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--only", help="one kind")
    ap.add_argument("--tracks", type=int, default=8,
                    help="tracks playing: the audio tracks past this many muted (a muted track is not read)")
    ap.add_argument("--listen", action="store_true")
    args = ap.parse_args()
    broke = False
    try:
        b = Board()
        if fields(b.cmd("UI_STATE", "UI:")).get("app") not in ("Arrange", "-", ""):
            raise BenchError("another app is open: the board is in use")
        song0, kit0 = song_name(b), current_kit(b)
        ids = kit_ids(b)
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        try:
            b.cmd("APP_START Arrange", "OK")
            time.sleep(2)
            b.cmd("ASONG OPEN CAP_MIXED", "ASONG:")
            wait_song_ready(b, "CAP_MIXED", 30)
            trk = tracks(b)
            if len(trk) < 8:
                raise BenchError("CAP_MIXED has fewer than 8 tracks: run hil_track_capacity.py --mix mixed")
            kit2_track = next(int(r["t"]) for r in trk if r.get("route") == "0" and r.get("live") == "-1")
            audio = [int(r["t"]) for r in trk if r.get("kind") == "a"]
            for t in audio:
                b.cmd(f"ASONG MUTE {t} {1 if t >= args.tracks else 0}", "ASONG:")
            audio = [t for t in audio if t < args.tracks]
            b.cmd("ASONG ARM 0", "ASONG:")
            time.sleep(8)                                  # freezes settle
            kinds = ["kit_swap", "arm_kit2", "trackkit", "edit", "punch"]
            for kind in ([args.only] if args.only else kinds):
                before = counters(b)
                at0 = under_at(b.cmd("ASONG_DBG", "ASONGDBG:"))   # also clears its maxima
                b.cmd("SD_TRACE", "SDTRACE:", 3)
                b.cmd("ASONG PLAY 0", "ASONG:")
                time.sleep(2)
                fingers(b, args.reps * REP_S + 2)
                stopped = 0
                for rep in range(args.reps):
                    if kind == "kit_swap":
                        b.cmd(f"KIT_LOAD {ids[KITS[rep % len(KITS)]]}", "OK")
                    elif kind == "arm_kit2":
                        b.cmd(f"ASONG ARM {kit2_track}", "ASONG:")
                        time.sleep(2)
                        b.cmd("ASONG ARM 0", "ASONG:")
                    elif kind == "trackkit":
                        b.cmd(f"ASONG TRACKKIT {kit2_track} {DRUM_PATHS[rep % 2]}", "ASONG:")
                    elif kind == "edit":
                        t = audio[rep % len(audio)]
                        b.cmd(f"ASONG DUP {t} 0", "ASONG:")
                        b.cmd(f"ASONG MOVE {t} 1 {2 + 3 * rep}", "ASONG:")
                        b.cmd(f"ASONG DELBLOCK {t} 1", "ASONG:")
                    elif kind == "punch":
                        b.cmd("ASONG PUNCH D", "ASONG:")
                        time.sleep(2)
                        b.cmd("ASONG PUNCH N", "ASONG:")
                    time.sleep(REP_S - 2 if kind in ("arm_kit2", "punch") else REP_S)
                    if asong(b).get("play") != "1":
                        stopped += 1
                        b.cmd("ASONG PLAY 0", "ASONG:")
                        time.sleep(1)
                dbg_line = b.cmd("ASONG_DBG", "ASONGDBG:")
                dbg = fields(dbg_line)
                sd = b.cmd("SD_TRACE", "SDTRACE:", 3)
                b.cmd("STM_VPAD STOP", "")
                b.cmd("ASONG STOP", "ASONG:")
                after = counters(b)
                d = {k: int(after[k] - before[k]) if after[k] >= before[k] else int(after[k]) for k in before}
                d["song_under"] = under_at(dbg_line) - at0      # ASONG's own count starts again with every PLAY
                bad = [f"{k}+{d[k]}" for k in ("song_under", "eng_under", "norings") if d[k]]
                if stopped:
                    bad.append(f"stopped {stopped}x")
                broke |= bool(bad)
                mem = fields(b.cmd("MEM", "MEM:"))
                print(f"{kind:9} x{args.reps}: {'OK  ' if not bad else 'FAIL'} {' '.join(bad)}  overruns={d['overruns']} "
                      f"ring_min={dbg.get('ringmin')}/{dbg.get('ringcap')} gap_max={int(num(dbg.get('gap_max_us')) // 1000)} ms "
                      f"read_max={int(num(dbg.get('read_max_us')) // 1000)} ms "
                      f"fill_max={int(num(dbg.get('fill_max_us')) // 1000)} ms bg_max={int(num(dbg.get('bg_max_us')) // 1000)} ms "
                      f"open_max={int(num(dbg.get('open_max_us')) // 1000)} ms "
                      f"under_at(wait/fill/read/open/yield/bg/take/other)={dbg.get('under_at')} "
                      f"psram={int(num(mem.get('psram_free')) // 1024)}/{int(num(mem.get('psram_largest')) // 1024)} kB",
                      flush=True)
                head = fields(sd.split("|")[0])
                print(f"      slowest read {head.get('slow_rd')} us, slowest write {head.get('slow_wr')} us, "
                      f"cache hits {head.get('hits')} misses {head.get('misses')}", flush=True)
                for part in sd.split("|")[1:]:
                    f = fields(part)
                    print(f"      {part.split()[0]} {part.split()[1]:8} calls={f.get('calls')} avg_us={f.get('avg_us')} "
                          f"max_us={f.get('max_us')} busy_ms={int(num(f.get('busy_us')) // 1000)}", flush=True)
                time.sleep(6)
        finally:
            b.cmd("ASONG STOP", "ASONG:")
            for t in range(8):
                b.cmd(f"ASONG MUTE {t} 0", "ASONG:")
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
    return 1 if broke else 0


if __name__ == "__main__":
    sys.exit(main())
