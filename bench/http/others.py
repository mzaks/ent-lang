#!/usr/bin/env python3
"""Measures the crawler against other crawlers over the network
(examples/http, http-SPEC.md §9.2, "Against other crawlers").

  python3 bench/http/others.py [pages ...] [--connections 64]
                               [--rounds 3] [--only NAME ...] [--keep DIR]

For each size (10000 100000 pages by default) site.py makes a site as
live.py does, and serve.c answers it on 127.0.0.1 (four threads, on CPUs
4 to 11). Each crawler crawls it from its seed through the server as a
proxy (so the site keeps its own URLs), with `connections` requests out
at most, robots.txt obeyed, no politeness delay, 2 tries after the first
where an answer may yet come right, 3 redirects followed and 2 s for an
answer, as near as each can be told to:

  c         bench/http/crawl.c (curl multi), on CPU 2: the reference
  ent       crawl_live.ent, built for one core, on CPU 2
  ent-par   crawl_live.ent, a --parallel build, on CPUs 0 to 3
  wget      wget --recursive, one connection, on CPU 2
  scrapy    Scrapy (bench/http/scrapy), on CPU 2
  colly     Colly (bench/http/colly), on CPUs 0 to 3
  spider    spider (bench/http/spider), on CPUs 0 to 3
  spider-mem  the same, its visited URLs all kept in memory (by default
            those past 15,000 go to SQLite on disk)

First each crawls once with the server's log on, and what it asked for
is held against what `c` asked for: the URLs it did not ask for, those
it asked for that `c` did not, and how many asks more or fewer there
were in all (a retry is an ask). Said, not hidden: where a crawler
cannot be made to do the same work, the numbers are of the work it did.
Then --rounds runs each, the log off: the median wall time, the CPU
time (user + system) and peak memory of that run, and pages a second
(of the URLs it asked for).

The other crawlers come from build/crawlers: `py` (a pixi environment
with wget and Scrapy), `colly` (go build in bench/http/colly) and
`spider-target` (cargo build --release in bench/http/spider);
bench/http/README.md says how they are made and their versions.
"""

import argparse
import collections
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
CRAWLERS = os.path.join(ROOT, "build", "crawlers")
PY = os.path.join(CRAWLERS, "py", ".pixi", "envs", "default", "bin")
sys.path.insert(0, HERE)
import live  # noqa: E402  (program_for, build_ent, set_conf)

NAMES = ["c", "ent", "ent-par", "wget", "scrapy", "colly", "spider",
         "spider-mem"]
RETRIES, REDIRECTS, TIMEOUT_MS = 2, 3, 2000


def commands(name, work, site, connections, proxy):
    """The command line and environment of a crawler, run in `site`."""
    env = dict(os.environ)
    one, four = ["taskset", "-c", "2"], ["taskset", "-c", "0-3"]
    seed, host = "http://site.test/", "site.test"
    if name == "c":
        return one + [os.path.join(work, "crawl_c")], env
    if name in ("ent", "ent-par"):
        cpus = one if name == "ent" else four
        return cpus + [os.path.join(work, f"crawl_live_{name}")], env
    if name == "wget":
        # (A directory of its own each run: wget keeps what it fetched,
        # to read its links again.)
        out = os.path.join(work, "wget-out")
        shutil.rmtree(out, ignore_errors=True)
        env["http_proxy"] = proxy
        return one + [os.path.join(PY, "wget"), "--recursive", "--level=inf",
                      "--page-requisites", "--no-verbose", "--quiet",
                      "--execute", "robots=on", f"--tries={RETRIES + 1}",
                      "--retry-on-http-error=429,500,502,503,504",
                      "--waitretry=0",
                      f"--max-redirect={REDIRECTS}",
                      f"--timeout={TIMEOUT_MS / 1000}", f"--domains={host}",
                      "--directory-prefix", out, seed], env
    if name == "scrapy":
        env["http_proxy"] = proxy
        settings = dict(
            ROBOTSTXT_OBEY=True, CONCURRENT_REQUESTS=connections,
            CONCURRENT_REQUESTS_PER_DOMAIN=connections, DOWNLOAD_DELAY=0,
            RETRY_TIMES=RETRIES, REDIRECT_MAX_TIMES=REDIRECTS,
            DOWNLOAD_TIMEOUT=TIMEOUT_MS / 1000, DEPTH_LIMIT=0,
            LOG_LEVEL="ERROR", TELNETCONSOLE_ENABLED=False)
        command = one + [os.path.join(PY, "scrapy"), "runspider",
                         os.path.join(HERE, "scrapy", "crawl_scrapy.py"),
                         "-a", f"seed={seed}", "-a", f"allow={host}"]
        for key, value in settings.items():
            command += ["-s", f"{key}={value}"]
        return command, env
    if name == "colly":
        return four + [os.path.join(CRAWLERS, "colly"), "-seed", seed,
                       "-allow", host, "-proxy", proxy,
                       "-conns", str(connections),
                       "-retries", str(RETRIES),
                       "-redirects", str(REDIRECTS),
                       "-timeout-ms", str(TIMEOUT_MS)], env
    if name in ("spider", "spider-mem"):
        # (spider keeps at most 15,000 URLs it visited in memory, the
        # others in SQLite on disk: spider-mem keeps them all in memory.)
        if name == "spider-mem":
            env["LINKS_VISITED_MEMORY_LIMIT"] = "100000000"
        return four + [os.path.join(CRAWLERS, "spider-target", "release",
                                    "spider-crawl"),
                       "--seed", seed, "--proxy", proxy,
                       "--conns", str(connections),
                       "--retries", str(RETRIES),
                       "--redirects", str(REDIRECTS),
                       "--timeout-ms", str(TIMEOUT_MS), "--resources"], env
    raise SystemExit(f"no crawler {name}")


