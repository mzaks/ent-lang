#!/usr/bin/env python3
"""Generates a site for the crawler (examples/http/http-SPEC.md, §7.1).

    site.py --pages N --seed S --out DIR [--depth D] [--retries R] ...

writes into DIR, all of it from the seed alone:

- `site.manifest`: every URL the site answers, for the mock (mock.ent)
  and the live server (serve.c): a line `url status delay_ms fails type
  location length`, then `length` bytes of body and a newline. (`-` for
  no location.)
- `crawl.conf`: what the crawler is to do (crawl.ent reads it).
- `www/`: the pages as files and `rules.txt` (redirects and faults), for
  crawlers that want files.
- `expected.txt`: the report the crawl must give, worked out here from
  the site alone, by Python's own URL and robots.txt rules (urllib), not
  by the crawler's code: the lines of `report` in crawl.ent, sorted.

`--robots` says what the site's `/robots.txt` is (§8.2, K11): `none`
(there is none: a 404, every page may be asked for), `rules` (pages
under `/private/` and those starting `/p/12` may not be, but for
`/private/open.html`; another agent's group disallows all; a
`Crawl-delay`), `slow` (those rules, answered after longer than
`slow_ms`: the host waits for them) or `timeout` (never answered in
time: every page may be asked for once it has failed). `--time-scale`
makes every delay and wait so many times as long (0.1 for a live crawl
that does not take long).

The pages are on `http://site.test/`; a link to another host is not to
be followed. What a page links to, and how it writes the link, is drawn
from the seed so that every case of §7.3 appears: absolute, root-relative,
relative, `./` and `../`, fragments, upper-case schemes and hosts, the
default port, single quotes, no quotes, `HREF`, `&amp;`, `<base href>`,
`src`, links in comments, and schemes that are not followed.
"""
import argparse
import heapq
import os
import random
import urllib.parse
import urllib.robotparser

HOST = "site.test"
ROOT = f"http://{HOST}/"
# (The crawler's name, as robots.txt groups know it.)
AGENT = "ent-crawl"


def robots_text(crawl_delay):
    """The robots.txt of `--robots rules`. (Allow before Disallow, so that
    urllib's first match and the longest match of RFC 9309, which the
    crawler keeps, say the same.)"""
    return ("# For the crawler's checks.\n"
            "User-agent: other-bot\n"
            "Disallow: /\n"
            "\n"
            "User-agent: *\n"
            "Allow: /private/open.html\n"
            "Disallow: /private/\n"
            "disallow: /p/12\n"
            f"Crawl-delay: {crawl_delay:g}\n")


def normalise(url):
    """The crawler's URL rules, by urllib: lower-case scheme and host, the
    default port dropped, an empty path made `/`, no fragment."""
    parts = urllib.parse.urlsplit(url)
    scheme = parts.scheme.lower()
    host = (parts.hostname or "").lower()
    port = parts.port
    if port and not ((scheme == "http" and port == 80) or
                     (scheme == "https" and port == 443)):
        host += f":{port}"
    path = parts.path or "/"
    return urllib.parse.urlunsplit((scheme, host, path, parts.query, ""))


def resolve(base, ref):
    """Where a link goes from a page at `base`; None where it is not to be
    followed (another scheme than http and https, or only a fragment)."""
    ref = ref.replace("&amp;", "&").strip()
    if not ref or ref.startswith("#"):
        return None
    scheme = urllib.parse.urlsplit(ref).scheme.lower()
    if scheme and scheme not in ("http", "https"):
        return None
    return normalise(urllib.parse.urljoin(base, ref))


