#!/usr/bin/env python3
"""Performance of the four sound sources in one song, measured on the board.

Builds the songs over CDC, plays them looped, and judges the render's
counters while they play:

  A  PERF_TEST   Drums kit (the pads' kit track, route 0) + Slice kit (the
                 second kit, route 1) + an audio track -- what Arrange holds
                 at once today.
  B  PERF_PITCH  Pitch Play (a pitched kit, four-voice chords on the pads)
                 over an audio track: the pitched engine owning the arena.
  C  PERF_ALL    Drums + Slice + a Pitch track (route 2, its instrument
                 built beside the kits' engines) + audio, all at once.

Fixtures, made once and kept on the card: the Slice kit PERF_SLICE (cut from
--loop by the Slicer's export, STOOL) and the pitched kit HIL_PITCHED
(tools/make_pitched_kit.py; the kit scan needs a reboot to list it, which
this does through the STM console and puts the board back on CDC).

What fails a run (exit 1): an unpaced render pass, an underrun of the sample
engine or the song's ring, a render pass over its block (overruns), a voice
refused a ring, a pass slower than its block on average. Bench trouble (no
board, a fixture that cannot be made) is exit 2.

The songs stay on the card to be opened in Arrange; the song and kit open
before the run are put back at its end. Speakers are muted while it runs
(AUDIO_MUTE) and unmuted after -- not with --listen, for a run someone hears.

  python tools/hil_perf_song.py                    # both scenarios, 30 s each
  python tools/hil_perf_song.py --only A --seconds 60
  python tools/hil_perf_song.py --drums 808_MACHINE_1 --loop /sdcard/crosspad/recordings/loop_006.wav
"""
from __future__ import annotations

import argparse
import glob
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

TOOLS = Path(__file__).resolve().parent
SLICE_KIT = "PERF_SLICE"
SLICE_PATH = f"/crosspad/kits/{SLICE_KIT}/kit.json"
PITCHED_KIT = "HIL PITCHED"          # as kit.json names it; the folder is HIL_PITCHED
BLOCK_US = 64 * 1_000_000 / 44_100          # one render pass at 44.1 kHz
BAR_S = 2.0                                 # 120 BPM, 4/4


class BenchError(Exception):
    """The bench, not the firmware: exit 2."""


# ── CDC ─────────────────────────────────────────────────────────────────────

class Board:
    def __init__(self) -> None:
        self.s: serial.Serial | None = None
        self.open()

    def open(self, wait_s: float = 60.0) -> None:
        end = time.time() + wait_s
        while time.time() < end:
            ports = sorted(glob.glob("/dev/cu.usbmodem1*") + glob.glob("/dev/ttyACM*"))
            for p in ports:
                try:
                    s = serial.Serial(p, 115200, timeout=0.05)
                    s.reset_input_buffer()
                    s.write(b"APP_SHA\r\n")
                    if b"APPSHA:" in self._read(s, 1.0):
                        self.s = s
                        return
                    s.close()
                except (serial.SerialException, OSError):
                    pass
            hil("usb-mode", "set", "default", check=False)
            time.sleep(2)
        raise BenchError("no CrossPad on CDC")

    @staticmethod
    def _read(s: serial.Serial, secs: float, until: bytes | None = None) -> bytes:
        end = time.time() + secs
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
            if until and until in buf and buf.endswith(b"\n"):
                break
        return buf

    def cmd(self, verb: str, prefix: str = "", secs: float = 2.0) -> str:
        """Send @p verb; the first line starting with @p prefix (any line when
        empty) and the rows after it."""
        assert self.s
        try:
            self.s.reset_input_buffer()
            self.s.write((verb + "\r\n").encode())
            raw = self._read(self.s, secs, prefix.encode() if prefix else b"\n")
        except (serial.SerialException, OSError):
            self.s = None
            self.open()
            return self.cmd(verb, prefix, secs)
        text = raw.decode(errors="replace")
        if not prefix:
            return text.strip()
        i = text.find(prefix)
        return text[i:].strip() if i >= 0 else ""


