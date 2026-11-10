#!/usr/bin/env python3
"""Fetches a public site once, politely, into a directory that serve.c
can answer from (http-SPEC.md §9.2, the mirror, and §9.3).

  python3 bench/http/mirror.py --seed URL --allow HOST [HOST ...]
                               --out DIR [--max-depth N] [--max-pages N]
                               [--delay S]

A crawler of its own, in Python's standard library alone and none of
the crawler's code, so that what it finds is something to hold the
crawler against: its links by Python's HTML parser (`href` and `src` of
any element, `<base href>`, entities, comments), URLs by urllib (site.py's
`resolve`, the crawler's rules), robots.txt by RFC 9309 (written here:
urllib's robotparser has no wildcards). It goes as the crawler does:
breadth first by depth, a redirect as deep as the ask redirected and
followed at most `redirects` times, a page as deep as may be fetched but
not read for links, the hosts of `--allow` only, 5xx and 429 tried
twice more. Politely: one request at a time, `--delay` seconds apart at
least (or robots.txt's `Crawl-delay` where longer), the crawler's user
agent, `--max-pages` asks at most.

Writes into DIR:

  site.manifest  every answer, as site.py writes it (serve.c's input):
                 `url status 0 0 type location length`, then the body
  crawl.conf     for crawl_live.ent and crawl.c on the mirror
  report.txt     the crawl as the crawler reports it (`fetched URL
                 status 1`, `redirect URL target`, `disallowed URL`,
                 `capped URL`), sorted
  links.txt      `link PAGE TARGET` for every link of every HTML page
                 (to any host, as scan_links.ent says them), sorted
  robots.txt.*   each host's robots.txt as it came
"""

import argparse
import collections
import gzip
import importlib.util
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                       "examples", "http")
spec = importlib.util.spec_from_file_location(
    "site_rules", os.path.join(EXAMPLE, "site.py"))
site_rules = importlib.util.module_from_spec(spec)
spec.loader.exec_module(site_rules)
resolve, normalise = site_rules.resolve, site_rules.normalise

AGENT = "ent-crawl/0.1 (+https://github.com/mzaks/ent-lang)"
TOKEN = "ent-crawl"
RETRIES, REDIRECTS, TIMEOUT = 2, 5, 30.0


def links_of(url, body):
    """Where the links of the page at `url` go, in order, by site.py's
    reading (Python's HTML parser)."""
    return site_rules.links_of(url, body.decode("utf-8", "replace"))


def robots_rules(body):
    """RFC 9309: the rules of the groups naming the crawler's token, or
    else those of `*` (groups of one name merged), and the Crawl-delay:
    [(allow, pattern)], seconds."""
    groups = []  # (agents, rules, delay)
    agents, rules, delay, in_agents = [], [], 0.0, False
    for line in body.splitlines():
        line = line.split("#", 1)[0]
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key, value = key.strip().lower(), value.strip()
        if key == "user-agent":
            if not in_agents and agents:
                groups.append((agents, rules, delay))
                agents, rules, delay = [], [], 0.0
            agents.append(value.lower())
            in_agents = True
            continue
        in_agents = False
        if key == "allow" and value:
            rules.append((True, value))
        elif key == "disallow" and value:
            rules.append((False, value))
        elif key == "crawl-delay":
            try:
                delay = float(value)
            except ValueError:
                pass
    if agents:
        groups.append((agents, rules, delay))
    ours = [g for g in groups if TOKEN in g[0]]
    kept = ours or [g for g in groups if "*" in g[0]]
    return ([r for g in kept for r in g[1]],
            max([g[2] for g in kept], default=0.0))


def allowed(rules, path):
    """Of the patterns matching `path` the longest says; allow on a tie."""
    best, allow = -1, True
    for a, pattern in rules:
        anchored = pattern.endswith("$")
        core = pattern[:-1] if anchored else pattern
        regex = ".*".join(re.escape(p) for p in core.split("*"))
        if re.match(regex + ("$" if anchored else ""), path):
            if len(pattern) > best or (len(pattern) == best and a):
                best, allow = len(pattern), a
    return allow


def path_of(url):
    parts = urllib.parse.urlsplit(url)
    return (parts.path or "/") + (f"?{parts.query}" if parts.query else "")


class NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirects)


def wire(url):
    """The URL as it goes on the wire (normalise encoded what needs it;
    this is for a seed or a Location as given)."""
    return urllib.parse.quote(url, safe=":/?#[]@!$&'()*+,;=%~")


