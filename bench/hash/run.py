#!/usr/bin/env python3
"""A hash index against a scan (bench/RESULTS.md, 2026-10-23).

200,000 ships of TEAMS teams, one in RATE changing team a frame, 500
frames, 10 or 100 reads a frame (`for x: Ship where x.team == t`), with
no index (a scan) or `index ByTeam of Ship by team hash` kept each way,
or none declared and the one the compiler finds made (`found`: ent build
--infer-indexes, docs/indexes.md step 12).
Built natively, on one core (pinned to CPU 2) and with --parallel; ms a
frame, the best of five rounds, the variants interleaved.

    bench/hash/run.py [teams rate ...]     (default: 1000 1000 1000 50 7 50)
"""
import os
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
ent = os.path.join(here, "..", "..", "tools", "ent")
ways = {"scan": "", "as fits": "index ByTeam of Ship by team hash",
        "incremental": "index ByTeam of Ship by team hash incremental",
        "rebuild": "index ByTeam of Ship by team hash rebuild",
        "found": ""}
args = sys.argv[1:] or ["1000", "1000", "1000", "50", "7", "50"]
cases = list(zip(args[0::2], args[1::2]))
source = open(os.path.join(here, "where.ent")).read()
work = tempfile.mkdtemp(prefix="hash-bench-")
env = dict(os.environ, ENT_CFLAGS="-march=native")
binaries = []
for teams, rate in cases:
    for reads in (10, 100):
        asks = " ".join("ask(%d)" % k for k in range(reads))
        for way, index in ways.items():
            text = (source.replace("INDEX", index).replace("TEAMS", teams)
                    .replace("RATE", rate).replace("ASKS", asks))
            stem = os.path.join(work, "%s-%s-%d-%s" % (teams, rate, reads,
                                                       way.replace(" ", "")))
            open(stem + ".ent", "w").write(text)
            for kind, flags in (("one", []), ("all", ["--parallel"])):
                out = stem + "-" + kind
                flags = flags + ["--no-index-remarks"] + (
                    ["--infer-indexes"] if way == "found" else [])
                subprocess.run([ent, "build"] + flags + [stem + ".ent", "-o",
                                                         out], env=env,
                               check=True, stdout=subprocess.DEVNULL)
                binaries.append(((teams, rate, reads, way, kind), out))
best = {}
for _ in range(5):
    for key, binary in binaries:
        pin = ["taskset", "-c", "2"] if key[4] == "one" else []
        start = time.perf_counter()
        subprocess.run(pin + [binary], check=True, stdout=subprocess.DEVNULL)
        ms = (time.perf_counter() - start) * 1000 / 500
        best[key] = min(best.get(key, ms), ms)
for teams, rate in cases:
    for reads in (10, 100):
        print("%s teams, one in %s changes, %d reads: " % (teams, rate, reads)
              + ", ".join("%s %.3f / %.3f" % (way, best[(teams, rate, reads,
                                                         way, "one")],
                                               best[(teams, rate, reads, way,
                                                     "all")])
                          for way in ways))