def hil(*args: str, check: bool = True) -> str:
    r = subprocess.run(["crosspad-hil", *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise BenchError(f"crosspad-hil {' '.join(args)}: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=(\S+)", line))


def num(v: str | None) -> float:
    if v is None:
        return 0.0
    m = re.match(r"-?\d+(\.\d+)?", v)
    return float(m.group(0)) if m else 0.0


# ── state on the board ──────────────────────────────────────────────────────

def kit_ids(b: Board, want: str = "") -> dict[str, int]:
    """The kit list by name. With @p want, asked again until it is there
    (20 s): a list read while the card is being scanned comes back short."""
    end = time.time() + 20
    while True:
        text = b.cmd("KIT_LIST", "KITS:", 4.0)
        out: dict[str, int] = {}
        for m in re.finditer(r"(\d+):([^,\r\n]+)", text):
            out[m.group(2).strip()] = int(m.group(1))
        if not want or want in out or time.time() > end:
            return out
        time.sleep(1.0)


def song_name(b: Board) -> str:
    """The open project's whole name: ASONG ends with it, spaces and all,
    where fields() keeps only its first word."""
    lines = b.cmd("ASONG", "ASONG:", 2.0).splitlines()
    line = lines[0] if lines else ""
    at = line.rfind(" project=")
    if at < 0:
        return ""
    name = line[at + 9:]
    err = name.find(" err=")                    # the last bounce's error follows the name
    return (name[:err] if err >= 0 else name).strip()


def current_kit(b: Board) -> int:
    return int(num(fields(b.cmd("SMPL_ENGINE", "SMPLENG:")).get("kit")))


def load_kit(b: Board, kid: int, timeout_s: float = 30.0, pitched: bool = False) -> None:
    """@p kid loaded: the native engine holding it, or -- a pitched kit -- a
    bank installed (a Pitch track's bank beside the kits does not count: the
    native engine's kit is asked for then)."""
    b.cmd(f"KIT_LOAD {kid}", "OK")
    end = time.time() + timeout_s
    while time.time() < end:
        done = (fields(b.cmd("PITCHED_STATUS", "PITCHED:")).get("active") == "1" if pitched
                else current_kit(b) == kid)
        if done:
            time.sleep(1.0)                 # the pads swap in after the take
            return
        time.sleep(0.5)
    raise BenchError(f"kit {kid} did not load in {timeout_s:.0f} s")


def asong(b: Board) -> dict[str, str]:
    return fields(b.cmd("ASONG", "ASONG:"))


def kit_track(b: Board, path: str, kid: int, timeout_s: float = 60.0) -> None:
    """A kit track playing @p path (ASONG SEQTRACK), armed: back once the new
    track is there, Arrange has handed it the kits' engine -- after freezing
    the track it leaves -- and the engine holds @p kid, the kit at @p path.
    The kit asked for, not "another one": the kit already loaded may be it,
    and a freeze may load one meanwhile."""
    used = int(num(fields(b.cmd("ASONG_TRK", "ATRK: used=")).get("used")))
    b.cmd(f"ASONG SEQTRACK {path}", "ASONG:")
    end = time.time() + timeout_s
    while time.time() < end:
        trk = fields(b.cmd("ASONG_TRK", "ATRK: used="))
        eng = fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5))
        if (int(num(trk.get("used"))) > used and trk.get("armed") == trk.get("live_kit")
                and eng.get("state") == "2" and int(num(eng.get("kit"))) == kid):
            time.sleep(1.0)                 # the pads swap in after the take
            return
        time.sleep(0.5)
    raise BenchError(f"the kit track's kit never came in: {path}")


def wait_song_ready(b: Board, name: str, timeout_s: float = 15.0) -> None:
    end = time.time() + timeout_s
    while time.time() < end:
        if asong(b).get("open") == "2" and song_name(b) == name:   # whole: a name may have spaces
            return
        time.sleep(0.5)
    raise BenchError(f"song {name} did not open")


