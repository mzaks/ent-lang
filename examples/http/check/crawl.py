#!/usr/bin/env python3
"""The crawler's checks on the mock (examples/http/http-SPEC.md, §8.2).

  examples/http/check/crawl.py [--parallel] [--keep DIR]

Builds crawl_mock.ent, crawl_lean.ent, crawl_combined.ent and
scan_links.ent once, generates sites with site.py (several sizes, seeds
and depths) and runs each program where its site is. Every report must
be site.py's expected one, which Python works out from the site alone:

- K1 to K6: crawl_mock's report, line for line (every page within the
  depth fetched once, broken links with the pages that link to them,
  redirect chains, loops and the limit, tries again and giving up, no
  page too deep or on another host, slow pages and timeouts);
- K7: crawl_lean's (without politeness, warnings, logging, history,
  clean-up), less the `slow` lines;
- K10: crawl_combined's (four steps in one system);
- K12: every link scan_links finds on every page, against Python's
  regular expressions and urllib.

`--parallel` builds the programs to run on all cores. Needs the compiler
built (build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
"""

import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(EXAMPLE))

# (pages, seed, depth)
SITES = [(50, 2, 8), (200, 1, 8), (1000, 3, 8), (300, 4, 3), (500, 5, 2)]


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


def run(exe, where):
    out = subprocess.run([exe], cwd=where, capture_output=True, text=True,
                         check=True).stdout
    return sorted({line for line in out.splitlines()
                   if line and not line.startswith("log ")})


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
    parser.add_argument("--keep", help="where to keep the sites and programs")
    args = parser.parse_args()
    generator = load_site_module()
    work = args.keep or tempfile.mkdtemp(prefix="ent-http-")
    os.makedirs(work, exist_ok=True)
    programs = {}
    for program in ("crawl_mock", "crawl_lean", "crawl_combined",
                    "scan_links"):
        programs[program] = os.path.join(work, program)
        build(program + ".ent", programs[program], args.parallel)
    failed = 0
    for pages, seed, depth in SITES:
        where = os.path.join(work, f"site-{pages}-{seed}-{depth}")
        subprocess.run([sys.executable, os.path.join(EXAMPLE, "site.py"),
                        "--pages", str(pages), "--seed", str(seed),
                        "--depth", str(depth), "--out", where], check=True)
        with open(os.path.join(where, "expected.txt")) as f:
            want = sorted(line for line in f.read().splitlines() if line)
        print(f"site of {pages} pages, seed {seed}, depth {depth}:")
        checks = [
            ("K1-K6 crawl_mock", run(programs["crawl_mock"], where), want),
            ("K7 crawl_lean", run(programs["crawl_lean"], where),
             [line for line in want if not line.startswith("slow ")]),
            ("K10 crawl_combined", run(programs["crawl_combined"], where),
             want),
            ("K12 scan_links", run(programs["scan_links"], where),
             expected_links(generator, where)),
        ]
        for name, got, expected in checks:
            failed += not compare(name, got, expected)
    print("all passed" if not failed else f"{failed} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
