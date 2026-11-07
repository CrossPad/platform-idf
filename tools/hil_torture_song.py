#!/usr/bin/env python3
"""Torture the song tools: everything a player does in Arrange, at random, in
a song of its own (ZZ_TORTURE), with the board checked after every step.

The actions, picked by weight from --seed (a failing run is replayed with the
same seed):

  tracks   a kit track (SEQTRACK), one with a kit of its own (SEQTRACK kit), a Pitch track
           (PITCHTRACK), an audio track (IMPORT); DELTRACK
  takes    REC and DUB on an armed seq track, and PUNCH in and out of a
           playing song, played as chords -- 3 to 10 pads struck in one
           write, so they land together
  storms   chord storms while the song plays, STM_VPAD bursts from the STM
  blocks   DUP, DELBLOCK, MOVE, CUT, DUPREGION, PASTE
  renders  FREEZE, ARM moving on (Arrange's auto-freeze), BOUNCESEQ
  FX       STOOL FX on a recording -- pitch (keeping the length or varispeed),
           high and low pass, reverse -- and the copy imported onto a track
  sounds   KIT_LOAD of a drums kit while the song plays, TRACKKIT
  mix      PLAY/STOP, TEMPO, MUTE, VOL
  song     closed (another song opened) and opened again: its tracks and
           blocks must come back as they were, and song.json on the card
           must be the song in memory before it closes (--evidence keeps it)

After every action: no reset and no core dump since the start, the board
answering. Every --check-every actions: HEAP_CHECK, no unpaced render pass,
no sample engine or song ring underrun, no voice refused a ring,
and at the end, back in the song and kit it started in, PSRAM within --leak-kb
of where it was. Render passes over their
block are counted and reported, not failed: kit loads and bounces make them.

A player at the board wins: PAD_STATS stm= growing beyond the STM_VPAD hits,
or another app on the screen, stops the run (exit 2), unmutes and puts the
song and kit back. ZZ_TORTURE is deleted after a passing run (--keep keeps
it), kept after a failing one.

Exit 0 passed, 1 the firmware failed a check, 2 the bench.

  python tools/hil_torture_song.py --actions 200
  python tools/hil_torture_song.py --seed 7 --listen
"""
from __future__ import annotations

import argparse
import json
import random
import re
import subprocess
import sys
import time
from pathlib import Path

from hil_perf_song import (BenchError, Board, asong, current_kit, fields, kit_ids, load_kit, num, song_name,
                           wait_song_ready)
from hil_stress_song import reset_log_head

SONG = "ZZ_TORTURE"
SONG_B = "ZZ_TORTURE_B"
FX_DIR = "/sdcard/crosspad/hil/torture"     # the FX copies, imported into the song from here
DRUM_KITS = ["808_MACHINE_1", "909_MACHINE_1", "POP_ICONIC", "BOOMBAP 01", "HOUSE 01", "TRAP_808_1", "NY_GRIT",
             "DNB_JUNGLE_1"]
SLICE_KITS = ["PERF_SLICE", "LOOP_006", "REC_062"]
INSTRUMENTS = ["BASS_Reese", "LEAD_Bright", "808_Bright", "BELL_Magic", "BASS_Moog"]
WAVS = ["/sdcard/crosspad/recordings/loop_006.wav", "/sdcard/crosspad/recordings/rec_064.wav",
        "/sdcard/crosspad/recordings/rec_061.wav", "/sdcard/crosspad/recordings/rec_062.wav"]
MAX_LEN = 32           # bars: no DUPREGION/PASTE past it
TOOLS = Path(__file__).resolve().parent


def drum_path(name: str) -> str:
    return f"/crosspad/drums/{name}/kit.json"


def slice_path(name: str) -> str:
    return f"/crosspad/slicer/{name}/kit.json"


def inst_path(name: str) -> str:
    return f"/crosspad/pitchplay/{name}/instrument.json"


class UserAtBoard(Exception):
    """Someone is playing the board: stop and give it back."""