def fresh_song(b: Board, name: str) -> None:
    """@p name, empty: opened, deleted if it was there, opened again."""
    b.cmd(f"ASONG OPEN {name}", "ASONG:")
    wait_song_ready(b, name)
    st = asong(b)
    if st.get("clips", "0/0/0/0") != "0/0/0/0" or st.get("len", "0") != "0":
        b.cmd("ASONG DELETE", "ASONG:")
        time.sleep(2)
        b.cmd(f"ASONG OPEN {name}", "ASONG:")
        wait_song_ready(b, name)
    b.cmd("ASONG TEMPO 120", "ASONG:")
    b.cmd("ASONG LEN 8", "ASONG:")
    b.cmd("ASONG REGION 0 8", "ASONG:")
    time.sleep(1)


def import_audio(b: Board, wav: str, track: int) -> None:
    """@p wav whole onto @p track at bar one, and wait for the copy into the
    project's clips: a 2 MB take is several seconds of card."""
    if "ERR" in b.cmd(f"ASONG IMPORT {wav} 0 0 {track} 0 0", "", 1.0):
        raise BenchError(f"import of {wav} refused")
    end = time.time() + 120
    while time.time() < end:
        st = asong(b)
        clips = st.get("clips", "0/0/0/0").split("/")
        if st.get("edit") == "2" and len(clips) > track and clips[track] != "0":
            return
        if st.get("edit") == "3":
            raise BenchError(f"import of {wav} failed")
        time.sleep(0.5)
    raise BenchError(f"import of {wav} did not finish")


def hits(b: Board, pattern: list[tuple[float, int]], bars: int) -> None:
    """Strike @p pattern -- (beat within the bar, pad) -- for @p bars bars,
    from now, as the take's first bar."""
    t0 = time.time()
    events = sorted((bar * 4 + beat, pad) for bar in range(bars) for beat, pad in pattern)
    for beat, pad in events:
        due = t0 + beat * BAR_S / 4
        while time.time() < due:
            time.sleep(0.002)
        b.cmd(f"PAD_PRESS {pad}", "OK", 0.08)
        b.cmd(f"PAD_RELEASE {pad}", "OK", 0.05)


def record_track(b: Board, pattern: list[tuple[float, int]], bars: int) -> None:
    if "OK" not in b.cmd("ASONG REC 0 0", "", 1.0) and asong(b).get("play") != "1":
        raise BenchError("REC refused")
    end = time.time() + 10
    while asong(b).get("play") != "1" and time.time() < end:
        time.sleep(0.05)
    hits(b, pattern, bars)
    b.cmd("ASONG STOP", "ASONG:")
    time.sleep(1.5)


# ── fixtures ────────────────────────────────────────────────────────────────

def ensure_slice_kit(b: Board, loop: str) -> None:
    if SLICE_KIT in kit_ids(b):
        return
    print(f"  making the Slice kit {SLICE_KIT} from {loop}")
    b.cmd(f"STOOL OV {loop}", "STOOL:")
    time.sleep(1)
    for _ in range(40):                         # job 1 = the picture still being read
        if fields(b.cmd("STOOL", "STOOL:")).get("job") != "1":
            break
        time.sleep(0.5)
    b.cmd("STOOL EQ 8", "STOOL:")
    b.cmd("STOOL ATK 3", "STOOL:")
    b.cmd("STOOL PITCH 0", "STOOL:")
    b.cmd(f"STOOL EXPORT {SLICE_KIT}", "STOOL:")
    for _ in range(60):
        st = fields(b.cmd("STOOL", "STOOL:"))
        if st.get("job") == "2" and st.get("result") == SLICE_KIT:
            break
        if st.get("job") == "3":
            raise BenchError(f"Slice kit export failed: {st.get('err') or st.get('result')}")
        time.sleep(0.5)
    if SLICE_KIT not in kit_ids(b):
        raise BenchError("the Slice kit is not in the kit list after its export")


