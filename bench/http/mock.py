#!/usr/bin/env python3
"""Measures the crawler on the mock (examples/http, http-SPEC.md §9.1).

  python3 bench/http/mock.py [pages ...] [--keep DIR] [--trace]

For each size (10000 100000 1000000 pages by default) site.py makes a
site (without its www/ files; deep enough for every page that can be
reached to be), and a copy of crawl_mock.ent that holds so many asks and
links is built for one core and for all (native: ENT_CFLAGS says
otherwise). Each runs where the site is: on one fast core (CPU 2,
BENCH_ONE_CPU says otherwise) and on all, three times each, the median
said. Its report is compared with the site's expected one first: a run
that does not give it is said and not timed.

The time is the whole run: reading the site (site.manifest), the crawl,
the report. Pages a second are pages asked for over that time. With
--trace the builds time each system (ent build --trace) and the trace of
the last size on one core is printed.
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXAMPLE = os.path.join(ROOT, "examples", "http")


def program_for(pages, where):
    """crawl_mock.ent, holding enough for `pages`: the asks (and pages),
    the links (several a page), the history (an answer each, and the
    tries again)."""
    with open(os.path.join(EXAMPLE, "crawl_mock.ent")) as f:
        text = f.read()
    capacity = 1 << max(12, (pages * 11 // 10).bit_length())
    # (Without the history said at the end, which only the checks read.)
    text = text.replace(" say_history()", "")
    text = text.replace("capacity Going 131072", f"capacity Going {capacity // 2}")
    text = text.replace("default_capacity 262144",
                        f"default_capacity {capacity}\n"
                        f"capacity Links {capacity * 8}\n"
                        f"capacity History {capacity * 2}")
    path = os.path.join(where, f"crawl_{pages}.ent")
    with open(path, "w") as f:
        f.write(text)
    return path


def build(source, out, parallel, trace):
    env = dict(os.environ)
    env.setdefault("ENT_CFLAGS", "-march=native")
    command = [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
               "-I", EXAMPLE, source, "-o", out]
    if parallel:
        command.insert(3, "--parallel")
    if trace:
        command.insert(3, "--trace")
    subprocess.run(command, check=True, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def run(exe, where, cpus):
    command = [exe] if cpus is None else ["taskset", "-c", cpus, exe]
    start = time.perf_counter()
    done = subprocess.run(command, cwd=where, capture_output=True, text=True,
                          check=True)
    seconds = time.perf_counter() - start
    lines = sorted({line for line in done.stdout.splitlines()
                    if line and not line.startswith("log ")})
    return seconds, lines, done.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("pages", nargs="*", type=int,
                        default=[10000, 100000, 1000000])
    parser.add_argument("--keep", help="where to keep sites and programs")
    parser.add_argument("--trace", action="store_true")
    args = parser.parse_args()
    work = args.keep or tempfile.mkdtemp(prefix="ent-http-bench-")
    os.makedirs(work, exist_ok=True)
    one = os.environ.get("BENCH_ONE_CPU", "2")
    print(f"| pages | fetched | one core s | pages/s | all cores s | pages/s |")
    print(f"|---|---|---|---|---|---|")
    trace = ""
    for pages in args.pages:
        site = os.path.join(work, f"site-{pages}")
        if not os.path.exists(os.path.join(site, "expected.txt")):
            subprocess.run([sys.executable, os.path.join(EXAMPLE, "site.py"),
                            "--pages", str(pages), "--depth", "1000",
                            "--per-host", str(pages * 2), "--no-www",
                            "--out", site], check=True)
        with open(os.path.join(site, "expected.txt")) as f:
            want = sorted(line for line in f.read().splitlines() if line)
        fetched = sum(line.startswith("fetched ") for line in want)
        source = program_for(pages, work)
        row = [str(pages), str(fetched)]
        for parallel, cpus in ((False, one), (True, None)):
            exe = os.path.join(work, f"crawl_{pages}_{'all' if parallel else 'one'}")
            build(source, exe, parallel, args.trace)
            times = []
            for _ in range(3):
                seconds, got, stderr = run(exe, site, cpus)
                if got != want:
                    print(f"{pages} pages ({'all' if parallel else 'one'}): "
                          f"the report is not the expected one", file=sys.stderr)
                    sys.exit(1)
                times.append(seconds)
                if args.trace and not parallel:
                    trace = stderr
            median = statistics.median(times)
            row += [f"{median:.3f}", f"{fetched / median:,.0f}"]
        print("| " + " | ".join(row) + " |", flush=True)
    if trace:
        print(trace)
    if not args.keep:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
