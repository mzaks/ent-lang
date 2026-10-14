#!/usr/bin/env python3
"""Measures the honeybee colony (examples/beecs) and its reference, beecs.

  examples/beecs/reference/fetch.sh               once
  python3 bench/beecs/run.py [squadron sizes] [--seeds n] [--parallel]

For each squadron size (100, beecs's default, 10 and 1 by default: a
squadron of one is a forager by itself, some 10,000 to 20,000 entities)
both run a year of beecs's defaults otherwise, for seeds 1 to n (5), one
run a process, on a fast core (CPU 2; BENCH_CPU says another). Said: the
median of the runs' times, the whole process each (the model's reading its
weather and writing its days, beecs's setting up), and the most memory.
The model is built native (ENT_CFLAGS says otherwise), and with
--parallel also for all the cores, which run on all of them.
"""

import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, "build", "beecs")
REFERENCE = os.path.join(BUILD, "ref_beecs")
WEATHER = os.path.join(BUILD, "run", "foraging-period")
PROGRAM = os.path.join(ROOT, "examples", "beecs", "beecs.ent")


def timed(command, cwd, cpus):
    """Wall seconds and the most memory [MB] of one run."""
    start = time.perf_counter()
    child = subprocess.Popen(["taskset", "-c", cpus] + command, cwd=cwd,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _, status, usage = os.wait4(child.pid, 0)
    seconds = time.perf_counter() - start
    if status != 0:
        sys.exit(f"{command[0]} failed")
    return seconds, usage.ru_maxrss / 1024.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sizes", nargs="*", type=int, default=[100, 10, 1])
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--parallel", action="store_true")
    args = parser.parse_args()
    if not os.path.exists(REFERENCE):
        sys.exit("no reference: run examples/beecs/reference/fetch.sh first")
    cpu = os.environ.get("BENCH_CPU", "2")
    flags = os.environ.get("ENT_CFLAGS", "-march=native")
    with tempfile.TemporaryDirectory(dir=BUILD) as work:
        builds = [("model, one core", [], cpu)]
        if args.parallel:
            builds.append(("model, all cores", ["--parallel"],
                           f"0-{os.cpu_count() - 1}"))
        programs = []
        for name, extra, cpus in builds:
            program = os.path.join(work, "model" + str(len(programs)))
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
                            *extra, PROGRAM, "-o", program], check=True,
                           env=dict(os.environ, ENT_CFLAGS=flags),
                           stdout=subprocess.DEVNULL)
            programs.append((name, program, cpus))
        os.symlink(WEATHER, os.path.join(work, "foraging-period"))
        print("| squadron size | | a year | memory |")
        print("|---|---|---|---|")
        for size in args.sizes:
            with open(os.path.join(work, "reference.json"), "w") as f:
                f.write('{"Foragers": {"SquadronSize": %d}}\n' % size)
            rows = []
            times, memory = [], 0.0
            for seed in range(1, args.seeds + 1):
                t, m = timed([REFERENCE, "-timing", "-params", "reference.json",
                              "-from", str(seed), "-to", str(seed)], work, cpu)
                times.append(t)
                memory = max(memory, m)
            rows.append(("beecs (Go), one core", statistics.median(times), memory))
            for name, program, cpus in programs:
                times, memory = [], 0.0
                for seed in range(1, args.seeds + 1):
                    with open(os.path.join(work, "parameters.txt"), "w") as f:
                        f.write(f"RandomSeed.Seed {seed}\nForagers.SquadronSize {size}\n")
                    t, m = timed([program], work, cpus)
                    times.append(t)
                    memory = max(memory, m)
                rows.append((name, statistics.median(times), memory))
            for i, (name, seconds, memory) in enumerate(rows):
                label = str(size) if i == 0 else ""
                print(f"| {label} | {name} | {seconds * 1000:,.1f} ms | {memory:,.0f} MB |")
            os.remove(os.path.join(work, "parameters.txt"))


if __name__ == "__main__":
    main()
