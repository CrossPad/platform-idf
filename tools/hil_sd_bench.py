#!/usr/bin/env python3
"""The card under N streaming voices: SD_BENCH over the grid of files (4, 8,
12, 16, 20) and piece sizes (4..64 kB), for each read path (port: what the
streamer does, stage: an 8 kB internal DMA stage, psram: straight to PSRAM).
Prints one row a run and a CSV to --csv. The song is stopped first: the
engine's own streaming would share the card.

  python tools/hil_sd_bench.py [--modes port,stage,psram] [--ms 3000] [--csv out.csv]
"""
from __future__ import annotations

import argparse
import sys
import time

from hil_perf_song import Board, fields

FILES = (4, 8, 12, 16, 20)
KB = (4, 8, 16, 32, 64)


def run(b: Board, line: str, secs: float) -> list[str]:
    assert b.s
    b.s.reset_input_buffer()
    b.s.write((line + "\r\n").encode())
    out = b._read(b.s, secs, until=b"SDBENCH: done").decode(errors="replace")
    return [l.strip() for l in out.splitlines() if l.startswith("SDBENCH:")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--modes", default="port,stage,psram")
    ap.add_argument("--ms", type=int, default=3000)
    ap.add_argument("--csv", default="")
    ap.add_argument("--prep", action="store_true", help="write the 20 files first")
    args = ap.parse_args()
    b = Board()
    b.cmd("ASONG STOP", "ASONG:")
    time.sleep(2)
    if args.prep:
        print("\n".join(run(b, "SD_BENCH PREP 2", 600)))
    rows = []
    for mode in args.modes.split(","):
        for n in FILES:
            for kb in KB:
                res = [l for l in run(b, f"SD_BENCH {n} {kb} {mode} {args.ms}", args.ms / 1000 + 20)
                       if "mbps=" in l or "ERR" in l]
                line = res[0] if res else f"SDBENCH: mode={mode} files={n} kb={kb} NO REPLY"
                print(line, flush=True)
                f = fields(line)
                rows.append((mode, n, kb, f.get("mbps"), f.get("avg_us"), f.get("p99_us"), f.get("worst_us"),
                             f.get("reads")))
    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("mode,files,kb,mbps,avg_us,p99_us,worst_us,reads\n")
            for r in rows:
                fh.write(",".join(str(x) for x in r) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
