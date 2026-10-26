#!/usr/bin/env python3
"""examples/infer/crowd.ent as written (no index) and built with
`--infer-indexes` (the indexes the compiler finds made: docs/indexes.md,
step 12). Built natively; on one core (pinned to CPU 2) and on all; the
best of three runs, which must say the same.

    bench/infer/run.py
"""
import os
import subprocess
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, "..", "..")
ent = os.path.join(root, "tools", "ent")
program = os.path.join(root, "examples", "infer", "crowd.ent")
work = tempfile.mkdtemp(prefix="infer-bench-")
env = dict(os.environ, ENT_CFLAGS="-march=native")
said = {}
print("| | one core s | all cores s |")
print("|---|---|---|")
for way, flags in (("as written", ["--no-index-remarks"]),
                   ("indexes found", ["--infer-indexes", "--no-index-remarks"])):
    times = []
    for kind, more in (("one", []), ("all", ["--parallel"])):
        out = os.path.join(work, way.replace(" ", "_") + "-" + kind)
        subprocess.run([ent, "build"] + flags + more + [program, "-o", out],
                       env=env, check=True)
        pin = ["taskset", "-c", "2"] if kind == "one" else []
        best = None
        for _ in range(3):
            start = time.perf_counter()
            text = subprocess.run(pin + [out], check=True, capture_output=True,
                                  text=True).stdout
            took = time.perf_counter() - start
            best = took if best is None else min(best, took)
            if said.setdefault("text", text) != text:
                raise SystemExit(f"{way} ({kind}) says otherwise")
        times.append(best)
    print(f"| {way} | {times[0]:.2f} | {times[1]:.2f} |")
