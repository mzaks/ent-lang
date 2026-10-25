#!/usr/bin/env python3
"""Gravity by a tree, over the cells of a Morton index, against Barnes's
treecode 1.4 (NEMO's src/nbody/evolve/hackcode/treecode1), on
treecode's own Plummer model: all held against the direct sum at the
same theta, with and without the quadrupoles. Two ports: a walk for each
body (bench/treecode/forces.ent, `for c in Bodies.cells`), and a walk
for each cube of bodies with lists of what passed, as treecode's own
(groups.ent); both import plummer.ent.

treecode is fetched from NEMO at a fixed commit and built on its own,
NEMO's pieces it takes in bench/treecode/shim, with BINARYIO and the
machine's instruction set. It makes the model (`nbody`, `seed`) and its
forces at each theta (`tstop=0 options=out-phi,out-acc`) into
`plummer.dat`, which the ports read: the bodies, and treecode's
accelerations to hold against their own direct sum.

Times on one core (CPU 2, taskset), and of ent on all: treecode's force
calculation as it reports it (CPUfc, its cputime in seconds here), each
port's best of 5 (making the index anew each time, which is not in it).
Interactions for each body: treecode's body-body and body-cell, the
ports' bodies and cells, which are the same (ent's index has treecode's
root cube and a body at each leaf: the same cells, opened for the same
bodies; checked). `apart`: the most a port's acceleration of a body is
from treecode's, over how big that is (the same terms, added in another
order), of both ports.

Usage: bench/treecode/run.py [--nbody 4096] [--seed 123]
                             [--thetas 1.0,0.75,0.5]
"""

import argparse
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "bench", "treecode")
SRC = os.path.join(OUT, "src")
NEMO_URL = "https://raw.githubusercontent.com/teuben/nemo"
NEMO_COMMIT = "8588424b9eb255539c34ce303aea1dcc7ecafa4b"
NEMO_PATH = "src/nbody/evolve/hackcode/treecode1"
SOURCES = ["treecode.c", "treecode.h", "treedefs.h", "treegrav.c",
           "treeio.c", "treeload.c"]
ENT = os.path.join(ROOT, "tools", "ent")


def run(command, **kw):
    return subprocess.run(command, check=True, text=True,
                          capture_output=True, **kw).stdout


def build_treecode():
    os.makedirs(SRC, exist_ok=True)
    for name in SOURCES:
        path = os.path.join(SRC, name)
        if not os.path.exists(path):
            run(["curl", "-sfL", "-o", path,
                 f"{NEMO_URL}/{NEMO_COMMIT}/{NEMO_PATH}/{name}"])
    binary = os.path.join(OUT, "treecode")
    shim = os.path.join(HERE, "shim")
    run(["cc", "-std=gnu99", "-O3", "-march=native", "-DBINARYIO",
         "-I", shim] + [os.path.join(SRC, n) for n in SOURCES
                        if n.endswith(".c")] +
        [os.path.join(shim, "shim.c"), "-lm", "-o", binary])
    return binary


def treecode(binary, folder, args, theta, quad):
    data = os.path.join(folder, "plummer.dat")
    if os.path.exists(data):
        os.remove(data)
    output = run(["taskset", "-c", "2", binary, "out=plummer.dat", "tstop=0",
                  "options=out-phi,out-acc", f"nbody={args.nbody}",
                  f"seed={args.seed}", f"theta={theta}",
                  f"usequad={'true' if quad else 'false'}"], cwd=folder)
    # rsize tdepth ftree actmax nbbtot nbctot CPUfc
    row = re.search(r"CPUfc\s*\n\s*(\S+)\s+(\d+)\s+(\S+)\s+(\d+)\s+(\d+)\s+"
                    r"(\d+)\s+(\S+)", output)
    return int(row.group(5)), int(row.group(6)), float(row.group(7)) * 1000.0


def build_ent(folder, port, theta, quad, parallel):
    text = open(os.path.join(HERE, port + ".ent")).read()
    for old, new in (("G.theta = 1.0", f"G.theta = {theta}"),
                     ("G.quad = false", f"G.quad = {'true' if quad else 'false'}")):
        assert old in text
        text = text.replace(old, new)
    program = os.path.join(folder, port + ".ent")
    open(program, "w").write(text)
    binary = os.path.join(folder, port + ("-par" if parallel else ""))
    env = dict(os.environ, ENT_CFLAGS="-march=native")
    run([ENT, "build", "-I", HERE] + (["--parallel"] if parallel else []) +
        [program, "-o", binary], env=env)
    return binary


def ent(binary, folder, parallel):
    output = run(([] if parallel else ["taskset", "-c", "2"]) + [binary],
                 cwd=folder)
    mine = re.search(r"ent: mean error (\S+) worst (\S+); cells (\d+) "
                     r"bodies (\d+)", output)
    theirs = re.search(r"treecode: mean error (\S+) worst (\S+)", output)
    apart = float(re.search(r"against treecode: at most (\S+)e-15",
                            output).group(1)) * 1e-15
    best = float(re.search(r"best walk (\S+) ms", output).group(1))
    return (float(mine.group(1)), float(mine.group(2)), int(mine.group(3)),
            int(mine.group(4)), float(theirs.group(1)),
            float(theirs.group(2)), apart, best)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nbody", type=int, default=4096)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--thetas", default="1.0,0.75,0.5")
    args = parser.parse_args()
    binary = build_treecode()
    folder = os.path.join(OUT, f"n{args.nbody}")
    os.makedirs(folder, exist_ok=True)
    print(f"nbody {args.nbody} seed {args.seed}; errors |a - direct| / "
          f"|direct|, mean and worst; times in ms")
    print(f"{'theta':>5} {'quad':>5} | {'treecode':>17} {'per body':>8} "
          f"{'ms':>6} | {'ent':>17} | {'walk':>6} {'all':>6} | "
          f"{'groups':>6} {'all':>6} | {'apart':>8}")
    for theta in args.thetas.split(","):
        for quad in (False, True):
            bb, bc, tc_ms = treecode(binary, folder, args, theta, quad)
            times, apart = [], 0.0
            for port in ("forces", "groups"):
                one = ent(build_ent(folder, port, theta, quad, False), folder,
                          False)
                every = ent(build_ent(folder, port, theta, quad, True), folder,
                            True)
                assert one[:7] == every[:7], (port, one, every)
                assert one[2:4] == (bc, bb), (port, one, bb, bc)
                mean, worst, cells, bodies, tmean, tworst = one[:6]
                apart = max(apart, one[6])
                times += [one[7], every[7]]
            print(f"{theta:>5} {str(quad).lower():>5} | {tmean:8.6f} "
                  f"{tworst:8.6f} {(bb + bc) / args.nbody:8.1f} {tc_ms:6.1f} | "
                  f"{mean:8.6f} {worst:8.6f} | {times[0]:6.1f} {times[1]:6.1f} | "
                  f"{times[2]:6.1f} {times[3]:6.1f} | {apart:8.1e}")

if __name__ == "__main__":
    main()
