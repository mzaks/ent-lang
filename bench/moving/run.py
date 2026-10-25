#!/usr/bin/env python3
"""A million dots moving in a square that wraps, each counting those
within 0.002 through an index (bench/moving/moving.ent): a grid of cells
0.004 and a Morton index of leaves of 16, each built three ways:

  cpu        --parallel: everything on the CPU's cores
  gpu, cpu   --gpu --parallel, ENT_INDEXES_ON_CPU=1: moving and counting
             on the GPU, the index made on the CPU (its rows copied over)
  gpu        --gpu --parallel: the index made on the GPU too

Native builds. Reports the median over --rounds of the ms a frame, and
checks that every build's sum of the counts is the same.

Usage: bench/moving/run.py [--rounds 3] [--dots 1000000] [--frames 50]
"""

import argparse
import os
import re
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "bench", "moving")
ENT = os.path.join(ROOT, "tools", "ent")
GRID = "grid (x, y) cell 0.004"
INDEXES = {"grid": GRID, "morton": "morton (x, y) leaf 16"}
WAYS = {"cpu": (["--parallel"], {}),
        "gpu, made on cpu": (["--gpu", "--parallel"], {"ENT_INDEXES_ON_CPU": "1"}),
        "gpu": (["--gpu", "--parallel"], {})}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--dots", type=int, default=1000000)
    parser.add_argument("--frames", type=int, default=50)
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    text = open(os.path.join(HERE, "moving.ent")).read()
    text = text.replace("Setup.dots = 1000000", f"Setup.dots = {args.dots}")
    text = text.replace("Setup.frames = 50", f"Setup.frames = {args.frames}")
    room = 1 << max(20, (args.dots - 1).bit_length())
    text = text.replace("capacity 1048576", f"capacity {room}")
    binaries = {}
    for index, line in INDEXES.items():
        program = os.path.join(OUT, f"moving_{index}.ent")
        open(program, "w").write(text.replace(GRID, line))
        for way, (flags, extra) in WAYS.items():
            out = os.path.join(OUT, f"moving_{index}_{way.replace(', ', '_').replace(' ', '_')}")
            env = dict(os.environ, ENT_CFLAGS="-march=native", **extra)
            subprocess.run([ENT, "build", "--no-cache"] + flags + [program, "-o", out],
                           check=True, env=env)
            binaries[(index, way)] = out
    times, sums = {}, {}
    for _ in range(args.rounds):
        for key, binary in binaries.items():
            said = subprocess.run([binary], check=True, capture_output=True,
                                  text=True).stdout
            ms, total = re.search(r"moving ([0-9.]+) ms a frame, sum (\d+)", said).groups()
            times.setdefault(key, []).append(float(ms))
            sums.setdefault(key[0], set()).add(total)
    for index, seen in sums.items():
        if len(seen) != 1:
            raise SystemExit(f"{index}: the builds' sums differ: {seen}")
    print(f"{args.dots} dots, ms a frame (median of {args.rounds})")
    print(f"{'index':<8} " + " ".join(f"{way:>17}" for way in WAYS))
    for index in INDEXES:
        print(f"{index:<8} " + " ".join(
            f"{statistics.median(times[(index, way)]):17.2f}" for way in WAYS))


if __name__ == "__main__":
    main()
