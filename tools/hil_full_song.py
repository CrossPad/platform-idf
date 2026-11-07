#!/usr/bin/env python3
"""A full arrangement, end to end: four different tracks over 64 bars at
120 BPM (2 min 8 s), played through live, every seq track frozen by moving
ARM (Arrange's auto-freeze) and played through again, then every seq track
bounced and played through as audio.

  PERF_FULL  track 1 Drums  -- a 2-bar take going round the whole song
             track 2 Slice  -- the second kit, a 1-bar take, the whole song
             track 3 Pitch  -- the instrument beside the kits, the whole song
             track 4 audio  -- a recording laid every 8 bars, bars 1..64

Judged, as hil_perf_song.py judges a run (no unpaced pass, no engine or song
ring underrun, no voice refused a ring), plus: every seq block covering the
64 bars, each bounce Done with exactly 64 bars of frames, the bounced track
turned to audio, the heap intact (HEAP_CHECK) and no reset or core dump from
start to end. Render passes over their block are reported, not failed: the
four-source song shows a few a minute that the I2S DMA absorbs.

Fixtures from hil_perf_song.py (PERF_SLICE, HIL PITCHED); the song stays on
the card. The song and kit open before are put back. --listen leaves the
speakers on.

  python tools/hil_full_song.py
"""
from __future__ import annotations

import argparse
import sys
import time

from hil_perf_song import (DRUMS_PATTERN, PITCH_PATTERN, PITCHED_PATH, SLICE_PATH, SLICE_PATTERN, BenchError,
                           Board, asong, current_kit, ensure_pitched_kit, ensure_slice_kit, fields, fresh_song, song_name,
                           import_audio, kit_ids, kit_track, load_kit, num, play_and_measure, record_track)
from hil_stress_song import reset_log_head, wait_inst

BARS = 64
BAR_FRAMES = 88200                     # 120 BPM, 4/4, 44.1 kHz
SONG_S = BARS * 2.0


def blocks(b: Board, t: int) -> list[dict[str, str]]:
    return [fields(l) for l in b.cmd(f"ASONG_SEQ {t}", "ASEQ:").splitlines() if "bar=" in l]


def covers_song(bl: list[dict[str, str]]) -> bool:
    end = max((int(num(x.get("bar"))) + int(num(x.get("bars"))) for x in bl), default=0)
    return bool(bl) and int(num(bl[0].get("bar"))) == 0 and end >= BARS


def bounce(b: Board, track: int, failures: list[str]) -> None:
    """Arrange's BOUNCE of @p track: rendered as a freeze is (core 2.14) --
    renders already on the card taken -- and turned into audio."""
    t0 = time.time()
    if "ERR" in b.cmd(f"ASONG BOUNCESEQ {track}", "", 1.0):
        failures.append(f"bounce of track {track + 1} refused")
        return
    st: dict[str, str] = {}
    while time.time() - t0 < SONG_S + 90:
        st = asong(b)
        if st.get("bounce") != "1" and time.time() - t0 > 1.0:
            break
        time.sleep(0.25)
    took = time.time() - t0
    clips = st.get("clips", "0/0/0/0").split("/")
    print(f"  bounce track {track + 1}: state={st.get('bounce')} pieces={st.get('pieces')} in {took:.1f} s "
          f"file={st.get('file')} clips={st.get('clips')}")
    if st.get("bounce") != "2":
        failures.append(f"bounce of track {track + 1} not done ({st.get('bounce')}, {st.get('err', '')})")
    time.sleep(1)
    if blocks(b, track):
        failures.append(f"track {track + 1} is still a seq track after its bounce")
    elif len(clips) > track and clips[track] == "0":
        failures.append(f"track {track + 1} has no audio after its bounce")


def segments(b: Board) -> list[int]:
    """What the song plays as audio on each track (ASONG_DBG seg=)."""
    line = b.cmd("ASONG_DBG", "ASONGDBG:", 2.0)
    seg = fields(line).get("seg", "-1/-1/-1/-1")
    return [int(x) for x in seg.split("/")]


