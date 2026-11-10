#!/usr/bin/env python3
"""The crawler on a public site, held against mirror.py's fetch of it
(http-SPEC.md §9.3; run by hand, never by the checks).

  python3 bench/http/public.py DIR [--public] [--local]

DIR is where mirror.py put a site. First the link scanner (scan_links.ent,
K12 on real pages) reads every HTML page of the mirror, and what it finds
is held against links.txt, what Python's HTML parser found. Then:

  --public  crawl_live.ent crawls the site itself over the network, with
            mirror.py's caps (crawl.conf: its depth, its pages, a second
            between asks to a host at least, one ask out to a host), and
            its report is held against report.txt
  --local   the same against serve.c answering from the mirror, at full
            speed (no delay, 64 out at once): the mirror answers as the
            site did

What either finds that the other does not is said, a few of each kind.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXAMPLE = os.path.join(ROOT, "examples", "http")
sys.path.insert(0, HERE)
import live  # noqa: E402  (build_ent, set_conf)

KINDS = ("fetched ", "redirect ", "disallowed ", "capped ")


def build(source, out):
    env = dict(os.environ)
    env.setdefault("ENT_CFLAGS", "-march=native")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"),
                    "build", "-I", EXAMPLE, source, "-o", out],
                   check=True, env=env)


def report_lines(text):
    """The lines both reports have, a `fetched` line without its tries."""
    lines = set()
    for line in text.splitlines():
        if not line.startswith(KINDS):
            continue
        if line.startswith("fetched "):
            line = line.rsplit(" ", 1)[0]
        lines.add(line)
    return lines


def compare(what, ours, theirs, show=8):
    only_ours, only_theirs = sorted(ours - theirs), sorted(theirs - ours)
    print(f"{what}: {len(ours & theirs)} alike, {len(only_ours)} only ent's, "
          f"{len(only_theirs)} only mirror.py's")
    for line in only_ours[:show]:
        print(f"  ent:       {line}")
    for line in only_theirs[:show]:
        print(f"  mirror.py: {line}")
    return not only_ours and not only_theirs


def crawl(program, mirror, work, conf, proxy=None):
    os.makedirs(work, exist_ok=True)
    shutil.copy(os.path.join(mirror, "crawl.conf"), work)
    for key, value in conf.items():
        live.set_conf(work, key, value)
    with open(os.path.join(work, "live.conf"), "w") as f:
        if proxy:
            f.write(f"proxy {proxy}\n")
        f.write("connections 64\n")
    out = subprocess.run([program], cwd=work, capture_output=True,
                         text=True)
    with open(os.path.join(work, "report.txt"), "w") as f:
        f.write(out.stdout)
    if out.returncode != 0:
        print(out.stderr[-2000:])
        raise SystemExit(f"{program} failed in {work}")
    return out.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("mirror")
    parser.add_argument("--public", action="store_true")
    parser.add_argument("--local", action="store_true")
    args = parser.parse_args()
    mirror = os.path.abspath(args.mirror)
    work = os.path.join(mirror, "ent")
    os.makedirs(work, exist_ok=True)
    scan = os.path.join(work, "scan_links")
    program = os.path.join(work, "crawl_live")
    build(os.path.join(EXAMPLE, "scan_links.ent"), scan)
    build(os.path.join(EXAMPLE, "crawl_live.ent"), program)

    out = subprocess.run([scan], cwd=mirror, capture_output=True, text=True,
                         check=True).stdout
    with open(os.path.join(mirror, "links.txt")) as f:
        theirs = set(f.read().split("\n")) - {""}
    good = compare("K12 links on the mirror's pages",
                   set(out.split("\n")) - {""}, theirs)

    with open(os.path.join(mirror, "report.txt")) as f:
        expected = report_lines(f.read())
    if args.public:
        report = crawl(program, mirror, os.path.join(work, "public"), {})
        good &= compare("the public crawl", report_lines(report), expected)
    if args.local:
        server = os.path.join(work, "serve")
        subprocess.run(["cc", "-O2", "-o", server,
                        os.path.join(EXAMPLE, "serve.c"), "-lpthread"],
                       check=True)
        port_file = os.path.join(work, "port")
        if os.path.exists(port_file):
            os.remove(port_file)
        serving = subprocess.Popen([server, "--port-file", port_file,
                                    os.path.join(mirror, "site.manifest")],
                                   stdout=subprocess.DEVNULL)
        try:
            while not os.path.exists(port_file):
                pass
            with open(port_file) as f:
                port = int(f.read())
            report = crawl(program, mirror, os.path.join(work, "local"),
                           {"min_delay_ms": 0, "per_host": 64},
                           proxy=f"http://127.0.0.1:{port}")
        finally:
            serving.terminate()
            serving.wait()
        good &= compare("the crawl of the mirror", report_lines(report),
                        expected)
    print("all alike" if good else "differences above")


if __name__ == "__main__":
    main()