def crawl(name, work, site, connections, log=None):
    """One crawl with the server up: wall s, CPU s, peak MB, exit status."""
    port_file = os.path.join(site, "port")
    if os.path.exists(port_file):
        os.remove(port_file)
    server_command = ["taskset", "-c", "4-11", os.path.join(work, "serve"),
                      "--threads", "4", "--port-file", port_file]
    if log:
        server_command += ["--log", log]
    server = subprocess.Popen(server_command +
                              [os.path.join(site, "site.manifest")],
                              stdout=subprocess.DEVNULL)
    try:
        while not os.path.exists(port_file):
            time.sleep(0.01)
        with open(port_file) as f:
            port = int(f.read())
        proxy = f"http://127.0.0.1:{port}"
        with open(os.path.join(site, "live.conf"), "w") as f:
            f.write(f"proxy {proxy}\nconnections {connections}\n")
        command, env = commands(name, work, site, connections, proxy)
        start = time.perf_counter()
        process = subprocess.Popen(command, cwd=site, env=env,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        _, status, usage = os.wait4(process.pid, 0)
        seconds = time.perf_counter() - start
    finally:
        server.terminate()
        server.wait()
    return (seconds, usage.ru_utime + usage.ru_stime, usage.ru_maxrss / 1024,
            os.waitstatus_to_exitcode(status))


def asked(log):
    """How many times each URL was asked for, by the server's log."""
    counts = collections.Counter()
    with open(log) as f:
        for line in f:
            parts = line.split(" ")
            if len(parts) >= 4:
                counts[parts[2]] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("pages", nargs="*", type=int, default=[10000, 100000])
    parser.add_argument("--connections", type=int, default=64)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only", nargs="*", default=NAMES)
    parser.add_argument("--keep", help="where to keep sites and programs")
    args = parser.parse_args()
    names = [n for n in NAMES if n == "c" or n in args.only]
    work = args.keep or tempfile.mkdtemp(prefix="ent-http-others-")
    os.makedirs(work, exist_ok=True)
    subprocess.run(["cc", "-O2", "-march=native", "-o",
                    os.path.join(work, "serve"),
                    os.path.join(EXAMPLE, "serve.c"), "-lpthread"], check=True)
    subprocess.run(["cc", "-O2", "-march=native", "-o",
                    os.path.join(work, "crawl_c"),
                    os.path.join(HERE, "crawl.c"), "-lcurl"], check=True)
    verified, measured = [], []
    for pages in args.pages:
        site = os.path.join(work, f"site-{pages}")
        if not os.path.exists(os.path.join(site, "expected.txt")):
            subprocess.run([sys.executable, os.path.join(EXAMPLE, "site.py"),
                            "--pages", str(pages), "--depth", "1000",
                            "--time-scale", "0.1", "--slow-ms", "10000",
                            "--timeout-ms", "20000", "--no-www",
                            "--out", site], check=True)
        live.set_conf(site, "per_host", args.connections)
        if "ent" in names or "ent-par" in names:
            source = live.program_for(pages, work)
            live.build_ent(source, os.path.join(work, f"crawl_live_ent"),
                           False)
            live.build_ent(source, os.path.join(work, f"crawl_live_ent-par"),
                           True)
        # Verified: what each asked for, against what c asked for.
        counts = {}
        for name in names:
            log = os.path.join(work, f"{name}-{pages}.log")
            *_, code = crawl(name, work, site, args.connections, log)
            counts[name] = asked(log)
            want, got = counts["c"], counts[name]
            missing = sorted(set(want) - set(got))
            more = sorted(set(got) - set(want))
            verified.append(
                f"| {pages} | {name} | {code} | {len(got):,} | "
                f"{sum(got.values()):,} | {len(missing):,} | {len(more):,} | "
                f"{', '.join(missing[:2] + more[:2])} |")
            print(verified[-1], flush=True)
        # Timed.
        for name in names:
            runs = sorted(crawl(name, work, site, args.connections)[:3]
                          for _ in range(args.rounds))
            seconds, cpu, peak = runs[len(runs) // 2]
            urls = len(counts[name])
            measured.append(
                f"| {pages} | {args.connections} | {name} | {seconds:.3f} | "
                f"{cpu:.3f} | {peak:.0f} | {urls / seconds:,.0f} |")
            print(measured[-1], flush=True)
    print("\n| pages | crawler | exit | URLs | asks | not asked | more "
          "| e.g. |\n|---|---|---|---|---|---|---|---|")
    print("\n".join(verified))
    print("\n| pages | connections | crawler | wall s | CPU s | peak MB "
          "| URLs/s |\n|---|---|---|---|---|---|---|")
    print("\n".join(measured))
    if not args.keep:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
