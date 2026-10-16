#!/usr/bin/env python3
"""Runs entsat on SATLIB's random 3-SAT sets and checks every answer.

  examples/sat/fetch.sh
  tools/ent build examples/sat/entsat.ent -o build/sat/entsat
  examples/sat/check/satlib.py [set ...]      # uf50-218 uuf50-218 ...

A set named uf... is satisfiable, uuf... unsatisfiable. A SAT answer's
model is checked here against the file's clauses, apart from the check
entsat does itself; an UNSAT answer's DRAT proof goes to drat-trim. Any
other answer, a wrong one, or a proof drat-trim does not verify is a
failure. Prints a line per set and exits 1 if anything failed.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BUILD = os.path.join(ROOT, "build", "sat")
SOLVER = os.path.join(BUILD, "entsat")
DRAT_TRIM = os.path.join(BUILD, "tools", "drat-trim")
TIMEOUT = 300
SETS = ["uf50-218", "uuf50-218", "uf75-325", "uuf75-325", "uf100-430", "uuf100-430",
        "uf125-538", "uuf125-538", "uf150-645", "uuf150-645", "uf175-753", "uuf175-753",
        "uf200-860", "uuf200-860", "uf225-960", "uuf225-960", "uf250-1065", "uuf250-1065"]


def clauses(path):
    result, clause = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] in "cp":
                continue
            if line[0] == "%":
                break
            for word in line.split():
                lit = int(word)
                if lit == 0:
                    result.append(clause)
                    clause = []
                else:
                    clause.append(lit)
    if clause:
        result.append(clause)
    return result


def check(path, satisfiable):
    """None where entsat's answer checks out, else what is wrong."""
    with tempfile.TemporaryDirectory() as run:
        shutil.copy(path, os.path.join(run, "problem.cnf"))
        start = time.monotonic()
        try:
            out = subprocess.run([SOLVER], cwd=run, capture_output=True, text=True,
                                 timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return f"no answer in {TIMEOUT} s", TIMEOUT
        seconds = time.monotonic() - start
        lines = out.stdout.splitlines()
        status = [l for l in lines if l.startswith("s ")]
        if out.returncode != 0 or len(status) != 1:
            return f"exit {out.returncode}, {status}: {out.stderr.strip()[-200:]}", seconds
        if status[0] != ("s SATISFIABLE" if satisfiable else "s UNSATISFIABLE"):
            return status[0], seconds
        if satisfiable:
            model = set()
            for l in lines:
                if l.startswith("v "):
                    model.update(int(w) for w in l.split()[1:] if w != "0")
            for clause in clauses(path):
                if not any(lit in model for lit in clause):
                    return f"model leaves {clause} false", seconds
        else:
            proof = subprocess.run([DRAT_TRIM, os.path.join(run, "problem.cnf"),
                                    os.path.join(run, "proof.drat")],
                                   capture_output=True, text=True, timeout=600)
            if "s VERIFIED" not in proof.stdout:
                return "proof not verified: " + proof.stdout.strip()[-200:], seconds
        return None, seconds


def main():
    sets = sys.argv[1:] or SETS
    failed = 0
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        for name in sets:
            files = sorted(os.path.join(d, f)
                           for d, _, fs in os.walk(os.path.join(BUILD, "cnf", name))
                           for f in fs if f.endswith(".cnf"))
            satisfiable = name.startswith("uf")
            results = list(pool.map(lambda p: check(p, satisfiable), files))
            wrong = [(p, r) for p, (r, _) in zip(files, results) if r]
            times = [s for _, s in results]
            print(f"{name:12} {len(files):5} instances  {len(wrong)} wrong  "
                  f"mean {sum(times) / len(times) * 1000:7.1f} ms  max {max(times) * 1000:8.1f} ms",
                  flush=True)
            for p, r in wrong[:5]:
                print(f"  {os.path.basename(p)}: {r}")
            failed += len(wrong)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