def ensure_pitched_kit(b: Board) -> None:
    if PITCHED_KIT in kit_ids(b):
        return
    print(f"  pushing the pitched kit {PITCHED_KIT} and rebooting for the kit scan")
    r = subprocess.run([sys.executable, str(TOOLS / "make_pitched_kit.py")], capture_output=True, text=True)
    if r.returncode != 0:
        raise BenchError(f"make_pitched_kit.py: {r.stderr.strip() or r.stdout.strip()}")
    assert b.s
    b.s.close()
    hil("console", "--reset", check=False)
    time.sleep(8)
    b.open(120)
    end = time.time() + 60
    while PITCHED_KIT not in kit_ids(b):          # the boot scan holds the list a while
        if time.time() > end:
            raise BenchError(f"{PITCHED_KIT} not listed after a reboot")
        time.sleep(2)


# ── measurement ─────────────────────────────────────────────────────────────

def snapshot(b: Board) -> dict[str, dict[str, str]]:
    rt = b.cmd("RT_PROF", "RTPROF:")
    return {
        "rt": fields(rt.splitlines()[0] if rt else ""),
        "peak": fields(b.cmd("SMPL_PEAK", "SMPLPEAK:")),
        "eng": fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5)),
        "pitch": fields(b.cmd("PITCHED_STATUS", "PITCHED:")),
        "song": asong(b),
        "mem": fields(b.cmd("MEM", "MEM:")),
    }


