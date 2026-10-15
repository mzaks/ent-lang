#!/usr/bin/env python3
"""Measures the honeybee colony (examples/beecs) and its reference, beecs.

  examples/beecs/reference/fetch.sh               once
  python3 bench/beecs/run.py [squadron sizes] [--seeds n] [--parallel] [--claims]
                             [--pgo]

For each squadron size (100, beecs's default, 10 and 1 by default: a
squadron of one is a forager by itself, some 10,000 to 20,000 entities)
both run a year of beecs's defaults otherwise, for seeds 1 to n (5), one
run a process, on a fast core (CPU 2; BENCH_CPU says another). Said: the
median of the runs' times, the whole process each (the model's reading its
weather and writing its days, beecs's setting up), and the most memory.
The model is built native (ENT_CFLAGS says otherwise), and with
--parallel also for all the cores, which run on all of them. With --claims
the model also forages without turns (claims.ent), each way it is built.
With --pgo each way is also built by its profile (tools/ent --instrument,
then --profile): the instrumented build runs a year of each squadron size
and each way of foraging for seeds that are not timed (101 and 102).
"""

import argparse
import os
import shutil
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


def write_parameters(work, seed, size, claims):
    with open(os.path.join(work, "parameters.txt"), "w") as f:
        f.write(f"RandomSeed.Seed {seed}\nForagers.SquadronSize {size}\n")
        if claims:
            f.write("Variant.Claims true\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sizes", nargs="*", type=int, default=[100, 10, 1])
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--parallel", action="store_true")
    parser.add_argument("--claims", action="store_true")
    parser.add_argument("--pgo", action="store_true")
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
        os.symlink(WEATHER, os.path.join(work, "foraging-period"))

        def build(extra, program):
            # (From the work folder: an instrumented build counts into
            # beecs.profile there.)
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"),
                            "build", *extra, PROGRAM, "-o", program],
                           check=True, cwd=work,
                           env=dict(os.environ, ENT_CFLAGS=flags),
                           stdout=subprocess.DEVNULL)

        programs = []
        for index, (name, extra, cpus) in enumerate(builds):
            ways = [(name, False)]
            if args.claims:
                ways.append((name.replace("model", "without turns"), True))
            program = os.path.join(work, f"model{index}")
            build(extra, program)
            for label, claims in ways:
                programs.append((label, program, cpus, claims))
            if not args.pgo:
                continue
            counts = os.path.join(work, "beecs.profile")
            shutil.rmtree(counts, ignore_errors=True)
            build(extra + ["--instrument"], program + "_counting")
            for size in args.sizes:
                for _, claims in ways:
                    for seed in (101, 102):
                        write_parameters(work, seed, size, claims)
                        timed([program + "_counting"], work, cpus)
            build(extra + ["--profile", counts], program + "_pgo")
            for label, claims in ways:
                programs.append((label + ", PGO", program + "_pgo", cpus, claims))
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
            for name, program, cpus, claims in programs:
                times, memory = [], 0.0
                for seed in range(1, args.seeds + 1):
                    write_parameters(work, seed, size, claims)
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
