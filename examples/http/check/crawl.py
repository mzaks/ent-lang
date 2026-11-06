#!/usr/bin/env python3
"""The crawler's checks on the mock (examples/http/http-SPEC.md, §8.2).

  examples/http/check/crawl.py [--parallel] [--live] [--keep DIR]

Builds crawl_mock.ent, crawl_lean.ent, crawl_combined.ent and
scan_links.ent once, generates sites with site.py (several sizes, seeds,
depths, robots.txt files and limits) and runs each program where its
site is. Every report must be site.py's expected one, which Python works
out from the site alone:

- K1 to K6: crawl_mock's report, line for line (every page within the
  depth fetched once, broken links with the pages that link to them,
  redirect chains, loops and the limit, tries again and giving up, no
  page too deep or on another host, slow pages and timeouts);
- K7: crawl_lean's (without politeness, warnings, logging, history,
  clean-up), less the `slow` lines;
- K10: crawl_combined's (four steps in one system);
- K12: every link scan_links finds on every page, against Python's
  regular expressions and urllib;
- K9: from crawl_mock's history (`history` lines), no host had more
  asks out at once than `per_host`, and no two asks to a host went out
  closer than its robots.txt's `Crawl-delay` (its own robots.txt aside);
- K11: the sites with a robots.txt (rules, answered late, never
  answered) give their expected reports, so what robots.txt disallows is
  never asked for.

`--live` checks crawl_live.ent too, on every site made again with its
times a tenth as long, against serve.c (built with cc): K8, its report
is the expected one, which is the mock's; K9 on its history.

`--parallel` builds the programs to run on all cores. Needs the compiler
built (build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
"""

import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(EXAMPLE))

# (pages, seed, depth, robots.txt, asks out to a host at once)
SITES = [(50, 2, 8, "none", 64), (200, 1, 8, "none", 64),
         (1000, 3, 8, "none", 64), (300, 4, 3, "none", 64),
         (500, 5, 2, "none", 64), (200, 6, 8, "rules", 4),
         (200, 7, 8, "slow", 8), (100, 8, 4, "timeout", 64)]


def load_site_module():
    spec = importlib.util.spec_from_file_location(
        "site_generator", os.path.join(EXAMPLE, "site.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(program, out, parallel):
    command = [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
               "-I", EXAMPLE, os.path.join(EXAMPLE, program), "-o", out]
    if parallel:
        command.insert(3, "--parallel")
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def run(exe, where, history=None):
    out = subprocess.run([exe], cwd=where, capture_output=True, text=True,
                         check=True, timeout=600).stdout
    if history is not None:
        history.extend(line for line in out.splitlines()
                       if line.startswith("history "))
    return sorted({line for line in out.splitlines()
                   if line and not line.startswith(("log ", "history "))})


def crawl_delay(where):
    """The `Crawl-delay` of the site's robots.txt, in seconds (0: none, or
    where it is not answered in time)."""
    with open(os.path.join(where, "site.manifest"), "rb") as manifest:
        data = manifest.read()
    with open(os.path.join(where, "crawl.conf")) as conf:
        timeout_ms = int(dict(line.split(" ", 1) for line in
                              conf.read().splitlines())["timeout_ms"])
    at = data.find(b"/robots.txt 200 ")
    if at < 0:
        return 0.0
    end = data.index(b"\n", at)
    fields = data[at:end].split(b" ")
    length = int(fields[-1])
    if int(fields[2]) > timeout_ms:
        return 0.0
    for line in data[end + 1:end + 1 + length].decode().splitlines():
        if line.lower().startswith("crawl-delay:"):
            return float(line.split(":", 1)[1])
    return 0.0


def polite(name, history, per_host, delay):
    """K9 from the history: (sent, answered, status, url) a row."""
    hosts = {}
    for line in history:
        _, sent, at, _, url = line.split(" ", 4)
        hosts.setdefault(url.split("/")[2], []).append(
            (float(sent), float(at), url))
    problems = []
    for host, rows in hosts.items():
        # (An answer that comes as another goes out is no longer out.)
        events = sorted([(sent, 1) for sent, _, _ in rows] +
                        [(at, -1) for _, at, _ in rows])
        out = most = 0
        for _, step in events:
            out += step
            most = max(most, out)
        if most > per_host:
            problems.append(f"{host}: {most} out at once, {per_host} allowed")
        sends = sorted(sent for sent, _, url in rows
                       if not url.endswith("/robots.txt"))
        gaps = [b - a for a, b in zip(sends, sends[1:])]
        if delay and gaps and min(gaps) < delay - 1e-9:
            problems.append(f"{host}: asks {min(gaps):.4f} s apart, "
                            f"robots.txt wants {delay}")
    if not history:
        problems.append("no history")
    if problems:
        print(f"  {name}: FAILED, " + "; ".join(problems))
        return False
    print(f"  {name}: ok ({len(history)} answers)")
    return True


class Server:
    """serve.c on a site's manifest, while a crawl runs."""

    def __init__(self, exe, where):
        self.where = where
        port_file = os.path.join(where, "port")
        if os.path.exists(port_file):
            os.remove(port_file)
        self.process = subprocess.Popen(
            [exe, "--port-file", port_file,
             os.path.join(where, "site.manifest")],
            stdout=subprocess.DEVNULL)
        while not os.path.exists(port_file):
            if self.process.poll() is not None:
                raise SystemExit("serve did not start")
            time.sleep(0.01)
        with open(port_file) as f:
            self.port = int(f.read())

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.process.terminate()
        self.process.wait()


def compare(name, got, want):
    if got == want:
        print(f"  {name}: ok ({len(want)} lines)")
        return True
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))
    print(f"  {name}: FAILED, {len(missing)} missing, {len(extra)} more")
    for line in missing[:5]:
        print(f"    missing: {line}")
    for line in extra[:5]:
        print(f"    more:    {line}")
    return False