class Torture:
    def __init__(self, b: Board, rng: random.Random, args: argparse.Namespace) -> None:
        self.b = b
        self.rng = rng
        self.args = args
        self.failures: list[str] = []
        self.log: list[dict] = []
        self.step = 0
        self.fx_n = 0
        self.overruns = 0
        self.stm_base = self.stm()
        self.rst0 = reset_log_head(b)
        self.base: dict[str, int] = {}
        self.ids = kit_ids(b)

    # ── board state ─────────────────────────────────────────────────────────

    def stm(self) -> int:
        return int(num(fields(self.b.cmd("PAD_STATS", "PADSTATS:")).get("stm")))

    def rows(self, verb: str, last: str, secs: float = 2.0) -> list[str]:
        """A reply of several rows, read until the one containing @p last:
        Board.cmd() stops at the first whole line."""
        assert self.b.s
        self.b.s.reset_input_buffer()
        self.b.s.write((verb + "\r\n").encode())
        buf = b""
        end = time.time() + secs
        while time.time() < end:
            buf += self.b.s.read(4096)
            i = buf.find(last.encode())
            if i >= 0 and b"\n" in buf[i:]:
                break
        return [l.strip() for l in buf.decode(errors="replace").splitlines() if l.strip()]

    def tracks(self) -> tuple[list[dict[str, str]], dict[str, str]]:
        rows = [r for r in self.rows("ASONG_TRK", "ATRK: used=") if r.startswith("ATRK:")]
        trk = [fields(r) for r in rows if " t=" in r]
        tail = fields(rows[-1]) if rows else {}
        return trk, tail

    def seq_tracks(self) -> list[int]:
        return [int(t["t"]) for t in self.tracks()[0] if t.get("kind") == "s"]

    def song_len(self) -> int:
        return int(num(asong(self.b).get("len")))

    def guard(self) -> None:
        if self.stm() > self.stm_base:
            raise UserAtBoard("pads struck on the board (PAD_STATS stm= grew)")
        app = fields(self.b.cmd("UI_STATE", "UI:")).get("app")
        if app != "Arrange":
            raise UserAtBoard(f"the screen shows {app}, not Arrange")

    def fail(self, what: str) -> None:
        msg = f"step {self.step}: {what}"
        print(f"    FAIL {what}", flush=True)
        self.failures.append(msg)

    def alive(self) -> None:
        """The board answers, and has neither reset nor dumped core."""
        st = asong(self.b)
        if not st:
            time.sleep(5)
            st = asong(self.b)
            if not st:
                self.fail("the board stopped answering ASONG")
        rst = reset_log_head(self.b)
        if rst != self.rst0:
            self.fail(f"the board reset or dumped core (log {self.rst0} -> {rst})")
            raise BenchError("the board reset: the run cannot go on in its song")

    def counters(self) -> dict[str, int]:
        eng = fields(self.b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5))
        peak = fields(self.b.cmd("SMPL_PEAK", "SMPLPEAK:"))
        return {
            "eng_under": int(num(eng.get("underrun"))),
            "norings": int(num(eng.get("norings"))),
            "song_under": int(num(asong(self.b).get("under"))),
            "unpaced": int(num(peak.get("unpaced"))),
            "overruns": int(num(peak.get("overruns"))),
        }

    def check(self) -> None:
        c = self.counters()
        heap = fields(self.b.cmd("HEAP_CHECK", "HEAPCHK:"))
        mem = fields(self.b.cmd("MEM", "MEM:"))
        if not self.base:
            self.base = c
        # A counter below its base was reset (its engine rebuilt): counted from 0.
        d = {k: c[k] - self.base[k] if c[k] >= self.base[k] else c[k] for k in c}
        self.overruns = d["overruns"]
        psram = int(num(mem.get("psram_free")) // 1024)
        print(f"    check: heap_ok={heap.get('ok')} psram={psram} kB largest={int(num(mem.get('psram_largest')) // 1024)} kB "
              f"int_min={mem.get('int_min')} unpaced={c['unpaced']} overruns={d['overruns']} eng_under={d['eng_under']} "
              f"song_under={d['song_under']} norings={d['norings']}", flush=True)
        if heap.get("ok") != "1":
            self.fail("HEAP_CHECK failed")
        if c["unpaced"]:
            self.fail(f"unpaced render passes ({c['unpaced']})")
        for k in ("eng_under", "song_under", "norings"):
            if d[k]:
                self.fail(f"{k} +{d[k]}")
        self.base.update({k: c[k] for k in ("eng_under", "song_under", "norings")})

    # ── waiting ─────────────────────────────────────────────────────────────

    def wait_edit(self, what: str, timeout_s: float = 120.0) -> bool:
        end = time.time() + timeout_s
        while time.time() < end:
            e = asong(self.b).get("edit")
            if e == "2" or e == "0":
                return True
            if e == "3":
                self.fail(f"{what}: the edit failed")
                return False
            time.sleep(0.5)
        self.fail(f"{what}: the edit did not finish in {timeout_s:.0f} s")
        return False

    def wait_render(self, what: str, timeout_s: float = 240.0) -> None:
        """A freeze or bounce through: it may refuse to start ('waiting' --
        the pads play its engine), never fail otherwise or hang."""
        end = time.time() + timeout_s
        seen = False
        st: dict[str, str] = {}
        while time.time() < end:
            line = self.b.cmd("ASONG", "ASONG:")
            st = fields(line)
            if st.get("bounce") == "1":
                seen = True
            elif seen or time.time() > end - timeout_s + 4:
                break
            time.sleep(0.5)
        else:
            self.fail(f"{what}: still rendering after {timeout_s:.0f} s")
            self.b.cmd("ASONG CANCEL", "ASONG:")
            return
        line = self.b.cmd("ASONG", "ASONG:")
        if fields(line).get("bounce") == "3":
            err = line.split(" err=", 1)[1] if " err=" in line else "?"
            if "waiting" not in err and "cancel" not in err.lower():
                self.fail(f"{what}: failed: {err}")
            else:
                print(f"    ({what}: {err})")

    # ── pads ────────────────────────────────────────────────────────────────

    def burst(self, lines: list[str]) -> None:
        """Several verbs in one write: chord pads land together."""
        assert self.b.s
        self.b.s.reset_input_buffer()
        self.b.s.write("".join(l + "\r\n" for l in lines).encode())
        time.sleep(0.01)

    def chord(self, hold_s: float) -> None:
        pads = self.rng.sample(range(16), self.rng.randint(3, 10))
        self.burst([f"PAD_PRESS {p} {self.rng.randint(30, 127)}" for p in pads])
        time.sleep(hold_s)
        self.burst([f"PAD_RELEASE {p}" for p in pads])

    def play_chords(self, seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            self.chord(self.rng.uniform(0.03, 0.4))
            time.sleep(self.rng.uniform(0.0, 0.3))

    # ── actions ─────────────────────────────────────────────────────────────

    def a_add_kit(self) -> str:
        if self.tracks()[1].get("used", "0") == "8":
            return "skip: 8 tracks"
        return "ok=" + fields(self.b.cmd("ASONG SEQTRACK", "ASONG:")).get("ok", "?")

    def a_add_kit2(self) -> str:
        if self.tracks()[1].get("used", "0") == "8":
            return "skip: 8 tracks"
        kit = self.rng.choice(SLICE_KITS + DRUM_KITS[:3])
        path = slice_path(kit) if kit in SLICE_KITS else drum_path(kit)
        ok = fields(self.b.cmd(f"ASONG SEQTRACK {path}", "ASONG:")).get("ok")
        return f"{kit} ok={ok}"

    def a_add_pitch(self) -> str:
        if self.tracks()[1].get("used", "0") == "8":
            return "skip: 8 tracks"
        name = self.rng.choice(INSTRUMENTS)
        ok = fields(self.b.cmd(f"ASONG PITCHTRACK {inst_path(name)}", "ASONG:")).get("ok")
        end = time.time() + 60
        inst: dict[str, str] = {}
        while time.time() < end:
            inst = fields(self.b.cmd("INST_STATUS", "INST:"))
            if inst.get("state") in ("ready", "failed"):
                break
            time.sleep(0.5)
        if ok == "1" and inst.get("state") != "ready":
            self.fail(f"Pitch track {name}: the instrument is {inst.get('state')} ({inst.get('msg')})")
        return f"{name} ok={ok} inst={inst.get('state')} beside={inst.get('beside')}"

    def free_or_audio_track(self) -> int:
        trk, _ = self.tracks()
        used = {int(t["t"]): t for t in trk}
        audio = [t for t, r in used.items() if r.get("kind") == "a"]
        free = [t for t in range(8) if t not in used]
        if audio and (not free or self.rng.random() < 0.5):
            return self.rng.choice(audio)
        return free[0] if free else -1

    def import_onto(self, wav: str) -> str:
        t = self.free_or_audio_track()
        if t < 0:
            return "skip: no track for audio"
        bar = self.rng.randint(0, max(0, self.song_len() - 1))
        r = self.b.cmd(f"ASONG IMPORT {wav} 0 0 {t} {bar} 0", "ASONG:")
        if fields(r).get("ok") != "1":
            return f"refused onto track {t + 1}"
        self.wait_edit(f"import of {wav}")
        return f"track {t + 1} bar {bar + 1}"

    def a_add_audio(self) -> str:
        wav = self.rng.choice(WAVS)
        return wav.rsplit("/", 1)[1] + " -> " + self.import_onto(wav)

    def take(self, kind: str) -> str:
        seq = self.seq_tracks()
        if not seq:
            return "skip: no seq track"
        t = self.rng.choice(seq)
        self.b.cmd("ASONG STOP", "ASONG:")             # REC and DUB start the song; PUNCH is for a playing one
        self.b.cmd(f"ASONG ARM {t}", "ASONG:")
        bar = self.rng.randint(0, max(0, self.song_len() - 2))
        ok = fields(self.b.cmd(f"ASONG {kind} {bar} 0", "ASONG:")).get("ok")
        end = time.time() + 10
        while asong(self.b).get("play") != "1" and time.time() < end:
            time.sleep(0.05)
        self.play_chords(self.rng.uniform(2.0, 6.0))
        self.b.cmd("ASONG STOP", "ASONG:")
        time.sleep(1.0)
        rows = [l for l in self.rows(f"ASONG_SEQ {t}", "ASEQ: track=") if "bar=" in l]
        return f"track {t + 1} from bar {bar + 1} ok={ok}: {len(rows)} blocks"

    def a_rec(self) -> str:
        return self.take("REC")

    def a_dub(self) -> str:
        return self.take("DUB")

    def a_punch(self) -> str:
        seq = self.seq_tracks()
        if not seq:
            return "skip: no seq track"
        t = self.rng.choice(seq)
        self.b.cmd(f"ASONG ARM {t}", "ASONG:")
        if asong(self.b).get("play") != "1":
            self.b.cmd(f"ASONG PLAY {self.rng.randint(0, max(0, self.song_len() - 1))}", "ASONG:")
            time.sleep(1.0)
        mode = self.rng.choice("RD")
        ok = fields(self.b.cmd(f"ASONG PUNCH {mode}", "ASONG:")).get("ok")
        self.play_chords(self.rng.uniform(2.0, 5.0))
        out = fields(self.b.cmd("ASONG PUNCH N", "ASONG:")).get("ok")
        return f"track {t + 1} {'REC' if mode == 'R' else 'DUB'} in ok={ok}, out ok={out}"

    def a_storm(self) -> str:
        self.b.cmd("ASONG PLAY 0", "ASONG:")
        secs = self.rng.uniform(3, 8)
        self.play_chords(secs)
        return f"{secs:.1f} s of chords while playing"

    def a_vpad(self) -> str:
        self.b.cmd("ASONG PLAY 0", "ASONG:")
        rate, count = self.rng.choice([(30, 120), (60, 300), (120, 400)])
        self.b.cmd(f"STM_VPAD {rate} {count} 40", "VPAD:")
        end = time.time() + count / rate + 10
        vp: dict[str, str] = {}
        while time.time() < end:
            vp = fields(self.b.cmd("STM_VPAD", "VPAD:"))
            if vp.get("remaining") == "0":
                break
            time.sleep(0.5)
        time.sleep(0.5)
        self.stm_base = self.stm()             # the STM's hits count as struck pads
        return f"{rate} Hz x {count}: sent={vp.get('sent')} short_esp={vp.get('short_esp')} esp_busy={vp.get('esp_busy')}"

    def pick_block(self) -> tuple[int, int, int]:
        trk = [t for t in self.tracks()[0] if int(num(t.get("blocks"))) > 0]
        if not trk:
            return -1, -1, 0
        t = self.rng.choice(trk)
        n = int(num(t.get("blocks")))
        return int(t["t"]), self.rng.randrange(n), n

    def a_dup(self) -> str:
        t, blk, _ = self.pick_block()
        if t < 0:
            return "skip: no block"
        ok = fields(self.b.cmd(f"ASONG DUP {t} {blk}", "ASONG:")).get("ok")
        return f"track {t + 1} block {blk} ok={ok}"

    def a_delblock(self) -> str:
        t, blk, n = self.pick_block()
        if t < 0:
            return "skip: no block"
        ok = fields(self.b.cmd(f"ASONG DELBLOCK {t} {blk}", "ASONG:")).get("ok")
        return f"track {t + 1} block {blk}/{n} ok={ok}"

    def a_move(self) -> str:
        t, blk, _ = self.pick_block()
        if t < 0:
            return "skip: no block"
        bar = self.rng.randint(0, max(1, self.song_len()))
        ok = fields(self.b.cmd(f"ASONG MOVE {t} {blk} {bar}", "ASONG:")).get("ok")
        return f"track {t + 1} block {blk} -> bar {bar + 1} ok={ok}"

    def a_cut(self) -> str:
        t, _, _ = self.pick_block()
        if t < 0:
            return "skip: no block"
        bar = self.rng.randint(1, max(1, self.song_len() - 1))
        ok = fields(self.b.cmd(f"ASONG CUT {t} {bar}", "ASONG:")).get("ok")
        return f"track {t + 1} at bar {bar + 1} ok={ok}"

    def a_dupregion(self) -> str:
        n = self.song_len()
        if n * 2 > MAX_LEN or n < 1:
            return f"skip: {n} bars"
        a = self.rng.randint(0, n - 1)
        bb = self.rng.randint(a + 1, n)
        self.b.cmd(f"ASONG REGION {a} {bb}", "ASONG:")
        ok = fields(self.b.cmd("ASONG DUPREGION", "ASONG:")).get("ok")
        self.b.cmd("ASONG REGION 0 0", "ASONG:")
        return f"bars {a + 1}..{bb} ok={ok}"

    def a_paste(self) -> str:
        n = self.song_len()
        if n + 4 > MAX_LEN or n < 2:
            return f"skip: {n} bars"
        a = self.rng.randint(0, n - 2)
        bb = min(n, a + self.rng.randint(1, 4))
        at = self.rng.randint(0, n)
        ok = fields(self.b.cmd(f"ASONG PASTE {a} {bb} {at}", "ASONG:")).get("ok")
        return f"bars {a + 1}..{bb} at {at + 1} ok={ok}"

    def a_deltrack(self) -> str:
        trk, tail = self.tracks()
        if int(num(tail.get("used"))) < 3:
            return "skip: few tracks"
        t = int(self.rng.choice(trk)["t"])
        ok = fields(self.b.cmd(f"ASONG DELTRACK {t}", "ASONG:")).get("ok")
        return f"track {t + 1} ok={ok}"

    def a_freeze(self) -> str:
        _, tail = self.tracks()
        seq = [t for t in self.seq_tracks() if str(t) != tail.get("armed")]
        if not seq:
            return "skip: no unarmed seq track"
        t = self.rng.choice(seq)
        self.b.cmd("ASONG STOP", "ASONG:")
        ok = fields(self.b.cmd(f"ASONG FREEZE {t}", "ASONG:")).get("ok")
        self.wait_render(f"freeze of track {t + 1}")
        fz = {r["t"]: r.get("fz") for r in self.tracks()[0]}.get(str(t))
        return f"track {t + 1} ok={ok} fz={fz}"

    def a_arm(self) -> str:
        seq = self.seq_tracks()
        if not seq:
            return "skip: no seq track"
        t = self.rng.choice(seq)
        self.b.cmd(f"ASONG ARM {t}", "ASONG:")
        return f"armed track {t + 1}"

    def a_bounce(self) -> str:
        seq = self.seq_tracks()
        if not seq:
            return "skip: no seq track"
        t = self.rng.choice(seq)
        self.b.cmd("ASONG STOP", "ASONG:")
        ok = fields(self.b.cmd(f"ASONG BOUNCESEQ {t}", "ASONG:")).get("ok")
        if ok == "1":
            self.wait_render(f"bounce of track {t + 1}")
        kind = {r["t"]: r.get("kind") for r in self.tracks()[0]}.get(str(t))
        return f"track {t + 1} ok={ok} kind now {kind}"

    def a_fx(self) -> str:
        src = self.rng.choice(WAVS[:3])
        st = self.rng.choice([-12, -7, -3, 0, 2, 5, 12])
        hp = self.rng.choice([0, 0, 125, 500])
        lp = self.rng.choice([0, 0, 2000, 8000])
        rev = int(self.rng.random() < 0.2)
        keep = int(self.rng.random() < 0.6)
        if not (st or hp or lp or rev):
            st = 3
        self.fx_n += 1
        out = f"{FX_DIR}/fx_{self.step:03d}.wav"
        ok = fields(self.b.cmd(f"STOOL FX {src} {out} {st} {hp} {lp} {rev} {keep}", "STOOL:")).get("ok")
        desc = f"{src.rsplit('/', 1)[1]} st={st} hp={hp} lp={lp} rev={rev} keep={keep}"
        if ok != "1":
            return desc + " refused"
        end = time.time() + 120
        job: dict[str, str] = {}
        while time.time() < end:
            job = fields(self.b.cmd("STOOL", "STOOL:", 2.0))
            if job.get("job") == "3" or (job.get("job") == "2" and out.rsplit("/", 1)[1] in job.get("result", "")):
                break
            time.sleep(0.5)
        if job.get("job") != "2":
            self.fail(f"FX {desc}: job={job.get('job')} err={job.get('err')} result={job.get('result')}")
            return desc + " failed"
        return desc + " -> " + self.import_onto(out)

    def a_kit_swap(self) -> str:
        name = self.rng.choice([k for k in DRUM_KITS if k in self.ids])
        if self.rng.random() < 0.6:
            self.b.cmd("ASONG PLAY 0", "ASONG:")
        try:
            load_kit(self.b, self.ids[name], timeout_s=40)
        except BenchError as e:
            trk = " | ".join(r for r in self.rows("ASONG_TRK", "ATRK: used=") if r.startswith("ATRK:"))
            eng = self.b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5)[:160]
            swap = [l for l in self.rows("KIT_SWAP_LOG", "KITSWAP: end") if "seq=" in l][-1:]
            self.fail(f"kit swap to {name}: {e} -- tracks: {trk} -- engine: {eng} -- last swap: {swap}")
        self.play_chords(self.rng.uniform(0.5, 2.0))
        return name

    def a_trackkit(self) -> str:
        trk, _ = self.tracks()
        seq = [r for r in trk if r.get("kind") == "s" and r.get("route") == "0"]
        if not seq:
            return "skip: no kit track"
        r = self.rng.choice(seq)
        name = self.rng.choice(DRUM_KITS[:4])
        ok = fields(self.b.cmd(f"ASONG TRACKKIT {r['t']} {drum_path(name)}", "ASONG:")).get("ok")
        return f"track {int(r['t']) + 1} route {r['route']} -> {name} ok={ok}"

    def a_play(self) -> str:
        if asong(self.b).get("play") == "1":
            self.b.cmd("ASONG STOP", "ASONG:")
            return "stop"
        bar = self.rng.randint(0, max(0, self.song_len() - 1))
        self.b.cmd(f"ASONG PLAY {bar}", "ASONG:")
        time.sleep(self.rng.uniform(1, 4))
        return f"play from bar {bar + 1}"

    def a_tempo(self) -> str:
        bpm = self.rng.randint(80, 150)
        self.b.cmd(f"ASONG TEMPO {bpm}", "ASONG:")
        return f"{bpm} BPM"

    def a_mix(self) -> str:
        trk, _ = self.tracks()
        if not trk:
            return "skip: no track"
        t = int(self.rng.choice(trk)["t"])
        if self.rng.random() < 0.5:
            m = int(self.rng.random() < 0.3)
            self.b.cmd(f"ASONG MUTE {t} {m}", "ASONG:")
            return f"track {t + 1} mute {m}"
        v = self.rng.randint(20, 100)
        self.b.cmd(f"ASONG VOL {t} {v}", "ASONG:")
        return f"track {t + 1} vol {v}%"

    def pull_song(self, tag: str) -> Path | None:
        """The song's song.json off the card, into --evidence: the port is
        let go for fs_transfer and taken back."""
        if not self.args.evidence:
            return None
        self.args.evidence.mkdir(parents=True, exist_ok=True)
        out = self.args.evidence / f"step{self.step:03d}_{tag}.json"
        assert self.b.s
        self.b.s.close()
        self.b.s = None
        subprocess.run([sys.executable, str(TOOLS / "fs_transfer.py"), "pull", f"/sdcard/SONGS/{SONG}/song.json",
                        str(out)], capture_output=True, text=True)
        self.b.open()
        return out if out.exists() else None

    @staticmethod
    def saved_tracks(path: Path | None) -> list[tuple[str, str, str]]:
        if not path:
            return []
        doc = json.loads(path.read_text())
        return [(str(i), "s" if t.get("kind") == 1 else "a", str(len(t.get("clips", []))))
                for i, t in enumerate(doc.get("tracks", [])) if t.get("used")]

    def a_reopen(self) -> str:
        self.b.cmd("ASONG STOP", "ASONG:")
        time.sleep(2)                                 # the last commit saved
        before = [(r["t"], r.get("kind"), r.get("blocks")) for r in self.tracks()[0]]
        saved = self.saved_tracks(self.pull_song("before"))
        if saved and saved != before:
            self.fail(f"song.json on the card is not the song in memory: memory {before}, card {saved}")
        self.b.cmd(f"ASONG OPEN {SONG_B}", "ASONG:")
        wait_song_ready(self.b, SONG_B, 30)
        self.b.cmd(f"ASONG OPEN {SONG}", "ASONG:")
        wait_song_ready(self.b, SONG, 30)
        time.sleep(1)
        after = [(r["t"], r.get("kind"), r.get("blocks")) for r in self.tracks()[0]]
        if before != after:
            time.sleep(3)
            later = [(r["t"], r.get("kind"), r.get("blocks")) for r in self.tracks()[0]]
            self.pull_song("after")
            self.fail(f"the song came back different: {before} -> {after} (3 s later {later})")
        return f"{len(after)} tracks back as they were" if before == after else "DIFFERENT"

    ACTIONS = {
        "add_kit": 5, "add_kit2": 3, "add_pitch": 3, "add_audio": 5, "rec": 8, "dub": 5, "punch": 4, "storm": 6,
        "vpad": 2,
        "dup": 9, "delblock": 4, "move": 4, "cut": 4, "dupregion": 2, "paste": 2, "deltrack": 2, "freeze": 4,
        "arm": 4, "bounce": 3, "fx": 4, "kit_swap": 4, "trackkit": 3, "play": 5, "tempo": 2, "mix": 3, "reopen": 1,
    }

    def run(self) -> None:
        names = list(self.ACTIONS)
        weights = [self.ACTIONS[n] for n in names]
        t_start = time.time()
        while self.step < self.args.actions and time.time() - t_start < self.args.minutes * 60:
            self.step += 1
            self.guard()
            name = "add_kit" if self.step == 1 else self.rng.choices(names, weights)[0]
            t0 = time.time()
            result = getattr(self, "a_" + name)()
            dt = time.time() - t0
            st = asong(self.b)
            print(f"{self.step:4} {name:10} {dt:5.1f} s  {result}   [len={st.get('len')} clips={st.get('clips')} "
                  f"play={st.get('play')}]", flush=True)
            self.log.append({"step": self.step, "action": name, "s": round(dt, 1), "result": result})
            self.alive()
            if self.step % self.args.check_every == 0:
                self.check()
        self.b.cmd("ASONG STOP", "ASONG:")
        time.sleep(6)                                 # the stream pool gives its pages back after 4 s of quiet
        self.check()


def fresh(b: Board, name: str) -> None:
    b.cmd(f"ASONG OPEN {name}", "ASONG:")
    wait_song_ready(b, name, 30)
    if asong(b).get("clips", "0/0/0/0").strip("/0") or asong(b).get("len", "0") != "0":
        b.cmd("ASONG DELETE", "ASONG:")
        time.sleep(3)
        b.cmd(f"ASONG OPEN {name}", "ASONG:")
        wait_song_ready(b, name, 30)
    b.cmd("ASONG TEMPO 120", "ASONG:")
    b.cmd("ASONG LEN 0", "ASONG:")
    b.cmd("ASONG REGION 0 0", "ASONG:")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--actions", type=int, default=150)
    ap.add_argument("--minutes", type=float, default=60.0, help="stop after this long whatever the count")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--check-every", type=int, default=5)
    ap.add_argument("--leak-kb", type=int, default=256,
                    help="PSRAM the board may have less of at the end than before, back in the same song and kit")
    ap.add_argument("--keep", action="store_true", help="keep ZZ_TORTURE after a passing run")
    ap.add_argument("--listen", action="store_true", help="leave the speakers on")
    ap.add_argument("--json", type=Path, help="write the step log and verdict here")
    ap.add_argument("--evidence", type=Path, help="song.json pulled here at every reopen")
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else int(time.time()) % 100000
    print(f"seed {seed}", flush=True)

    t: Torture | None = None
    user = ""
    try:
        b = Board()
        song0 = song_name(b)
        kit0 = current_kit(b)
        psram_start = int(num(fields(b.cmd("MEM", "MEM:")).get("psram_free")) // 1024)
        # The native engine sizes a kit's heads by what is free when it loads
        # (16-64 kB a sample): the same kit can come back holding 600 kB more
        # or less, which is not a leak. Counted out of the comparison.
        heads_start = int(num(fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5)).get("heads_kb")))
        app = fields(b.cmd("UI_STATE", "UI:")).get("app")
        if app not in ("Arrange", "-", ""):
            raise BenchError(f"{app} is open: the board is in use")
        if not args.listen:
            b.cmd("AUDIO_MUTE 0 1", "OK")
            b.cmd("AUDIO_MUTE 1 1", "OK")
        try:
            b.cmd("ASONG STOP", "ASONG:")
            b.cmd(f"FS_MKDIR {FX_DIR}", "")
            fresh(b, SONG_B)
            fresh(b, SONG)
            if app != "Arrange":
                b.cmd("APP_START Arrange", "OK")
                time.sleep(2)
            drums = [k for k in DRUM_KITS if k in kit_ids(b)]
            if not drums:
                raise BenchError("none of the drums kits is on the card")
            load_kit(b, kit_ids(b)[drums[0]], timeout_s=40)
            t = Torture(b, random.Random(seed), args)
            t.run()
        except UserAtBoard as e:
            user = str(e)
        finally:
            b.cmd("ASONG STOP", "ASONG:")
            b.cmd("STM_VPAD STOP", "")
            passed = t is not None and not t.failures and not user
            if passed and not args.keep:
                for name in (SONG, SONG_B):
                    b.cmd(f"ASONG OPEN {name}", "ASONG:")
                    try:
                        wait_song_ready(b, name, 30)
                        b.cmd("ASONG DELETE", "ASONG:")
                        time.sleep(3)
                    except BenchError:
                        pass
            if song0:
                b.cmd(f"ASONG OPEN {song0}", "ASONG:")
            if kit0 >= 0:
                b.cmd(f"KIT_LOAD {kit0}", "OK")
            if not args.listen:
                b.cmd("AUDIO_MUTE 0 0", "OK")
                b.cmd("AUDIO_MUTE 1 0", "OK")
            if t is not None and not user:
                if song0:
                    try:
                        wait_song_ready(b, song0, 30)
                    except BenchError:
                        pass
                time.sleep(8)                         # the kit's take, then the pool's 4 s of quiet
                psram_end = int(num(fields(b.cmd("MEM", "MEM:")).get("psram_free")) // 1024)
                heads_end = int(num(fields(b.cmd("SMPL_ENGINE", "SMPLENG:", 2.5)).get("heads_kb")))
                short = psram_start - psram_end - (heads_end - heads_start)
                print(f"PSRAM before {psram_start} kB, after {psram_end} kB ({psram_end - psram_start:+d}); "
                      f"kit heads {heads_start} -> {heads_end} kB; short {short} kB")
                if short > args.leak_kb:
                    t.failures.append(f"PSRAM {short} kB short after the run (kit heads counted out), back in {song0}")
    except BenchError as e:
        print(f"BENCH: {e}")
        if t and t.failures:
            print("FAIL:\n  " + "\n  ".join(t.failures))
            return 1
        return 2

    if args.json and t:
        args.json.write_text(json.dumps({"seed": seed, "steps": t.log, "failures": t.failures,
                                         "overruns": t.overruns, "stopped_by_user": user}, indent=2))
    if user:
        print(f"STOPPED: {user} -- the board is yours again (seed {seed}, {t.step if t else 0} steps)")
        return 2
    assert t
    print(f"\n{t.step} actions, seed {seed}, render passes over their block: {t.overruns}")
    print("PASS" if not t.failures else "FAIL:\n  " + "\n  ".join(t.failures))
    return 1 if t.failures else 0


if __name__ == "__main__":
    sys.exit(main())