def freezes_idle(b: Board, timeout_s: float) -> float:
    """Until no freeze has run for 5 s -- Arrange starts one 3 s after the
    song stops or ARM moves, one at a time -- or @p timeout_s. Seconds taken."""
    t0 = time.time()
    quiet_since = time.time()
    while time.time() - t0 < timeout_s:
        st = asong(b)
        if st.get("bounce") == "1":
            quiet_since = time.time()
        elif time.time() - quiet_since >= 5:
            break
        time.sleep(1.0)
    return time.time() - t0


def freeze_by_arm(b: Board, arm: int, track: int, failures: list[str]) -> None:
    """ARM @p arm with the song stopped and Arrange up: Arrange freezes the
    seq track ARM leaves, @p track (core 2.9 freezeTrack), on its own -- after
    any freeze still waiting, one at a time."""
    b.cmd(f"ASONG ARM {arm}", "ASONG:")
    took = freezes_idle(b, 3 * (SONG_S + 30))
    seg = segments(b)
    print(f"  ARM {arm}: freezes done in {took:.0f} s, segments {seg}")
    if seg[track] < 1:
        failures.append(f"track {track + 1} does not play a freeze after ARM left it (segments {seg})")


def mem_mark(b: Board, label: str) -> None:
    """Internal RAM and its DMA-capable part: free now and the least since
    boot -- which phase took them down."""
    m = fields(b.cmd("MEM", "MEM:"))
    print(f"  [mem {label}] int_free={m.get('int_free')} int_min={m.get('int_min')} "
          f"dma_free={m.get('dma_free')} dma_largest={m.get('dma_largest')} dma_min={m.get('dma_min')}", flush=True)


