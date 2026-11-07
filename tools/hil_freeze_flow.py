#!/usr/bin/env python3
"""Arrange's freeze, as it is worked with: a Drums take going round 16 bars
in a song of its own (ZZ_FRZ, deleted after), ARM moving on (a new track) --
the freeze starts at once and renders the block's two passes; frozen again
unchanged -- nothing rendered; the block cut in two -- only the new half's
render. Prints each step's time, the renders (pieces=done/needed) and what
the song plays as audio (segments). The song open before is opened again.

  python tools/hil_freeze_flow.py
"""
import time
from hil_perf_song import Board, fields, fresh_song, kit_ids, load_kit, record_track, DRUMS_PATTERN, song_name, asong

def seg(b):
    return fields(b.cmd("ASONG_DBG", "ASONGDBG:", 2.0)).get("seg")

def wait_freeze(b, label, started_by=None):
    t0 = time.time(); seen = False; st = {}
    while time.time() - t0 < 120:
        st = asong(b)
        if st.get("bounce") == "1": seen = True
        if seen and st.get("bounce") != "1": break
        if not seen and time.time() - t0 > 4: break
        time.sleep(0.25)
    print(f"{label}: ran={seen} took {time.time()-t0:.1f} s state={st.get('bounce')} pieces={st.get('pieces')} "
          f"file={st.get('file')} segments={seg(b)}", flush=True)

b = Board()
song0 = song_name(b)
ids = kit_ids(b, "POP_ICONIC")
fresh_song(b, "ZZ_FRZ")
b.cmd("ASONG LEN 16", "ASONG:"); b.cmd("ASONG REGION 0 16", "ASONG:")
b.cmd("APP_START Arrange", "OK"); time.sleep(2)
load_kit(b, ids["POP_ICONIC"])
b.cmd("ASONG SEQTRACK", "ASONG:")
record_track(b, DRUMS_PATTERN, 2)
b.cmd("ASONG REGION 0 0", "ASONG:")
print("blocks:", " | ".join(l for l in b.cmd("ASONG_SEQ 0", "ASEQ:").splitlines() if "bar=" in l))
t = time.time(); b.cmd("ASONG ARM -1", "ASONG:")
wait_freeze(b, "1. ARM moved on (new track)")
b.cmd("ASONG ARM 0", "ASONG:"); time.sleep(0.5); b.cmd("ASONG ARM -1", "ASONG:")
wait_freeze(b, "2. again, nothing changed")
b.cmd("ASONG ARM 0", "ASONG:"); time.sleep(0.3)
print(b.cmd("ASONG CUT 0 8", "ASONG:")[:20])
print("blocks:", " | ".join(l for l in b.cmd("ASONG_SEQ 0", "ASEQ:").splitlines() if "bar=" in l))
b.cmd("ASONG ARM -1", "ASONG:")
wait_freeze(b, "3. block cut in two")
b.cmd("ASONG DELETE", "ASONG:")
time.sleep(3)
b.cmd(f"ASONG OPEN {song0}", "ASONG:")
time.sleep(4)
print("open again:", song_name(b))
