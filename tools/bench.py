#!/usr/bin/env python3
"""Benchmark the rewrite against the original program on the same trace.

    python3 tools/bench.py --bin build [--runs 5]

Builds legacy/original with g++ -O2 into a temporary folder, then times
whole-process wall clock (read + parse + simulate + print) for both
programs on the configurations from the original README. Prints a
Markdown table with the median of N runs.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIGS = [(32768, 64, 4), (32768, 64, 2), (16384, 64, 2), (16384, 64, 8)]


def timed(cmd, stdin_text=None) -> tuple[float, str]:
    t0 = time.perf_counter()
    out = subprocess.run(cmd, input=stdin_text, capture_output=True, text=True, check=True)
    return time.perf_counter() - t0, out.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = ap.parse_args()

    cachesim = os.path.join(args.bin, "cachesim")
    trace = os.path.join(ROOT, "traces", "example.trc")
    if not os.path.exists(cachesim):
        sys.exit(f"missing {cachesim}; build first")
    if not shutil.which(args.cxx):
        sys.exit(f"compiler {args.cxx} not found")

    with tempfile.TemporaryDirectory() as tmp:
        legacy = os.path.join(ROOT, "legacy", "original")
        orig = os.path.join(tmp, "original")
        subprocess.run([args.cxx, "-std=c++11", "-O2", os.path.join(legacy, "Cache.cpp"),
                        os.path.join(legacy, "Main.cpp"), "-I", legacy, "-o", orig], check=True)

        print(f"machine: {platform.processor() or platform.machine()}, {os.cpu_count()} logical CPUs, "
              f"{platform.system()} {platform.release()}")
        print(f"trace: traces/example.trc (695,521 accesses), median of {args.runs} runs, wall clock per process\n")
        print("| config (size/line/ways) | original -O2 | rewrite | rewrite --no-3c | speed-up | same misses |")
        print("|---|---:|---:|---:|---:|:---:|")
        for size, block, ways in CONFIGS:
            cfg = os.path.join(tmp, "c.cfg")
            with open(cfg, "w") as f:
                f.write(f"Cache Size={size}\nBlock Size={block}\nAssociativity={ways}\n")
            o_times, n_times, f_times = [], [], []
            o_out = n_out = ""
            for _ in range(args.runs):
                t, o_out = timed([orig], f"{cfg}\n{trace}\n")
                o_times.append(t)
                t, n_out = timed([cachesim, "-q", "-s", str(size), "-b", str(block), "-a", str(ways), trace])
                n_times.append(t)
                t, _ = timed([cachesim, "-q", "--no-3c", "-s", str(size), "-b", str(block), "-a", str(ways), trace])
                f_times.append(t)
            o_miss = next(line.split()[-1] for line in o_out.splitlines() if line.startswith("MISSES"))
            n_miss = n_out.split("(")[1].split("/")[0]
            om, nm, fm = (statistics.median(x) * 1000 for x in (o_times, n_times, f_times))
            print(f"| {size} / {block} / {ways} | {om:.0f} ms | {nm:.0f} ms | {fm:.0f} ms | {om / fm:.1f}x "
                  f"| {'yes' if o_miss == n_miss else 'NO'} ({n_miss}) |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
