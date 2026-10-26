#!/usr/bin/env python3
"""Graph kernels over a relation (bench/graph/graph.ent) against the GAP
Benchmark Suite (github.com/sbeamer/gapbs) on the same Kronecker graph.

GAP is fetched at a fixed commit and built into build/bench/graph/gapbs with
the machine's own instruction set; its converter makes the graph (`-g
scale`), written once as an edge list for ent-lang and serialized for GAP.
bench/graph/check.cc, built on GAP's graph builder, says what the kernels
must find, and every ent-lang run is held against it.

Variants, each its own process, round-robin:
  ent / ent-par        graph.ent: PageRank and the search, so the links
                       are read both ways
  ent-pr / ent-pr-par  graph.ent without the search: the links are read
                       only into a page
  gap / gap-par        GAP's pr_spmv and bfs
One core is CPU 2 (taskset), all cores whatever OpenMP and ent-lang take.
Reports the median over --rounds of each process's best run, in ms.

Then churn.ent on one core, as built and sorting anew after every change
(ENT_EDGES_ANEW=1): k links let go of and k made each step, for k of 1,
100 and 10,000, then every page gathers along its links in: the ms a step
takes to rewire (with keeping the links in order) and to gather, from
the fourth step on, and the first three steps' rewiring in all (where the
links as laid get room). Both must end with the same hash.

Then pick.ent on one core, with the indexes of the links by weight and by
their ends (`//I`), without, and without with the ones the compiler
finds made (`found`: --infer-indexes): every page's three heaviest links out
(`by l.w last 3`), and its links to vertex 0 (`where v == hub`), of every
page and of those with more than 1,000 links out. The best of five runs
(the indexed ones' first, which makes the index, said apart); both must
find the same.

Usage: bench/graph/run.py [--scale 18] [--rounds 3] [--source 0]
"""

import argparse
import os
import re
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "bench", "graph")
GAP = os.path.join(OUT, "gapbs")
GAP_URL = "https://github.com/sbeamer/gapbs.git"
GAP_COMMIT = "941462c"
ENT = os.path.join(ROOT, "tools", "ent")
FLAGS = "-std=c++11 -O3 -march=native -fopenmp"


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def fetch_gap():
    if not os.path.isdir(GAP):
        os.makedirs(OUT, exist_ok=True)
        run(["git", "clone", "-q", GAP_URL, GAP])
        run(["git", "-C", GAP, "checkout", "-q", GAP_COMMIT])
    for kernel in ("pr_spmv", "bfs", "converter"):
        if not os.path.exists(os.path.join(GAP, kernel)):
            run(["make", "-C", GAP, kernel, "CXX=g++", f"CXX_FLAGS={FLAGS}"])
    check = os.path.join(OUT, "check")
    if not os.path.exists(check):
        run(["g++", "-O2", "-ffp-contract=off", "-std=c++11", "-fopenmp",
             "-I", os.path.join(GAP, "src"), os.path.join(HERE, "check.cc"),
             "-o", check])
    return check


def make_graph(scale):
    folder = os.path.join(OUT, f"kron{scale}")
    os.makedirs(folder, exist_ok=True)
    el = os.path.join(folder, "graph.el")
    sg = os.path.join(folder, "graph.sg")
    converter = os.path.join(GAP, "converter")
    if not os.path.exists(el):
        run([converter, "-g", str(scale), "-e", el])
    if not os.path.exists(sg):
        run([converter, "-f", el, "-b", sg])
    return folder, sg


def pr_only(source):
    """graph.ent without the search, which reads the links out of a page."""
    for name in ("down", "up"):
        start = source.index(f"system {name}() {{")
        end = source.index("\nsystem ", start + 5)
        source = source[:start] + source[end + 1:]
    return source.replace("  down() run_if B.mode == 0\n", "").replace(
        "  up() run_if B.mode == 1\n", "")


def build_ent():
    env = dict(os.environ, ENT_CFLAGS="-march=native")
    text = open(os.path.join(HERE, "graph.ent")).read()
    variant = os.path.join(OUT, "graph_pr.ent")
    open(variant, "w").write(pr_only(text))
    binaries = {}
    for name, program in (("ent", os.path.join(HERE, "graph.ent")),
                          ("ent-pr", variant)):
        for par in (False, True):
            out = os.path.join(OUT, name + ("-par" if par else ""))
            run([ENT, "build", "-I", HERE] + (["--parallel"] if par else []) +
                [program, "-o", out], env=env)
            binaries[name + ("-par" if par else "")] = out
    churn = os.path.join(OUT, "churn")
    run([ENT, "build", "-I", HERE, os.path.join(HERE, "churn.ent"), "-o", churn],
        env=env)
    picks = {}
    text = open(os.path.join(HERE, "pick.ent")).read()
    plain = os.path.join(OUT, "pick_plain.ent")
    open(plain, "w").write("".join(line for line in text.splitlines(True)
                                   if not line.rstrip().endswith("//I")))
    for name, program, flags in (
            ("indexed", os.path.join(HERE, "pick.ent"), []),
            ("plain", plain, ["--no-index-remarks"]),
            ("found", plain, ["--infer-indexes"])):
        out = os.path.join(OUT, "pick-" + name)
        run([ENT, "build", "-I", HERE] + flags + [program, "-o", out], env=env)
        picks[name] = out
    return binaries, churn, picks