def play_and_measure(b: Board, seconds: float, during=None) -> dict:
    b.cmd("RT_PROF 1", "OK")
    before = snapshot(b)
    b.cmd("ASONG PLAY 0", "ASONG:")
    t_end = time.time() + seconds
    worst_total = worst_pass = 0.0
    voices_max = pitch_voices_max = 0
    samples = 0
    while time.time() < t_end:
        if during:
            during(b)
        s = snapshot(b)
        samples += 1
        worst_total = max(worst_total, num(s["rt"].get("total_us")))
        worst_pass = max(worst_pass, num(s["rt"].get("worst_us")))
        voices_max = max(voices_max, int(num(s["eng"].get("voices"))))
        pitch_voices_max = max(pitch_voices_max, int(num(s["pitch"].get("voices"))))
        time.sleep(1.0)
    after = snapshot(b)
    b.cmd("ASONG STOP", "ASONG:")

    def delta(section: str, key: str) -> int:
        return int(num(after[section].get(key)) - num(before[section].get(key)))

    return {
        "seconds": seconds,
        "samples": samples,
        "playing": after["song"].get("play") == "1" or num(after["song"].get("pos")) > 0,
        "render_avg_us_max": round(worst_total, 1),
        "render_worst_us": round(worst_pass, 1),
        "block_us": round(BLOCK_US, 1),
        "mix_us": num(after["rt"].get("mix_us")),
        "unpaced": int(num(after["peak"].get("unpaced"))),
        "overruns": delta("peak", "overruns"),
        "underrun_engine": delta("eng", "underrun"),
        "norings": delta("eng", "norings"),
        "steals": delta("eng", "steal"),
        "notes": delta("eng", "notes"),
        "voices_max": voices_max,
        "song_underruns": delta("song", "under"),
        "song_openfail": delta("song", "openfail"),
        "pitch_voices_max": pitch_voices_max,
        "pitch_steals": delta("pitch", "steals"),
        "pitch_rt_max_us": num(after["pitch"].get("rt_max_us")),
        "psram_free_kb": int(num(after["mem"].get("psram_free")) // 1024),
        "psram_largest_kb": int(num(after["mem"].get("psram_largest")) // 1024),
        "int_free": int(num(after["mem"].get("int_free"))),
    }


def judge(name: str, r: dict) -> list[str]:
    bad = []
    if r["unpaced"]:
        bad.append(f"unpaced={r['unpaced']} (the render ran without the I2S brake)")
    if r["overruns"]:
        bad.append(f"{r['overruns']} render passes over their block")
    if r["underrun_engine"]:
        bad.append(f"{r['underrun_engine']} sample engine underruns")
    if r["norings"]:
        bad.append(f"{r['norings']} voices refused a ring")
    if r["song_underruns"] or r["song_openfail"]:
        bad.append(f"song ring: under={r['song_underruns']} openfail={r['song_openfail']}")
    if r["render_avg_us_max"] > r["block_us"]:
        bad.append(f"render {r['render_avg_us_max']} us on average > block {r['block_us']} us")
    if r["notes"] <= 0 and name in ("A", "C"):
        bad.append("no notes played: the kit tracks are silent")
    if name == "C":
        if r["pitch_voices_max"] <= 0:
            bad.append("the Pitch track never sounded")
    return bad


# ── the scenarios ───────────────────────────────────────────────────────────

DRUMS_PATTERN = [(0, 0), (1, 1), (2, 0), (3, 1)] + [(x / 2, 2) for x in range(8)]   # kick, snare, eighth hats
SLICE_PATTERN = [(x / 2, x % 8) for x in range(8)]                                    # every slice, in eighths
CHORDS = [[0, 4, 7, 11], [2, 5, 9, 12], [4, 7, 11, 14], [5, 9, 12, 15]]
PITCH_PATTERN = [(0, 0), (0, 4), (0, 7), (2, 2), (2, 5), (2, 9)]                   # two chords a bar
PITCHED_PATH = "/crosspad/kits/HIL_PITCHED/kit.json"


def scenario_a(b: Board, ids: dict[str, int], drums: str, audio: str, seconds: float) -> dict:
    print("A: Drums + Slice + audio (PERF_TEST)")
    if drums not in ids:
        raise BenchError(f"no kit called {drums} on the card")
    fresh_song(b, "PERF_TEST")
    b.cmd("APP_START Arrange", "OK")
    time.sleep(2)
    load_kit(b, ids[drums])
    b.cmd("ASONG SEQTRACK", "ASONG:")
    print("  drums take, 2 bars")
    record_track(b, DRUMS_PATTERN, 2)
    kit_track(b, SLICE_PATH, kit_ids(b, SLICE_KIT)[SLICE_KIT])
    print("  slice take, 1 bar")
    record_track(b, SLICE_PATTERN, 1)
    print("  audio track")
    import_audio(b, audio, 2)
    seq = b.cmd("ASONG_SEQ 0", "ASEQ:")
    seq1 = b.cmd("ASONG_SEQ 1", "ASEQ:")
    print("  track 1 (drums):", " | ".join(l for l in seq.splitlines() if "bar=" in l) or "-")
    print("  track 2 (slice):", " | ".join(l for l in seq1.splitlines() if "bar=" in l) or "-")
    print("  clips:", asong(b).get("clips"))
    print(f"  playing {seconds:.0f} s looped")
    return play_and_measure(b, seconds)


def scenario_b(b: Board, ids: dict[str, int], audio: str, seconds: float) -> dict:
    print("B: Pitch Play + audio (PERF_PITCH)")
    fresh_song(b, "PERF_PITCH")
    import_audio(b, audio, 0)
    b.cmd("APP_START Sampler", "OK")
    time.sleep(2)
    load_kit(b, ids[PITCHED_KIT], pitched=True)
    chord = {"i": 0, "held": []}

    def strum(board: Board) -> None:
        for p in chord["held"]:
            board.cmd(f"PAD_RELEASE {p}", "OK", 0.05)
        chord["held"] = CHORDS[chord["i"] % len(CHORDS)]
        chord["i"] += 1
        for p in chord["held"]:
            board.cmd(f"PAD_PRESS {p}", "OK", 0.05)

    print(f"  playing {seconds:.0f} s, a four-note chord a second over the audio")
    r = play_and_measure(b, seconds, strum)
    for p in chord["held"]:
        b.cmd(f"PAD_RELEASE {p}", "OK", 0.05)
    return r


def scenario_c(b: Board, ids: dict[str, int], drums: str, audio: str, seconds: float) -> dict:
    print("C: Drums + Slice + Pitch + audio at once (PERF_ALL)")
    if drums not in ids:
        raise BenchError(f"no kit called {drums} on the card")
    fresh_song(b, "PERF_ALL")
    b.cmd("APP_START Arrange", "OK")
    time.sleep(2)
    load_kit(b, ids[drums])
    b.cmd("ASONG SEQTRACK", "ASONG:")
    print("  drums take, 2 bars")
    record_track(b, DRUMS_PATTERN, 2)
    kit_track(b, SLICE_PATH, kit_ids(b, SLICE_KIT)[SLICE_KIT])
    print("  slice take, 1 bar")
    record_track(b, SLICE_PATTERN, 1)
    b.cmd(f"ASONG PITCHTRACK {PITCHED_PATH}", "ASONG:")
    end = time.time() + 60
    while time.time() < end:
        inst = fields(b.cmd("INST_STATUS", "INST:"))
        if inst.get("state") == "ready":
            break
        if inst.get("state") == "failed":
            raise BenchError(f"the Pitch track's instrument did not load: {inst.get('msg')}")
        time.sleep(0.5)
    else:
        raise BenchError("the Pitch track's instrument did not load in 60 s")
    print(f"  instrument: {inst}")
    if inst.get("beside") != "1":
        raise BenchError("the instrument took the kits' engine instead of loading beside it")
    print("  pitch take, 1 bar")
    record_track(b, PITCH_PATTERN, 1)
    print("  audio track")
    import_audio(b, audio, 3)
    for t in range(3):
        rows = b.cmd(f"ASONG_SEQ {t}", "ASEQ:")
        print(f"  track {t + 1}:", " | ".join(l for l in rows.splitlines() if "bar=" in l) or "-")
    print("  clips:", asong(b).get("clips"))
    print(f"  playing {seconds:.0f} s looped")
    r = play_and_measure(b, seconds)
    r["kit_engine"] = current_kit(b) == ids[drums]
    return r


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", choices=("A", "B", "C"))
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--drums", default="POP_ICONIC", help="a kit from the DRUMS list")
    ap.add_argument("--loop", default="/sdcard/crosspad/recordings/loop_006.wav",
                    help="the WAV the Slice kit is cut from, once")
    ap.add_argument("--audio", default="/sdcard/crosspad/recordings/rec_062.wav",
                    help="the WAV on the audio track")
    ap.add_argument("--json", type=Path, help="write the results here too")
    ap.add_argument("--listen", action="store_true",
                    help="leave the speakers on: someone is listening to the run")
    args = ap.parse_args()

    try:
        b = Board()
        song0 = song_name(b)
        kit0 = current_kit(b)
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        results: dict[str, dict] = {}
        verdict: dict[str, list[str]] = {}
        try:
            if args.only in (None, "A"):
                ensure_slice_kit(b, args.loop)
                results["A"] = scenario_a(b, kit_ids(b), args.drums, args.audio, args.seconds)
            if args.only in (None, "B"):
                ensure_pitched_kit(b)
                results["B"] = scenario_b(b, kit_ids(b), args.audio, args.seconds)
            if args.only in (None, "C"):
                ensure_slice_kit(b, args.loop)
                ensure_pitched_kit(b)
                results["C"] = scenario_c(b, kit_ids(b), args.drums, args.audio, args.seconds)
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

    failed = False
    for name, r in results.items():
        verdict[name] = judge(name, r)
        print(f"\n== {name} ==")
        for k, v in r.items():
            print(f"  {k:20} {v}")
        print("  " + ("PASS" if not verdict[name] else "FAIL: " + "; ".join(verdict[name])))
        failed |= bool(verdict[name])
    if args.json:
        args.json.write_text(json.dumps({"results": results, "verdict": verdict}, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