def fetch(url):
    """status, content type, location, body; or None, failure, '', b''."""
    request = urllib.request.Request(wire(url), headers={
        "User-Agent": AGENT, "Accept-Encoding": "gzip"})
    try:
        with OPENER.open(request, timeout=TIMEOUT) as r:
            status, headers, body = r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        status, headers, body = e.code, e.headers, e.read()
    except Exception as e:  # noqa: BLE001 (any failure is an answer)
        return None, type(e).__name__, "", b""
    if headers.get("Content-Encoding", "") == "gzip":
        body = gzip.decompress(body)
    kind = re.sub(r"\s+", "", headers.get("Content-Type", "") or "") or \
        "application/octet-stream"
    return status, kind, headers.get("Location", "") or "", body


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--seed", required=True)
    parser.add_argument("--allow", nargs="+", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--max-pages", type=int, default=2000)
    parser.add_argument("--delay", type=float, default=1.0)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    seed = normalise(args.seed)
    scheme = urllib.parse.urlsplit(seed).scheme
    hosts = {h.lower() for h in args.allow}

    answers = {}          # url -> (status, kind, location, body)
    report, links = [], []
    last = 0.0
    asks = 0

    def polite_fetch(url, wait):
        nonlocal last, asks
        tries = 0
        while True:
            pause = last + wait - time.monotonic()
            if pause > 0:
                time.sleep(pause)
            last = time.monotonic()
            asks += 1
            answer = fetch(url)
            print(f"{asks} {answer[0]} {url}", flush=True)
            if answer[0] is not None and (answer[0] < 500 and answer[0] != 429):
                return answer
            if tries >= RETRIES:
                return answer
            tries += 1
            time.sleep(2.0 * 2 ** (tries - 1))

    # robots.txt first, for every host.
    robots = {}
    for host in sorted(hosts):
        url = f"{scheme}://{host}/robots.txt"
        status, kind, location, body = polite_fetch(url, args.delay)
        if status is not None:
            answers[url] = (status, kind, location, body)
        with open(os.path.join(args.out, f"robots.txt.{host}"), "wb") as f:
            f.write(body)
        rules, delay = robots_rules(body.decode("utf-8", "replace")) \
            if status == 200 else ([], 0.0)
        robots[host] = (rules, max(delay, args.delay))
        report.append(f"fetched {url} {status if status else 'failed'} 1")

    depth = {seed: 0}
    level = [seed]
    taken = 0
    d = 0
    while level:
        following = []
        queue = collections.deque(level)
        while queue:
            url = queue.popleft()
            # (With its port, as the crawler names hosts: another port is
            # another host.)
            host = urllib.parse.urlsplit(url).netloc
            if url in answers or host not in hosts:
                continue
            rules, wait = robots[host]
            if not allowed(rules, path_of(url)):
                report.append(f"disallowed {url}")
                answers[url] = None
                continue
            if taken >= args.max_pages:
                report.append(f"capped {url}")
                answers[url] = None
                continue
            taken += 1
            status, kind, location, body = polite_fetch(url, wait)
            if status is None:
                report.append(f"fetched {url} "
                              f"{'timeout' if kind == 'TimeoutError' else 'failed'} 1")
                answers[url] = None
                continue
            answers[url] = (status, kind, location, body)
            report.append(f"fetched {url} {status} 1")
            found = []
            if "html" in kind.lower():
                # (Every HTML answer's links are said, as scan_links says
                # them, a redirect's too; a 200's alone are followed.)
                found = links_of(url, body)
                links.extend(f"link {url} {t}" for t in found)
            if 300 <= status < 400 and location:
                target = resolve(url, location)
                report.append(f"redirect {url} {target}")
                hops = depth.get(("hops", url), 0)
                if target and hops < REDIRECTS and target not in answers:
                    depth[("hops", target)] = hops + 1
                    if target not in depth:
                        depth[target] = depth[url]
                    queue.append(target)
                continue
            if status == 200 and found and depth[url] < args.max_depth:
                for t in found:
                    if t not in depth:
                        depth[t] = depth[url] + 1
                        following.append(t)
        level = following
        d += 1

    with open(os.path.join(args.out, "site.manifest"), "wb") as out:
        for url in sorted(u for u, a in answers.items() if a):
            status, kind, location, body = answers[url]
            location = location.replace(" ", "%20") or "-"
            out.write(f"{url} {status} 0 0 {kind} {location} {len(body)}\n"
                      .encode())
            out.write(body + b"\n")
    with open(os.path.join(args.out, "report.txt"), "w") as f:
        f.write("\n".join(sorted(set(report))) + "\n")
    with open(os.path.join(args.out, "links.txt"), "w") as f:
        f.write("\n".join(sorted(set(links))) + "\n")
    with open(os.path.join(args.out, "crawl.conf"), "w") as f:
        conf = dict(seed=seed, allow=" ".join(sorted(hosts)),
                    max_depth=args.max_depth, retries=RETRIES,
                    redirects=REDIRECTS, timeout_ms=int(TIMEOUT * 1000),
                    slow_ms=10000, per_host=1, backoff_ms=2000,
                    min_delay_ms=int(args.delay * 1000),
                    max_pages=args.max_pages)
        for key, value in conf.items():
            f.write(f"{key} {value}\n")
    print(f"{asks} asks, {sum(1 for a in answers.values() if a)} answers, "
          f"{len(set(links))} links", file=sys.stderr)


if __name__ == "__main__":
    main()