def ent_times(output):
    times = {"pr": [], "bfs": []}
    lines = {}
    for line in output.splitlines():
        m = re.match(r"(pr|bfs) ([0-9.]+) ms, (.*)", line)
        if m:
            times[m.group(1)].append(float(m.group(2)))
            lines[m.group(1)] = f"{m.group(1)} {m.group(3)}"
    return {k: min(v) for k, v in times.items() if v}, lines


def gap_time(output):
    trials = [float(t) for t in re.findall(r"Trial Time:\s+([0-9.]+)", output)]
    return min(trials) * 1000.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", type=int, default=18)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--source", type=int, default=0)
    args = parser.parse_args()

    check = fetch_gap()
    folder, sg = make_graph(args.scale)
    want = {line.split(" ")[0]: line for line in
            run([check, "-f", sg, "-r", str(args.source)]).splitlines()
            if line.startswith(("pr ", "bfs "))}
    binaries, churn, picks = build_ent()
    one = ["taskset", "-c", "2"]
    results = {}
    for _ in range(args.rounds):
        for name, binary in binaries.items():
            pinned = [] if name.endswith("-par") else one
            out = run(pinned + [binary], cwd=folder)
            times, lines = ent_times(out)
            if name.startswith("ent-pr"):
                # (Without its steps the search goes nowhere.)
                times.pop("bfs", None)
                lines.pop("bfs", None)
            for kernel, line in lines.items():
                if line != want[kernel]:
                    raise SystemExit(f"{name}: {line!r}, want {want[kernel]!r}")
            for kernel, ms in times.items():
                results.setdefault((name, kernel), []).append(ms)
        for par in (False, True):
            env = dict(os.environ)
            if not par:
                env["OMP_NUM_THREADS"] = "1"
            pinned = [] if par else one
            name = "gap-par" if par else "gap"
            for kernel, exe, extra in (("pr", "pr_spmv", []),
                                       ("bfs", "bfs", ["-r", str(args.source)])):
                out = run(pinned + [os.path.join(GAP, exe), "-f", sg, "-n", "3"] + extra,
                          env=env)
                results.setdefault((name, kernel), []).append(gap_time(out))

    print(f"kron{args.scale}: {want['pr']}; {want['bfs']}")
    print(f"{'variant':<12} {'pr ms':>9} {'bfs ms':>9}")
    for name in ("ent", "ent-pr", "gap", "ent-par", "ent-pr-par", "gap-par"):
        row = [statistics.median(results[(name, k)]) if (name, k) in results else None
               for k in ("pr", "bfs")]
        print(f"{name:<12} " + " ".join(f"{v:9.2f}" if v is not None else f"{'':>9}"
                                         for v in row))

    steps = {}
    for _ in range(args.rounds):
        for way, extra in (("mended", {}), ("anew", {"ENT_EDGES_ANEW": "1"})):
            out = run(one + [churn], cwd=folder, env=dict(os.environ, **extra))
            for m in re.finditer(r"k (\d+): rewire ([0-9.]+) ms, gather ([0-9.]+) ms "
                                 r"a step, first ([0-9.]+) ms, hash (\d+)", out):
                k = int(m.group(1))
                steps.setdefault((way, k), []).append(
                    (float(m.group(2)), float(m.group(3)), m.group(5),
                     float(m.group(4))))
    print(f"{'churn k':<10} {'rewire':>9} {'gather':>9} {'first 3':>9} "
          f"{'anew':>9} {'gather':>9}")
    for k in (1, 100, 10000):
        hashes = {x[2] for way in ("mended", "anew") for x in steps[(way, k)]}
        if len(hashes) != 1:
            raise SystemExit(f"churn k {k}: hashes differ: {hashes}")
        mended = steps[("mended", k)]
        anew = steps[("anew", k)]
        med = lambda xs, i: statistics.median(x[i] for x in xs)
        print(f"{k:<10} {med(mended, 0):9.2f} {med(mended, 1):9.2f} "
              f"{med(mended, 3):9.2f} {med(anew, 0):9.2f} {med(anew, 1):9.2f}")

    found = {}
    for _ in range(args.rounds):
        for name, binary in picks.items():
            out = run(one + [binary], cwd=folder)
            lines = re.findall(r"(top|hub of more than \d+) ([0-9.]+) ms, (.*)", out)
            what = {}
            for kind, ms, value in lines:
                what.setdefault(kind, []).append((float(ms), value))
            for kind, runs in what.items():
                values = {v for _, v in runs}
                key = (kind,)
                if found.setdefault(key, values) != values:
                    raise SystemExit(f"pick {kind}: {values} vs {found[key]}")
                times = [ms for ms, _ in runs]
                found.setdefault((name, kind), []).append(
                    (min(times[1:]), times[0]))
    print(f"{'pick':<26} {'indexed':>9} {'(first)':>9} {'plain':>9} "
          f"{'found':>9} {'(first)':>9}")
    for kind in ("top", "hub of more than 0", "hub of more than 1000"):
        idx = found[("indexed", kind)]
        pl = found[("plain", kind)]
        fd = found[("found", kind)]
        print(f"{kind:<26} {statistics.median(x[0] for x in idx):9.2f} "
              f"{statistics.median(x[1] for x in idx):9.2f} "
              f"{statistics.median(x[0] for x in pl):9.2f} "
              f"{statistics.median(x[0] for x in fd):9.2f} "
              f"{statistics.median(x[1] for x in fd):9.2f}")


if __name__ == "__main__":
    main()
