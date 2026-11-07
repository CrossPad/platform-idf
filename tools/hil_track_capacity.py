#!/usr/bin/env python3
"""How many tracks a song can hold before it breaks up: a song built one
track at a time, played at every count with fingers on the pads, judged by
what a listener would hear.

  audio  every track an audio track, each a different recording laid over the
         whole song -- the card's worst case: one stream per track.
  mixed  what a song is made of: a Drums take on the pads' kit (armed, played
         live), a second kit's track and a Pitch track (frozen once ARM is
         elsewhere, as Arrange freezes them), and audio tracks after them.

At each count the song plays --seconds looped (16 bars, 120 BPM) while chords
of 3-6 pads land every half second on the armed track. A count is safe when
nothing broke: no song ring, sample engine or second-kit underrun, no voice
refused a ring, no unpaced render pass, no render pass over its block on
average. Also reported: the render's worst pass, the least the song ring held,
the longest card read, PSRAM free and its largest block.

The songs (CAP_AUDIO, CAP_MIXED) stay on the card; the song and kit open
before are put back, the speakers muted while it runs (--listen leaves them).

  python tools/hil_track_capacity.py                 # both mixes, 8 tracks
  python tools/hil_track_capacity.py --mix audio --seconds 45
"""
from __future__ import annotations

import argparse
import json
import random
import sys
import time
from pathlib import Path

from hil_perf_song import (BLOCK_US, DRUMS_PATTERN, SLICE_PATTERN, PITCH_PATTERN, BenchError, Board, asong, current_kit,
                           fields, kit_ids, kit_track, load_kit, num, record_track, song_name, wait_song_ready)
from hil_stress_song import reset_log_head

BARS = 16
# A different recording per track, with its size: 16-bit stereo at 44.1 kHz.
WAVS = {"rec_062": 2432556, "rec_060": 1974572, "rec_067": 3489068, "rec_063": 1557804,
        "rec_006": 1882924, "rec_008": 1848620, "rec_007": 1432108, "rec_066": 1091884}
FRAMES_PER_BAR = 44100 * 2                     # 120 BPM, 4/4
REC_DIR = "/sdcard/crosspad/recordings"
DRUMS = "POP_ICONIC"
SLICE_KIT = "/crosspad/slicer/PERF_SLICE/kit.json"
INSTRUMENT = "/crosspad/pitchplay/BASS_Reese/instrument.json"


def rows(b: Board, verb: str, last: str, secs: float = 2.0) -> list[str]:
    assert b.s
    b.s.reset_input_buffer()
    b.s.write((verb + "\r\n").encode())
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        buf += b.s.read(4096)
        i = buf.find(last.encode())
        if i >= 0 and b"\n" in buf[i:]:
            break
    return [l.strip() for l in buf.decode(errors="replace").splitlines() if l.strip()]


def tracks(b: Board) -> list[dict[str, str]]:
    return [fields(r) for r in rows(b, "ASONG_TRK", "ATRK: used=") if r.startswith("ATRK: t=")]


def fresh(b: Board, name: str) -> None:
    b.cmd(f"ASONG OPEN {name}", "ASONG:")
    wait_song_ready(b, name, 30)
    if asong(b).get("clips", "0/0/0/0").strip("/0") or asong(b).get("len", "0") != "0":
        b.cmd("ASONG DELETE", "ASONG:")
        time.sleep(3)
        b.cmd(f"ASONG OPEN {name}", "ASONG:")
        wait_song_ready(b, name, 30)
    b.cmd("ASONG TEMPO 120", "ASONG:")
    time.sleep(1)
    b.cmd(f"ASONG LEN {BARS}", "ASONG:")
    b.cmd(f"ASONG REGION 0 {BARS}", "ASONG:")


