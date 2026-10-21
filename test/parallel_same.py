#!/usr/bin/env python3
"""Each program of test/Integration/Inputs and examples says the same on
one thread as with every loop that can on many (`--parallel`,
ENT_PARALLEL_MIN=1, OMP_NUM_THREADS=8): built both ways, run both ways,
what each prints and how it ends compared byte for byte.

  parallel_same.py <ent driver...> -- <work dir> <program.ent...>

A program that is not built on one thread (one that needs C of its own,
or is not a whole program) is left out, as is one that opens a window (it
does not end by itself); one that is built on one thread but not for many
fails, as does one whose two runs differ. Builds and runs go side by side,
a core each.
"""
import concurrent.futures
import os
import subprocess
import sys


def main():
    split = sys.argv.index("--")
    driver, rest = sys.argv[1:split], sys.argv[split + 1:]
    work, programs = rest[0], rest[1:]
    os.makedirs(work, exist_ok=True)

    def run(program, exe):
        try:
            done = subprocess.run(
                [exe], cwd=os.path.dirname(program), stdin=subprocess.DEVNULL,
                capture_output=True, timeout=120,
                env=dict(os.environ, OMP_NUM_THREADS="8"))
            return done.stdout + done.stderr + b"\nrc=%d" % done.returncode
        except subprocess.TimeoutExpired:
            return b"timed out"

    def compare(program):
        with open(program, errors="replace") as source:
            if "import window" in source.read():
                return "window", program
        name = os.path.relpath(program).replace(os.sep, "_")
        one, many = (os.path.join(work, name + suffix)
                     for suffix in (".one", ".many"))
        built = subprocess.run(driver + ["build", program, "-o", one],
                               capture_output=True)
        if built.returncode:
            return "not built", program
        built = subprocess.run(
            driver + ["build", "--parallel", program, "-o", many],
            capture_output=True, env=dict(os.environ, ENT_PARALLEL_MIN="1"))
        if built.returncode:
            return "FAIL: not built for many threads", program
        if run(program, one) != run(program, many):
            return "FAIL: says other things on many threads", program
        return "same", program

    cores = max(1, (os.cpu_count() or 2) // 2)
    with concurrent.futures.ThreadPoolExecutor(cores) as pool:
        results = list(pool.map(compare, programs))
    counts = {}
    for verdict, program in results:
        counts[verdict] = counts.get(verdict, 0) + 1
        if verdict.startswith("FAIL"):
            print(f"{verdict}: {program}")
    for verdict in sorted(counts):
        print(f"{verdict}: {counts[verdict]}")
    return 1 if any(v.startswith("FAIL") for v in counts) else 0


if __name__ == "__main__":
    sys.exit(main())