def expected_links(generator, where):
    """The links site.py's own reading finds on every HTML page."""
    site = generator.Site.__new__(generator.Site)
    lines = set()
    with open(os.path.join(where, "site.manifest"), "rb") as manifest:
        data = manifest.read()
    at = 0
    while at < len(data):
        end = data.index(b"\n", at)
        url, status, delay, fails, kind, location, length = \
            data[at:end].decode().split(" ")
        body = data[end + 1:end + 1 + int(length)].decode()
        at = end + 1 + int(length) + 1
        if "html" in kind:
            for target in site.links_of(url, {"body": body}):
                lines.add(f"link {url} {target}")
    return sorted(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--parallel", action="store_true")
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--keep", help="where to keep the sites and programs")
    args = parser.parse_args()
    generator = load_site_module()
    work = args.keep or tempfile.mkdtemp(prefix="ent-http-")
    os.makedirs(work, exist_ok=True)
    programs = {}
    names = ["crawl_mock", "crawl_lean", "crawl_combined", "scan_links"]
    if args.live:
        names.append("crawl_live")
        programs["serve"] = os.path.join(work, "serve")
        subprocess.run(["cc", "-O2", "-o", programs["serve"],
                        os.path.join(EXAMPLE, "serve.c"), "-lpthread"],
                       check=True)
    for program in names:
        programs[program] = os.path.join(work, program)
        build(program + ".ent", programs[program], args.parallel)
    failed = 0
    for pages, seed, depth, robots, per_host in SITES:
        where = os.path.join(work, f"site-{pages}-{seed}-{depth}-{robots}")
        made = [sys.executable, os.path.join(EXAMPLE, "site.py"),
                "--pages", str(pages), "--seed", str(seed),
                "--depth", str(depth), "--robots", robots,
                "--per-host", str(per_host), "--no-www"]
        subprocess.run(made + ["--out", where], check=True)
        with open(os.path.join(where, "expected.txt")) as f:
            want = sorted(line for line in f.read().splitlines() if line)
        print(f"site of {pages} pages, seed {seed}, depth {depth}, "
              f"robots.txt {robots}, {per_host} to a host:")
        history = []
        mock = run(programs["crawl_mock"], where, history)
        checks = [
            ("K1-K6, K11 crawl_mock", mock, want),
            ("K7 crawl_lean", run(programs["crawl_lean"], where),
             [line for line in want if not line.startswith("slow ")]),
            ("K10 crawl_combined", run(programs["crawl_combined"], where),
             want),
            ("K12 scan_links", run(programs["scan_links"], where),
             expected_links(generator, where)),
        ]
        for name, got, expected in checks:
            failed += not compare(name, got, expected)
        failed += not polite("K9 crawl_mock", history, per_host,
                             crawl_delay(where))
        if not args.live:
            continue
        # The same site, its times a tenth as long, over the network.
        live = where + "-live"
        subprocess.run(made + ["--time-scale", "0.1", "--out", live],
                       check=True)
        with open(os.path.join(live, "expected.txt")) as f:
            want = sorted(line for line in f.read().splitlines() if line)
        with Server(programs["serve"], live) as server:
            with open(os.path.join(live, "live.conf"), "w") as f:
                f.write(f"proxy http://127.0.0.1:{server.port}\n"
                        f"connections 64\n")
            history = []
            got = run(programs["crawl_live"], live, history)
        failed += not compare("K8 crawl_live", got, want)
        failed += not polite("K9 crawl_live", history, per_host,
                             crawl_delay(live))
    print("all passed" if not failed else f"{failed} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