class Site:
    def __init__(self, pages, seed, robots="none", scale=1.0):
        self.rng = random.Random(seed)
        self.pages = pages
        self.robots = robots
        self.scale = scale
        # url path -> dict(status, delay, fails, type, location, body)
        self.answers = {}
        # path -> [(form, target ref as written)]; written into bodies at
        # the end, when every path is known.
        self.make()

    def page(self, path, status=200, delay=5, fails=0, kind="text/html",
             location=None, body=""):
        delay = int(round(delay * self.scale))
        self.answers[path] = dict(status=status, delay=delay, fails=fails,
                                  type=kind, location=location, body=body)

    def link(self, here, target, rng):
        """A link from the page at path `here` to path `target`, written in
        one of the ways a page writes links."""
        directory = here.rsplit("/", 1)[0] + "/"
        form = rng.randrange(12)
        if form == 0:
            ref = f"http://{HOST}{target}"
        elif form == 1:
            ref = target
        elif form == 2 and target.startswith(directory):
            ref = target[len(directory):]
        elif form == 3 and target.startswith(directory):
            ref = "./" + target[len(directory):]
        elif form == 4 and here.count("/") >= 2:
            ref = "../" * (here.count("/") - 1) + target.lstrip("/")
        elif form == 5:
            ref = target + "#part" + str(rng.randrange(3))
        elif form == 6:
            ref = f"HTTP://SITE.TEST{target}"
        elif form == 7:
            ref = f"http://{HOST}:80{target}"
        else:
            ref = target
        quote = rng.choice(['"', '"', '"', "'", ""])
        attribute = rng.choice(["href", "href", "href", "HREF", "Href"])
        if quote == "" and (" " in ref or ">" in ref):
            quote = '"'
        return f'<a {attribute}={quote}{ref}{quote}>{target}</a>'

    def make(self):
        rng = self.rng
        n = self.pages
        paths = [f"/p/{i}.html" for i in range(n)]
        bodies = {}
        # Pages that are other than plain ones.
        missing = [f"/missing/{j}.html" for j in range(max(2, n // 50))]
        flaky = [f"/flaky/{j}.html" for j in range(max(2, n // 100))]
        slow = [f"/slow/{j}.html" for j in range(max(1, n // 200))]
        self.page("/broken500.html", status=500, body="<p>always broken</p>")
        self.page("/timeout.html", delay=5000, body="<p>too slow</p>")
        self.page("/hidden.html", body="<p>only in a comment</p>")
        self.page("/q", body="<p>a query page</p>")
        self.page("/q?a=1&b=2", body="<p>a query page with a query</p>")
        for j, path in enumerate(flaky):
            self.page(path, fails=1 + j % 2, body=f"<p>flaky {j}</p>")
        for j, path in enumerate(slow):
            self.page(path, delay=1500, body=f"<p>slow {j}</p>")
        # Redirects: chains of one to three to a page, one of four (too
        # long), a loop, one to a missing page.
        redirects = []
        for j in range(max(3, n // 100)):
            hops = 1 + j % 3
            chain = [f"/r/{j}/{k}" for k in range(hops)]
            target = paths[rng.randrange(n)]
            for k, path in enumerate(chain):
                self.page(path, status=301 if k % 2 == 0 else 302, delay=2,
                          location=chain[k + 1] if k + 1 < hops else target,
                          kind="text/plain")
            redirects.append(chain[0])
        long_chain = [f"/long/{k}" for k in range(5)]
        for k, path in enumerate(long_chain):
            self.page(path, status=301, delay=2, kind="text/plain",
                      location=long_chain[k + 1] if k + 1 < 5 else paths[0])
        self.page("/loop/a", status=302, delay=2, location="/loop/b",
                  kind="text/plain")
        self.page("/loop/b", status=302, delay=2, location="/loop/a",
                  kind="text/plain")
        self.page("/r/missing", status=301, delay=2,
                  location="/missing/0.html", kind="text/plain")
        # Pictures: not HTML, so not scanned (their bytes have an href).
        pictures = [f"/img/{j}.png" for j in range(max(2, n // 100))]
        for j, path in enumerate(pictures):
            self.page(path, kind="image/png",
                      body=f'PNG href="/p/{n - 1 - j}.html"')
        # Pages under /d/ with a <base href> that the relative links of
        # them go by.
        based = [f"/d/{j}/index.html" for j in range(max(2, n // 100))]
        # Pages robots.txt keeps the crawler from, and one it lets it to.
        private = []
        if self.robots != "none":
            private = [f"/private/{j}.html" for j in range(max(2, n // 100))]
            private.append("/private/open.html")
            for path in private:
                self.page(path, body=f"<p>{path}</p>")
            text = robots_text(0.2 * self.scale)
            delay = {"rules": 5, "slow": 2000, "timeout": 5000}[self.robots]
            self.page("/robots.txt", delay=delay, kind="text/plain", body=text)

        extras = (missing + flaky + slow + redirects + pictures + based +
                  private +
                  ["/broken500.html", "/timeout.html", "/q", "/q?a=1&amp;b=2",
                   "/loop/a", "/r/missing", "/long/0"])
        # Every extra is linked from some plain page.
        extra_from = {path: rng.randrange(min(n, 40)) for path in extras}

        for i, path in enumerate(paths):
            parts = [f"<html><head><title>page {i}</title></head><body>"]
            for _ in range(rng.randrange(2, 7)):
                # Mostly near, sometimes far: a site deeper than a few
                # links.
                if rng.random() < 0.7:
                    target = paths[min(n - 1, i + 1 + rng.randrange(20))]
                else:
                    target = paths[rng.randrange(n)]
                parts.append(self.link(path, target, rng))
            for extra, source in extra_from.items():
                if source == i:
                    if extra.startswith("/img/"):
                        parts.append(f'<img src="{extra}">')
                    else:
                        parts.append(f'<a href="{extra}">x</a>')
            if rng.random() < 0.1:
                parts.append('<a href="http://other.test/away.html">away</a>')
            if rng.random() < 0.05:
                parts.append('<a href="mailto:someone@site.test">mail</a> '
                             '<a href="javascript:void(0)">js</a> '
                             '<a href="#top">top</a> '
                             '<a data-href="/hidden.html">not a link</a>')
            if i == 0:
                parts.append('<!-- <a href="/hidden.html">hidden</a> -->')
            parts.append("</body></html>")
            bodies[path] = "\n".join(parts)
        for j, path in enumerate(based):
            target = paths[rng.randrange(n)]
            bodies[path] = (f'<html><head><base href="/p/"></head><body>'
                            f'<a href="{target.rsplit("/", 1)[1]}">based</a>'
                            f'</body></html>')
        index = ["<html><body>"]
        for i in range(min(n, 5)):
            index.append(f'<a href="p/{i}.html">{i}</a>')
        for path in private:
            index.append(f'<a href="{path}">private</a>')
        index.append("</body></html>")
        bodies["/"] = "\n".join(index)
        for path, body in bodies.items():
            self.page(path, body=body)

    def links_of(self, url, answer):
        """The links of a page as the crawler is to find them (by regular
        expressions here, not by its scanner)."""
        import re
        body = re.sub(r"<!--.*?-->", "", answer["body"], flags=re.S)
        base = url
        found = re.search(r"<base\s[^>]*href\s*=\s*(\"[^\"]*\"|'[^']*'|[^\s>]+)",
                          body, re.I)
        if found:
            base = resolve(url, found.group(1).strip("\"'")) or url
        links = []
        # (The base's own href is no link.)
        body = re.sub(r"<base\s[^>]*>", "", body, flags=re.I)
        for match in re.finditer(
                r"(?<=[\s])(?:href|src)=(\"[^\"]*\"|'[^']*'|[^\s>]+)", body,
                re.I):
            value = match.group(1)
            if value[:1] in "\"'":
                value = value[1:-1]
            target = resolve(base, value)
            if target is not None:
                links.append(target)
        return links


def crawl(site, conf):
    """The report a crawl of `site` by `conf` gives: breadth first by
    depth, a link one deeper and a redirect as deep, every URL asked for
    once, retries and timeouts as the mock answers them.

    - A URL first reached by a link (or the seed) is 0 redirects from it;
      one a redirect leads to, one more than the URL redirected. A
      redirect from a URL that many as the limit or more is not followed
      (`redirect-limit`).
    - Links are taken from pages that answered 200 with HTML and are less
      deep than the deepest allowed, to the hosts allowed.
    """
    by_url = {normalise(ROOT.rstrip("/") + path): a
              for path, a in site.answers.items()}
    seed = normalise(conf["seed"])
    allowed = conf["allow"].split()
    # Each host's robots.txt is asked for first, as deep as the seed; its
    # rules, where it answered 200 in the end.
    robots = normalise(ROOT + "robots.txt")
    depth, hops = {seed: 0, robots: 0}, {seed: 0, robots: 0}
    queue, order = [(0, 0, robots), (0, 1, seed)], 1
    rules = None
    answer = by_url.get(robots)
    if answer and outcome_of(answer, conf) == "200":
        rules = urllib.robotparser.RobotFileParser()
        rules.parse(answer["body"].splitlines())
    edges, limited, redirect_of = set(), set(), {}
    done, disallowed = set(), set()

    def reach(target, d, h):
        nonlocal order
        if target not in depth or (d, h) < (depth[target], hops[target]):
            depth[target], hops[target] = d, h
            order += 1
            heapq.heappush(queue, (d, order, target))

    while queue:
        d, _, url = heapq.heappop(queue)
        if url in done or d > depth[url]:
            continue
        done.add(url)
        if url != robots and rules and not rules.can_fetch(AGENT, url):
            disallowed.add(url)
            continue
        answer = by_url.get(url)
        if answer is None:
            continue
        if answer["location"] and outcome_of(answer, conf).startswith("3"):
            target = resolve(url, answer["location"])
            if target is None or host_of(target) not in allowed:
                continue
            redirect_of[url] = target
            if hops[url] >= conf["redirects"]:
                limited.add(url)
            else:
                reach(target, d, hops[url] + 1)
            continue
        if (outcome_of(answer, conf) != "200" or answer["type"] != "text/html"
                or d >= conf["max_depth"]):
            continue
        for target in site.links_of(url, answer):
            if host_of(target) not in allowed:
                continue
            edges.add((url, target))
            reach(target, d + 1, 0)

    lines = []
    outcomes = {}
    for url in depth:
        if url in disallowed:
            lines.append(f"disallowed {url}")
            outcomes[url] = "disallowed"
            continue
        answer = by_url.get(url)
        outcome = outcome_of(answer, conf) if answer else "404"
        outcomes[url] = outcome
        tries = tries_of(answer, conf) if answer else 1
        lines.append(f"fetched {url} {outcome} {tries}")
        if url in redirect_of:
            lines.append(f"redirect {url} {redirect_of[url]}")
        if url in limited:
            lines.append(f"redirect-limit {url}")
        if answer and answer["delay"] > conf["slow_ms"]:
            lines.append(f"slow {url}")
    for source, target in edges:
        if not outcomes[target].startswith(("2", "3", "disallowed")):
            lines.append(f"broken {target} {outcomes[target]} from {source}")
    return sorted(set(lines))


def host_of(url):
    return urllib.parse.urlsplit(url).netloc


def outcome_of(answer, conf):
    """What the crawler ends with for an answer: the status, or why there
    was none. The mock answers 503 to the first `fails` tries; 5xx and 429
    are tried again, `retries` more times at most."""
    if answer["delay"] > conf["timeout_ms"]:
        return "timeout"
    if answer["fails"] > conf["retries"]:
        return "503"
    return str(answer["status"])


def tries_of(answer, conf):
    retried = answer["status"] >= 500 or answer["status"] == 429
    if answer["delay"] > conf["timeout_ms"] or retried or \
            answer["fails"] > conf["retries"]:
        return conf["retries"] + 1
    return answer["fails"] + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pages", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", required=True)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--retries", type=int, default=2)
    parser.add_argument("--redirects", type=int, default=3)
    parser.add_argument("--timeout-ms", type=int, default=3000)
    parser.add_argument("--slow-ms", type=int, default=1000)
    parser.add_argument("--per-host", type=int, default=64)
    parser.add_argument("--robots", default="none",
                        choices=["none", "rules", "slow", "timeout"])
    parser.add_argument("--time-scale", type=float, default=1.0)
    parser.add_argument("--no-www", action="store_true",
                        help="leave out www/ (the files for the live "
                             "server), for the mock alone")
    args = parser.parse_args()

    site = Site(args.pages, args.seed, args.robots, args.time_scale)
    scaled = lambda ms: int(round(ms * args.time_scale))
    conf = dict(seed=ROOT, allow=HOST, max_depth=args.depth,
                retries=args.retries, redirects=args.redirects,
                timeout_ms=scaled(args.timeout_ms),
                slow_ms=scaled(args.slow_ms),
                per_host=args.per_host, backoff_ms=scaled(100))
    os.makedirs(os.path.join(args.out, "www"), exist_ok=True)
    with open(os.path.join(args.out, "crawl.conf"), "w") as out:
        for key in ("seed", "allow", "max_depth", "retries", "redirects",
                    "timeout_ms", "slow_ms", "per_host", "backoff_ms"):
            out.write(f"{key} {conf[key]}\n")
    with open(os.path.join(args.out, "site.manifest"), "wb") as out:
        for path in sorted(site.answers):
            a = site.answers[path]
            body = a["body"].encode()
            location = a["location"] or "-"
            out.write(f"{ROOT.rstrip('/')}{path} {a['status']} {a['delay']} "
                      f"{a['fails']} {a['type']} {location} {len(body)}\n"
                      .encode())
            out.write(body + b"\n")
    rules = []
    for path in sorted(site.answers):
        a = site.answers[path]
        if path.endswith("/") or "?" in path:
            continue
        if a["location"] or a["status"] != 200 or a["fails"] or \
                a["delay"] > 100 or a["type"] != "text/html":
            rules.append(f"{path} {a['status']} {a['delay']} {a['fails']} "
                         f"{a['type']} {a['location'] or '-'}")
        if args.no_www:
            continue
        file = os.path.join(args.out, "www", path.lstrip("/"))
        os.makedirs(os.path.dirname(file), exist_ok=True)
        with open(file, "w") as out:
            out.write(a["body"])
    if not args.no_www:
        with open(os.path.join(args.out, "www", "index.html"), "w") as out:
            out.write(site.answers["/"]["body"])
    with open(os.path.join(args.out, "www", "rules.txt"), "w") as out:
        out.write("\n".join(rules) + "\n")
    with open(os.path.join(args.out, "expected.txt"), "w") as out:
        out.write("\n".join(crawl(site, conf)) + "\n")


if __name__ == "__main__":
    main()
