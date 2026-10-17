#!/usr/bin/env python3
"""Times entsat against MiniSat 2.2 and CaDiCaL on the fixed sample of SAT
Competition 2023 problems (bench/sat/sample.txt), as SAT competitions
score: how many are solved within the time limit, and PAR-2, the seconds
of each solved problem plus twice the limit for each that is not.

  bench/sat/fetch.sh
  ENT_CFLAGS=-march=native tools/ent build -I examples/sat bench/sat/entsat.ent -o build/sat/entsat-bench
  bench/sat/run.py [--limit 300] [--cores 0-3] [--set sample] [--solvers entsat,minisat,cadical] [--fresh minisat]

Each problem runs on a core of its own, of those --cores names (as
taskset names them): the problems are timed on cores alike, the fast
ones by default. --set is the list of problems: sample.txt, or dev.txt,
those any solver has solved, for trying builds. Results are kept in
build/sat/bench/cores-<cores>/<solver>-<limit>.json, so that a problem a
solver was already timed on is not run again (--fresh says which
solvers are). An answer two solvers disagree on is printed and makes the
exit status 1; entsat checks its own models against the clauses.
"""
import argparse
import json
import os
import queue
import statistics
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
    # (A build being tried, beside the one timed last; any other
    # build/sat/entsat-<name> is timed as entsat-<name>.)
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


def run(solver, hash_, limit, cpu):
    path = os.path.join(CNF, hash_ + ".cnf")
    with tempfile.TemporaryDirectory() as where:
        if solver.startswith("entsat"):
            os.symlink(path, os.path.join(where, "problem.cnf"))
            with open(os.path.join(where, "limit.txt"), "w") as f:
                f.write(f"{max(int(limit) - 1, 1)}\n")
            command = SOLVERS.get(solver, [os.path.join(BUILD, solver)])
        else:
            command = SOLVERS[solver] + [path]
        start = time.monotonic()
        try:
            out = subprocess.run(["taskset", "-c", str(cpu)] + command, cwd=where,
                                 capture_output=True, text=True, timeout=limit)
            seconds = time.monotonic() - start
            result = answer(out.stdout)
            note = "" if result else (out.stdout + out.stderr).strip()[-300:]
            if not result and "s UNKNOWN" in out.stdout:
                seconds, note = limit, "time: " + " ".join(
                    l[2:] for l in out.stdout.splitlines() if l.startswith("c "))
        except subprocess.TimeoutExpired:
            seconds, result, note = limit, None, "time"
    return {"seconds": seconds, "answer": result, "note": note}


def cores(text):
    ids = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        ids += range(int(a), int(b or a) + 1)
    return ids


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=float, default=300)
    parser.add_argument("--cores", default="0-3")
    parser.add_argument("--set", default="sample")
    parser.add_argument("--solvers", default="entsat,minisat,cadical")
    parser.add_argument("--fresh", default="entsat")
    arguments = parser.parse_args()
    here = os.path.dirname(__file__)
    sample = [line.split() for line in open(os.path.join(here, arguments.set + ".txt"))]
    folder = os.path.join(BUILD, "bench", "cores-" + arguments.cores)
    os.makedirs(folder, exist_ok=True)
    solvers = arguments.solvers.split(",")
    fresh = set(arguments.fresh.split(",")) if arguments.fresh else set()
    free = queue.Queue()
    for cpu in cores(arguments.cores):
        free.put(cpu)

    def pinned(solver, hash_):
        cpu = free.get()
        try:
            return run(solver, hash_, arguments.limit, cpu)
        finally:
            free.put(cpu)

    results = {}
    for solver in solvers:
        kept = os.path.join(folder, f"{solver}-{arguments.limit:g}.json")
        results[solver] = json.load(open(kept)) if os.path.exists(kept) else {}
        todo = [h for h, _ in sample if solver in fresh or h not in results[solver]]
        if todo:
            with ThreadPoolExecutor(free.qsize()) as pool:
                done = list(pool.map(lambda h: pinned(solver, h), todo))
            results[solver].update(zip(todo, done))
            json.dump(results[solver], open(kept, "w"), indent=1)
        results[solver] = {h: results[solver][h] for h, _ in sample}

    wrong = 0
    for hash_, name in sample:
        answers = {s: results[s][hash_]["answer"] for s in solvers if results[s][hash_]["answer"]}
        if len(set(answers.values())) > 1:
            wrong += 1
            print(f"disagree on {name}: {answers}")
    print(f"{len(sample)} problems ({arguments.set}), limit {arguments.limit:g} s, "
          f"cores {arguments.cores}")
    print(f"{'solver':20} {'solved':>6} {'sat':>4} {'unsat':>5} {'PAR-2':>9}")
    par2 = {}
    for solver in solvers:
        r = results[solver].values()
        solved = [x for x in r if x["answer"]]
        par2[solver] = sum(x["seconds"] if x["answer"] else 2 * arguments.limit for x in r)
        print(f"{solver:20} {len(solved):6} {sum(x['answer'] == 'sat' for x in r):4} "
              f"{sum(x['answer'] == 'unsat' for x in r):5} {par2[solver]:9.0f}")
    # (Against MiniSat: PAR-2, and on the problems both solve where one
    # takes a second or more, the median and the geometric mean of the
    # times' ratio; those of milliseconds are how fast a program starts.)
    for solver in par2:
        if solver.startswith("entsat") and "minisat" in par2:
            m, e = results["minisat"], results[solver]
            both = [h for h, _ in sample if m[h]["answer"] and e[h]["answer"]
                    and max(m[h]["seconds"], e[h]["seconds"]) >= 1]
            ratios = [e[h]["seconds"] / max(m[h]["seconds"], 0.01) for h in both]
            median = statistics.median(ratios) if both else float("nan")
            mean = statistics.geometric_mean(ratios) if both else float("nan")
            print(f"{solver} / minisat: PAR-2 {par2[solver] / par2['minisat']:.2f}; "
                  f"on the {len(both)} both solve (1 s or more) median {median:.2f}, "
                  f"geometric mean {mean:.2f}")
    for solver in solvers:
        if solver.startswith("entsat"):
            errors = [(name, results[solver][h]["note"]) for h, name in sample
                      if results[solver][h]["note"]
                      and not results[solver][h]["note"].startswith("time")]
            for name, note in errors[:10]:
                print(f"{solver} on {name}: {note[-200:]}")
    sys.exit(1 if wrong else 0)


if __name__ == "__main__":
    main()
