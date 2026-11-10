#!/usr/bin/env python3
"""Measures the crawler over the network against C (examples/http,
http-SPEC.md §9.1, "Live against C").

  python3 bench/http/live.py [pages ...] [--connections 64 256 1024]
                             [--rounds 3] [--keep DIR]

For each size (10000 100000 pages by default) site.py makes a site, its
times a tenth as long (as check/crawl.py --live has them) but for when
an ask is slow (1 s) and when it times out (2 s): no page comes near
either, so the report does not hang on how long a frame takes (with the
checks' 100 ms, a frame of crawl_live at 10^5 pages is longer, and asks
waiting on it were said to be slow). serve.c
answers it on 127.0.0.1 (four threads, on CPUs 4 to 11). Three crawlers
crawl it, each where the site is, with `connections` requests out at
most (to the host and in all: crawl.conf's `per_host` and live.conf's
`connections`):

  ent       crawl_live.ent, built for one core, on CPU 2
  ent-par   crawl_live.ent, a --parallel build, on CPUs 0 to 3
  c         bench/http/crawl.c (curl multi), on CPU 2

Each run's report is compared with the site's expected one first: one
that differs is said, and the bench stops. Said: the median wall time
over --rounds, the CPU time (user + system) and peak memory of that
run, and pages a second. Native builds (ENT_CFLAGS says otherwise).
"""

import argparse
import os
import resource
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
    """crawl_live.ent as it is (see mock.py), without the history said at
    the end."""
    with open(os.path.join(EXAMPLE, "crawl_live.ent")) as f:
        text = f.read()
    text = text.replace(" say_history()", "")
    path = os.path.join(where, f"crawl_live_{pages}.ent")
    with open(path, "w") as f:
        f.write(text)
    return path


def build_ent(source, out, parallel):
    env = dict(os.environ)
    env.setdefault("ENT_CFLAGS", "-march=native")
    command = [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
               "-I", EXAMPLE, source, "-o", out]
    if parallel:
        command.insert(3, "--parallel")
    subprocess.run(command, check=True, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def run(command, where):
    """Wall seconds, CPU seconds, of them in the system (the kernel: sockets,
    TCP), and peak memory (MB) of one run, and its report."""
    start = time.perf_counter()
    process = subprocess.Popen(command, cwd=where, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
    out = process.stdout.read()
    _, status, usage = os.wait4(process.pid, 0)
    seconds = time.perf_counter() - start
    if status != 0:
        raise SystemExit(f"{command[-1]} failed in {where}")
    lines = sorted({line for line in out.splitlines()
                    if line and not line.startswith(("log ", "history "))})
    return (seconds, usage.ru_utime + usage.ru_stime, usage.ru_stime,
            usage.ru_maxrss / 1024, lines)


def set_conf(where, key, value):
    path = os.path.join(where, "crawl.conf")
    with open(path) as f:
        lines = [line for line in f.read().splitlines()
                 if not line.startswith(key + " ")]
    lines.append(f"{key} {value}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("pages", nargs="*", type=int, default=[10000, 100000])
    parser.add_argument("--connections", nargs="*", type=int,
                        default=[64, 256, 1024])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--keep", help="where to keep sites and programs")
    args = parser.parse_args()
    work = args.keep or tempfile.mkdtemp(prefix="ent-http-live-")
    os.makedirs(work, exist_ok=True)
    serve = os.path.join(work, "serve")
    subprocess.run(["cc", "-O2", "-march=native", "-o", serve,
                    os.path.join(EXAMPLE, "serve.c"), "-lpthread"], check=True)
    crawl_c = os.path.join(work, "crawl_c")
    subprocess.run(["cc", "-O2", "-march=native", "-o", crawl_c,
                    os.path.join(HERE, "crawl.c"), "-lcurl"], check=True)
    print("| pages | fetched | connections | crawler | wall s | CPU s | "
          "system s | peak MB | pages/s |")
    print("|---|---|---|---|---|---|---|---|---|")
    for pages in args.pages:
        site = os.path.join(work, f"site-{pages}")
        if not os.path.exists(os.path.join(site, "expected.txt")):
            subprocess.run([sys.executable, os.path.join(EXAMPLE, "site.py"),
                            "--pages", str(pages), "--depth", "1000",
                            "--time-scale", "0.1", "--slow-ms", "10000",
                            "--timeout-ms", "20000", "--no-www",
                            "--out", site],
                           check=True)
        with open(os.path.join(site, "expected.txt")) as f:
            want = sorted(line for line in f.read().splitlines() if line)
        fetched = sum(line.startswith("fetched ") for line in want)
        source = program_for(pages, work)
        crawlers = {"c": ["taskset", "-c", "2", crawl_c]}
        for name, parallel, cpus in (("ent", False, "2"),
                                     ("ent-par", True, "0-3")):
            exe = os.path.join(work, f"crawl_live_{pages}_{name}")
            build_ent(source, exe, parallel)
            crawlers[name] = ["taskset", "-c", cpus, exe]
        for connections in args.connections:
            set_conf(site, "per_host", connections)
            times = {name: [] for name in crawlers}
            for _ in range(args.rounds):
                for name, command in crawlers.items():
                    port_file = os.path.join(site, "port")
                    if os.path.exists(port_file):
                        os.remove(port_file)
                    server = subprocess.Popen(
                        ["taskset", "-c", "4-11", serve, "--threads", "4",
                         "--port-file", port_file,
                         os.path.join(site, "site.manifest")],
                        stdout=subprocess.DEVNULL)
                    while not os.path.exists(port_file):
                        time.sleep(0.01)
                    with open(port_file) as f:
                        port = int(f.read())
                    with open(os.path.join(site, "live.conf"), "w") as f:
                        f.write(f"proxy http://127.0.0.1:{port}\n"
                                f"connections {connections}\n")
                    try:
                        seconds, cpu, system, peak, got = run(command, site)
                    finally:
                        server.terminate()
                        server.wait()
                    if got != want:
                        missing = sorted(set(want) - set(got))[:3]
                        more = sorted(set(got) - set(want))[:3]
                        raise SystemExit(
                            f"{name}, {pages} pages, {connections} "
                            f"connections: not the expected report; "
                            f"missing {missing}, more {more}")
                    times[name].append((seconds, cpu, system, peak))
            for name in crawlers:
                runs = sorted(times[name])
                seconds, cpu, system, peak = runs[len(runs) // 2]
                print(f"| {pages} | {fetched} | {connections} | {name} | "
                      f"{seconds:.3f} | {cpu:.3f} | {system:.3f} | {peak:.0f} | "
                      f"{fetched / seconds:,.0f} |", flush=True)
    if not args.keep:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
