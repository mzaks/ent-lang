#!/usr/bin/env python3
"""One core and all of them give the same epidemic (the spec's fifth
check).

  examples/epidemic/reference/fetch.sh      once
  examples/epidemic/check/same.py

Builds check/same_check.ent twice, for one core and for all of them
(where its systems are also fused), and runs it: the build for one
core, and the other on 1, 4 and 16 threads and on as many as the machine
has. Every run writes for each day a number made of everything about
every one of 100,000 people; the runs are to be the same line for line.
Says the first day on which one is not. Needs the compiler built
(build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
RUN = os.path.join(ROOT, "build", "epidemic", "run")


def build(name, flags):
    program = os.path.join(RUN, name)
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build"] + flags +
        ["-I", os.path.join(HERE, ".."), os.path.join(HERE, "same_check.ent"),
         "-o", program], check=True, env=environment)
    return program


def run(program, threads):
    environment = dict(os.environ)
    if threads:
        environment["OMP_NUM_THREADS"] = str(threads)
    return subprocess.run([program], check=True, capture_output=True, text=True,
                          cwd=RUN, env=environment).stdout.split("\n")


def main():
    if not os.path.isfile(os.path.join(RUN, "baseline_household_demographics.csv")):
        sys.exit("no household panel: run examples/epidemic/reference/fetch.sh")
    parameters = os.path.join(RUN, "parameters.csv")
    if os.path.isfile(parameters):
        os.remove(parameters)
    one = run(build("same_one", []), None)
    all_cores = build("same_all", ["--parallel"])
    failed = 0
    for threads in (1, 4, 16, None):
        other = run(all_cores, threads)
        name = f"{threads} threads" if threads else "all the cores"
        if other == one:
            print(f"for all the cores, on {name}: the same {len(one) - 1} days")
            continue
        failed += 1
        day = next(i for i, (a, b) in enumerate(zip(one, other)) if a != b)
        print(f"for all the cores, on {name}: not the same from day {day}")
        print(f"  one core: {one[day]}\n  this run: {other[day]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