def judge_play(label: str, r: dict, failures: list[str]) -> None:
    print(f"  {label}: render avg max {r['render_avg_us_max']} us, worst {r['render_worst_us']} us "
          f"(block {r['block_us']}), overruns {r['overruns']}, notes {r['notes']}, "
          f"pitch voices {r['pitch_voices_max']}, PSRAM {r['psram_free_kb']} kB")
    if r["unpaced"]:
        failures.append(f"{label}: unpaced render passes")
    if r["underrun_engine"] or r["norings"]:
        failures.append(f"{label}: engine underrun={r['underrun_engine']} norings={r['norings']}")
    if r["song_underruns"] or r["song_openfail"]:
        failures.append(f"{label}: song ring under={r['song_underruns']} openfail={r['song_openfail']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drums", default="POP_ICONIC")
    ap.add_argument("--loop", default="/sdcard/crosspad/recordings/loop_006.wav")
    ap.add_argument("--audio", default="/sdcard/crosspad/recordings/rec_062.wav")
    ap.add_argument("--listen", action="store_true", help="leave the speakers on")
    args = ap.parse_args()

    failures: list[str] = []
    try:
        b = Board()
        song0 = song_name(b)
        kit0 = current_kit(b)
        muted0 = fields(b.cmd("AUDIO_STATUS", "AUDIO:")).get("mute", "[0,0]").strip("[]").split(",")
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        ensure_slice_kit(b, args.loop)
        ensure_pitched_kit(b)
        ids = kit_ids(b, args.drums)
        rst0 = reset_log_head(b)
        t_start = time.time()
        try:
            print(f"PERF_FULL: {BARS} bars at 120 BPM ({SONG_S:.0f} s), four tracks")
            fresh_song(b, "PERF_FULL")
            b.cmd(f"ASONG LEN {BARS}", "ASONG:")
            b.cmd(f"ASONG REGION 0 {BARS}", "ASONG:")     # a take goes round to the loop's end
            b.cmd("APP_START Arrange", "OK")
            time.sleep(2)
            load_kit(b, ids[args.drums])
            b.cmd("ASONG SEQTRACK", "ASONG:")
            mem_mark(b, "start")
            print("  drums take")
            record_track(b, DRUMS_PATTERN, 2)
            kit_track(b, SLICE_PATH)
            mem_mark(b, "after drums take")
            print("  slice take")
            record_track(b, SLICE_PATTERN, 1)
            b.cmd(f"ASONG PITCHTRACK {PITCHED_PATH}", "ASONG:")
            inst = wait_inst(b, beside=True)
            if inst.get("beside") != "1":
                raise BenchError(f"the Pitch instrument did not load beside the kits: {inst}")
            mem_mark(b, "after slice kit")
            print("  pitch take")
            record_track(b, PITCH_PATTERN, 1)
            mem_mark(b, "after pitch")
            print("  audio: a recording every 8 bars")
            for bar in range(0, BARS, 8):
                if "ERR" in b.cmd(f"ASONG IMPORT {args.audio} 0 0 3 {bar} 0", "", 1.0):
                    raise BenchError(f"import at bar {bar + 1} refused")
                end = time.time() + 60
                while time.time() < end:
                    st = asong(b)
                    if st.get("edit") == "2" and st.get("clips", "0/0/0/0").split("/")[3] == str(bar // 8 + 1):
                        break
                    time.sleep(0.5)
            for t, name in ((0, "drums"), (1, "slice"), (2, "pitch")):
                bl = blocks(b, t)
                print(f"  {name}: " + " | ".join(f"bar {x.get('bar')} x{x.get('bars')} notes {x.get('notes')}" for x in bl))
                if not covers_song(bl):
                    failures.append(f"the {name} track does not cover bars 1..{BARS}")
            print("  clips:", asong(b).get("clips"))
            b.cmd("ASONG REGION 0 0", "ASONG:")           # no loop: the song plays to its end

            print(f"  freezes Arrange began while it was built: done in {freezes_idle(b, 3 * (SONG_S + 30)):.0f} s, "
                  f"segments {segments(b)}")
            mem_mark(b, "after imports and freezes")
            print(f"play the whole arrangement ({SONG_S + 7:.0f} s, past its end)")
            judge_play("four tracks", play_and_measure(b, SONG_S + 7), failures)

            mem_mark(b, "after play")
            print("auto-freeze: ARM moves on, the track it leaves freezes (each in real time)")
            b.cmd("APP_START Arrange", "OK")
            time.sleep(2)
            freeze_by_arm(b, 0, 2, failures)        # Pitch, armed since its take
            freeze_by_arm(b, 1, 0, failures)        # Drums
            freeze_by_arm(b, -1, 1, failures)       # Slice; nothing armed now
            seg = segments(b)
            if min(seg[:3]) < 1:
                failures.append(f"not every seq track plays its freeze: segments {seg}")

            mem_mark(b, "after freezes")
            print(f"play the frozen arrangement ({SONG_S + 7:.0f} s)")
            judge_play("frozen", play_and_measure(b, SONG_S + 7), failures)

            mem_mark(b, "after frozen play")
            print("bounce every seq track (each in real time)")
            for t in (0, 1, 2):
                bounce(b, t, failures)
            print("  clips:", asong(b).get("clips"))

            mem_mark(b, "after bounces")
            print(f"play the bounced arrangement ({SONG_S + 7:.0f} s)")
            judge_play("bounced", play_and_measure(b, SONG_S + 7), failures)
        finally:
            b.cmd("ASONG STOP", "ASONG:")
            heap = fields(b.cmd("HEAP_CHECK", "HEAPCHK:"))
            rst = reset_log_head(b)
            mem = fields(b.cmd("MEM", "MEM:"))
            print(f"after {time.time() - t_start:.0f} s: heap_ok={heap.get('ok')} reset_log={rst} "
                  f"int_min={mem.get('int_min')} psram_free={int(num(mem.get('psram_free'))) // 1024} kB")
            if heap.get("ok") != "1":
                failures.append("HEAP_CHECK failed")
            if rst != rst0:
                failures.append(f"the board reset or dumped core ({rst0} -> {rst})")
            if song0:
                b.cmd(f"ASONG OPEN {song0}", "ASONG:")
            if kit0 >= 0:
                b.cmd(f"KIT_LOAD {kit0}", "OK")
            for codec, was in enumerate(muted0[:2]):          # as it was before, muted or not
                b.cmd(f"AUDIO_MUTE {codec} {1 if was.strip() == '1' else 0}", "OK")
    except BenchError as e:
        print(f"BENCH: {e}")
        return 2

    print("PASS" if not failures else "FAIL:\n  " + "\n  ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