def audio_track(b: Board, t: int, wav: str) -> None:
    """@p wav on track @p t from bar one, copied after itself to the song's end."""
    if fields(b.cmd(f"ASONG IMPORT {REC_DIR}/{wav}.wav 0 0 {t} 0 0", "ASONG:")).get("ok") != "1":
        raise BenchError(f"import of {wav} refused")
    end = time.time() + 120
    while time.time() < end:
        tr = {r["t"]: r for r in tracks(b)}.get(str(t))
        if tr and int(num(tr.get("blocks"))) > 0 and asong(b).get("edit") in ("0", "2"):
            break
        time.sleep(0.5)
    else:
        raise BenchError(f"import of {wav} did not land on track {t + 1}")
    frames = (WAVS[wav] - 44) // 4
    bars = -(-frames // FRAMES_PER_BAR)
    for blk in range(-(-BARS // bars) - 1):
        b.cmd(f"ASONG DUP {t} {blk}", "ASONG:")
    b.cmd(f"ASONG LEN {BARS}", "ASONG:")


def freeze_all_but(b: Board, armed: int) -> list[str]:
    """Every seq track but @p armed frozen, as Arrange leaves them."""
    notes = []
    b.cmd(f"ASONG ARM {armed}", "ASONG:")
    for r in tracks(b):
        t = int(r["t"])
        if r.get("kind") != "s" or t == armed or r.get("fz") == "1":
            continue
        end = time.time() + 180
        while time.time() < end:
            if asong(b).get("bounce") != "1":
                b.cmd(f"ASONG FREEZE {t}", "ASONG:")
            time.sleep(2)
            if {x["t"]: x for x in tracks(b)}.get(str(t), {}).get("fz") == "1" and asong(b).get("bounce") != "1":
                break
        else:
            notes.append(f"track {t + 1} not frozen")
    return notes


def counters(b: Board) -> dict[str, float]:
    eng = fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5))
    peak = fields(b.cmd("SMPL_PEAK", "SMPLPEAK:"))
    return {"song_under": num(asong(b).get("under")), "eng_under": num(eng.get("underrun")),
            "norings": num(eng.get("norings")),
            "overruns": num(peak.get("overruns")), "unpaced": num(peak.get("unpaced"))}


def measure(b: Board, seconds: float, rng: random.Random) -> dict:
    b.cmd("RT_PROF 1", "OK")
    b.cmd("RT_PROF", "RTPROF:")
    b.cmd("ASONG_DBG", "ASONGDBG:")                      # clears its maxima
    before = counters(b)
    b.cmd("ASONG PLAY 0", "ASONG:")
    t_end = time.time() + seconds
    avg_max = worst = 0.0
    ring_min, read_max, gap_max = 1 << 30, 0.0, 0.0
    next_poll = time.time() + 2
    while time.time() < t_end:
        pads = rng.sample(range(16), rng.randint(3, 6))
        b.s.write("".join(f"PAD_PRESS {p} {rng.randint(60, 127)}\r\n" for p in pads).encode())
        time.sleep(rng.uniform(0.1, 0.3))
        b.s.write("".join(f"PAD_RELEASE {p}\r\n" for p in pads).encode())
        time.sleep(rng.uniform(0.2, 0.4))
        if time.time() >= next_poll:
            next_poll = time.time() + 2
            rt_raw = b.cmd("RT_PROF", "RTPROF:")
            rt = fields(rt_raw.splitlines()[0]) if rt_raw else {}
            avg_max = max(avg_max, num(rt.get("total_us")))
            worst = max(worst, num(rt.get("worst_us")))
            dbg = fields(b.cmd("ASONG_DBG", "ASONGDBG:"))
            if "ringmin" in dbg and num(dbg.get("ringmin")) > 0:
                ring_min = min(ring_min, num(dbg.get("ringmin")))
            read_max = max(read_max, num(dbg.get("read_max_us")))
            gap_max = max(gap_max, num(dbg.get("gap_max_us")))
    playing = asong(b).get("play") == "1"
    b.cmd("ASONG STOP", "ASONG:")
    after = counters(b)
    mem = fields(b.cmd("MEM", "MEM:"))
    d = {k: int(after[k] - before[k]) if after[k] >= before[k] else int(after[k]) for k in before}
    d["unpaced"] = int(after["unpaced"])
    return {**d, "playing": playing, "render_avg_us": round(avg_max), "render_worst_us": round(worst),
            "ring_min": None if ring_min == 1 << 30 else int(ring_min), "read_max_ms": round(read_max / 1000, 1),
            "gap_max_ms": round(gap_max / 1000, 1), "psram_kb": int(num(mem.get("psram_free")) // 1024),
            "psram_largest_kb": int(num(mem.get("psram_largest")) // 1024)}


def broken(r: dict) -> list[str]:
    bad = [f"{k}+{r[k]}" for k in ("song_under", "eng_under", "norings") if r[k]]
    if r["unpaced"]:
        bad.append(f"unpaced={r['unpaced']}")
    if r["render_avg_us"] > BLOCK_US:
        bad.append(f"render {r['render_avg_us']} us > block")
    if not r["playing"]:
        bad.append("stopped")
    return bad


def build_and_measure(b: Board, mix: str, args, rng: random.Random, ids: dict[str, int]) -> list[dict]:
    name = "CAP_AUDIO" if mix == "audio" else "CAP_MIXED"
    print(f"\n== {mix} ({name}) ==", flush=True)
    fresh(b, name)
    results = []
    plan: list[str] = (["audio"] * args.tracks if mix == "audio"
                       else (["drums", "kit2", "pitch"] + ["audio"] * args.tracks)[:args.tracks])
    wav_i = 0
    for n, kind in enumerate(plan, start=1):
        t = n - 1
        if kind == "audio":
            audio_track(b, t, list(WAVS)[wav_i % len(WAVS)])
            wav_i += 1
        elif kind == "drums":
            load_kit(b, ids[DRUMS], timeout_s=40)
            b.cmd("ASONG SEQTRACK", "ASONG:")
            record_track(b, DRUMS_PATTERN, 2)
            b.cmd(f"ASONG DUP {t} 0", "ASONG:")       # the take round the song: blocks to bar 16
            for blk in range(1, BARS // 2 - 1):
                b.cmd(f"ASONG DUP {t} {blk}", "ASONG:")
        elif kind == "kit2":
            kit_track(b, SLICE_KIT)
            record_track(b, SLICE_PATTERN, 1)
            for blk in range(BARS - 1):
                b.cmd(f"ASONG DUP {t} {blk}", "ASONG:")
        elif kind == "pitch":
            b.cmd(f"ASONG PITCHTRACK {INSTRUMENT}", "ASONG:")
            end = time.time() + 60
            while time.time() < end and fields(b.cmd("INST_STATUS", "INST:")).get("state") not in ("ready", "failed"):
                time.sleep(0.5)
            record_track(b, PITCH_PATTERN, 1)
            for blk in range(BARS - 1):
                b.cmd(f"ASONG DUP {t} {blk}", "ASONG:")
        b.cmd(f"ASONG LEN {BARS}", "ASONG:")
        notes = freeze_all_but(b, 0) if mix == "mixed" else []
        time.sleep(5)                                  # the stream pool settles
        r = measure(b, args.seconds, rng)
        r.update({"tracks": n, "added": kind, "notes": notes, "bad": broken(r)})
        results.append(r)
        print(f"{n} tracks (+{kind:5}): {'OK  ' if not r['bad'] else 'FAIL'} "
              f"song_under={r['song_under']} eng_under={r['eng_under']} "
              f"norings={r['norings']} overruns={r['overruns']} render avg/worst={r['render_avg_us']}/"
              f"{r['render_worst_us']} us ring_min={r['ring_min']} read_max={r['read_max_ms']} ms "
              f"psram={r['psram_kb']}/{r['psram_largest_kb']} kB {' '.join(notes)}", flush=True)
        if reset_log_head(b) != args.rst0:
            raise BenchError("the board reset")
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mix", choices=("audio", "mixed", "both"), default="both")
    ap.add_argument("--tracks", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--listen", action="store_true")
    args = ap.parse_args()
    rng = random.Random(7)
    out: dict[str, list[dict]] = {}
    try:
        b = Board()
        if fields(b.cmd("UI_STATE", "UI:")).get("app") not in ("Arrange", "-", ""):
            raise BenchError("another app is open: the board is in use")
        song0, kit0 = song_name(b), current_kit(b)
        args.rst0 = reset_log_head(b)
        ids = kit_ids(b)
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        try:
            b.cmd("APP_START Arrange", "OK")
            time.sleep(2)
            for mix in (("audio", "mixed") if args.mix == "both" else (args.mix,)):
                out[mix] = build_and_measure(b, mix, args, rng, ids)
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
    print()
    for mix, rs in out.items():
        safe = 0
        for r in rs:
            if r["bad"]:
                break
            safe = r["tracks"]
        print(f"{mix}: safe up to {safe} tracks" + (f" (breaks at {safe + 1}: {', '.join(rs[safe]['bad'])})"
                                                    if safe < len(rs) else ""))
    if args.json:
        args.json.write_text(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
