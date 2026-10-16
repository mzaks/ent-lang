#!/usr/bin/env python3
"""Times entsat against MiniSat 2.2 and CaDiCaL on the fixed sample of SAT
Competition 2023 problems (bench/sat/sample.txt), as SAT competitions
score: how many are solved within the time limit, and PAR-2, the seconds
of each solved problem plus twice the limit for each that is not.

  bench/sat/fetch.sh
  ENT_CFLAGS=-march=native tools/ent build -I examples/sat bench/sat/entsat.ent -o build/sat/entsat-bench
  bench/sat/run.py [--limit 300] [--jobs 8] [--solvers entsat,minisat,cadical] [--fresh minisat]

Results are kept in build/sat/bench/<solver>-<limit>.json, so that a
solver already timed with that limit is not run again (--fresh says
which are). An answer two solvers disagree on is printed and makes the
exit status 1; entsat checks its own models against the clauses.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.path.join(ROOT, "build", "sat")
CNF = os.path.join(BUILD, "comp", "cnf")
SOLVERS = {
    "entsat": [os.path.join(BUILD, "entsat-bench")],
    # (A build being tried, beside the one timed last.)
    "entsat-dev": [os.path.join(BUILD, "entsat-dev")],
    "minisat": [os.path.join(BUILD, "tools", "minisat", "build", "release", "bin", "minisat"),
                "-verb=0"],
    "cadical": [os.path.join(BUILD, "tools", "cadical", "build", "cadical"), "-q"],
}


def answer(text):
    for line in text.splitlines():
        line = line.strip()
        if line in ("s SATISFIABLE", "SATISFIABLE"):
            return "sat"
        if line in ("s UNSATISFIABLE", "UNSATISFIABLE"):
            return "unsat"
    return None


def run(solver, hash_, limit):
    path = os.path.join(CNF, hash_ + ".cnf")
    with tempfile.TemporaryDirectory() as where:
        if solver.startswith("entsat"):
            os.symlink(path, os.path.join(where, "problem.cnf"))
            with open(os.path.join(where, "limit.txt"), "w") as f:
                f.write(f"{max(int(limit) - 1, 1)}\n")
            command = SOLVERS[solver]
        else:
            command = SOLVERS[solver] + [path]
        start = time.monotonic()
        try:
            out = subprocess.run(command, cwd=where, capture_output=True, text=True,
                                 timeout=limit)
            seconds = time.monotonic() - start
            result = answer(out.stdout)
            note = "" if result else (out.stdout + out.stderr).strip()[-300:]
            if not result and "s UNKNOWN" in out.stdout:
                seconds, note = limit, "time: " + " ".join(
                    l[2:] for l in out.stdout.splitlines() if l.startswith("c "))
        except subprocess.TimeoutExpired:
            seconds, result, note = limit, None, "time"
    return {"seconds": seconds, "answer": result, "note": note}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=float, default=300)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--solvers", default="entsat,minisat,cadical")
    parser.add_argument("--fresh", default="entsat")
    arguments = parser.parse_args()
    sample = [line.split() for line in open(os.path.join(os.path.dirname(__file__), "sample.txt"))]
    os.makedirs(os.path.join(BUILD, "bench"), exist_ok=True)
    solvers = arguments.solvers.split(",")
    fresh = set(arguments.fresh.split(",")) if arguments.fresh else set()
    results = {}
    for solver in solvers:
        kept = os.path.join(BUILD, "bench", f"{solver}-{arguments.limit:g}.json")
        if solver not in fresh and os.path.exists(kept):
            results[solver] = json.load(open(kept))
            continue
        with ThreadPoolExecutor(arguments.jobs) as pool:
            done = list(pool.map(lambda s: run(solver, s[0], arguments.limit), sample))
        results[solver] = {h: r for (h, _), r in zip(sample, done)}
        json.dump(results[solver], open(kept, "w"), indent=1)

    wrong = 0
    for hash_, name in sample:
        answers = {s: results[s][hash_]["answer"] for s in solvers if results[s][hash_]["answer"]}
        if len(set(answers.values())) > 1:
            wrong += 1
            print(f"disagree on {name}: {answers}")
    print(f"{len(sample)} problems, limit {arguments.limit:g} s, {arguments.jobs} at a time")
    print(f"{'solver':10} {'solved':>6} {'sat':>4} {'unsat':>5} {'PAR-2':>9}")
    par2 = {}
    for solver in solvers:
        r = results[solver].values()
        solved = [x for x in r if x["answer"]]
        par2[solver] = sum(x["seconds"] if x["answer"] else 2 * arguments.limit for x in r)
        print(f"{solver:10} {len(solved):6} {sum(x['answer'] == 'sat' for x in r):4} "
              f"{sum(x['answer'] == 'unsat' for x in r):5} {par2[solver]:9.0f}")
    for solver in par2:
        if solver.startswith("entsat") and "minisat" in par2:
            print(f"{solver} / minisat PAR-2: {par2[solver] / par2['minisat']:.2f}")
    errors = [(name, results["entsat"][h]["note"]) for h, name in sample
              if "entsat" in results and results["entsat"][h]["note"]
              and not results["entsat"][h]["note"].startswith("time")]
    for name, note in errors[:10]:
        print(f"entsat on {name}: {note[-200:]}")
    sys.exit(1 if wrong else 0)


if __name__ == "__main__":
    main()
