#!/usr/bin/env python3
"""`Name.get(key)` of a hash index, ns a lookup (bench/RESULTS.md,
2026-10-23): 1,000, 200,000 and 4,000,000 keys, in a row or at random,
found or not; the loop without the lookups taken off. Built natively, on
CPU 2, the best of five. Compare with bench/hash/swiss.cpp:

    clang++ -O2 -DNDEBUG -march=native -std=c++17 bench/hash/swiss.cpp \\
      -o swiss $(pkg-config --libs absl_hash absl_raw_hash_set)
"""
import os
import subprocess
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
ent = os.path.join(here, "..", "..", "tools", "ent")
source = open(os.path.join(here, "get.ent")).read()
work = tempfile.mkdtemp(prefix="hash-get-")
env = dict(os.environ, ENT_CFLAGS="-march=native")
keys = {"in a row": "i", "at random": "(wyrand(1, i) >> 1) as i64"}
runs = []
for count in (1000, 200000, 4000000):
    for kind, key in keys.items():
        for miss in (0, 1):
            for ask in ("true", "false"):
                text = (source.replace("KEYS", str(count)).replace("KEY", key)
                        .replace("MISS", str(miss)).replace("ASK", ask))
                stem = os.path.join(work, "get-%d-%s-%d-%s" % (
                    count, kind.replace(" ", ""), miss, ask))
                open(stem + ".ent", "w").write(text)
                subprocess.run([ent, "build", stem + ".ent", "-o", stem],
                               env=env, check=True, stdout=subprocess.DEVNULL)
                runs.append(((count, kind, miss, ask), stem))
best = {}
for _ in range(5):
    for key, binary in runs:
        start = time.perf_counter()
        subprocess.run(["taskset", "-c", "2", binary], check=True,
                       stdout=subprocess.DEVNULL)
        took = time.perf_counter() - start
        best[key] = min(best.get(key, took), took)
for count in (1000, 200000, 4000000):
    for kind in keys:
        ns = [(best[(count, kind, miss, "true")] -
               best[(count, kind, miss, "false")]) * 1e9 / 32e6
              for miss in (0, 1)]
        print("%7d keys %-9s | found %5.2f | not %5.2f ns" % (count, kind,
                                                              ns[0], ns[1]))
