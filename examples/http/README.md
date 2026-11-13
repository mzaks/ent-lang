# A crawler on a pipeline of requests

The request pipeline from the talk "ECS pattern explained" (MCE 2018): a
request is an entity, each stage of it a component (`Ask`, `Sent`, `Got`,
`Failed`), each step a system with one job (time out, warn when slow,
try again, follow a redirect, drop duplicates, log, keep history, clean
up). No system calls another, so any optional one can be left out or
several merged into one, and the program does not change.

On top of it, a crawler: an answered page becomes the asks for the pages
it links to, so the pipeline feeds itself. Breadth first by depth, one
ask per URL, a limit of asks out per host, robots.txt kept (its rules
and `Crawl-delay`). The spec is [http-SPEC.md](http-SPEC.md). It runs on
a mock of the network (C1) and over the network (C3), the same layers
with another schedule.

| File | What |
|---|---|
| `pipeline.ent` | The request pipeline. Knows nothing of crawling or sockets. |
| `crawl.ent` | The crawler: hosts, depth, links, the report. |
| `urls.ent` | URLs (normalise, resolve) and links on a page, in fns over `text`. |
| `robots.ent` | A robots.txt's rules for the crawler (RFC 9309), in fns over `text`. |
| `mock.ent` | The device, mocked: answers from a site's manifest on a clock of its own. |
| `live.ent` | The device over the network: `devices/http` (libcurl) and the machine's clock. |
| `crawl_mock.ent`, `crawl_live.ent` | The programs: the schedules. |
| `watch.ent`, `crawl_window.ent` | A window that shows a crawl as it runs: what is out, slow ones, errors, answers each second. |
| `crawl_lean.ent` | The same without the optional systems (check K7). |
| `crawl_combined.ent` | Four pipeline systems as one (check K10). |
| `scan_links.ent` | Every link of every page, for check K12. |
| `site.py` | Makes a site (pages, redirects, failures, slow pages, odd links, robots.txt) and the report a crawl of it must give. |
| `serve.c` | A server on epoll that answers a site as the mock does, for the live crawl. |
| `check/crawl.py` | Builds the programs, runs them on several sites, compares. |
| `sites.md` | The public sites the crawler may crawl, the rules, and what crawling them found (§9.3). |

Run (needs the compiler in `build/bin` and `LLVM_PREFIX`):

```sh
python3 examples/http/site.py --pages 200 --out /tmp/site
tools/ent build -I examples/http examples/http/crawl_mock.ent -o /tmp/crawl
(cd /tmp/site && /tmp/crawl | grep -v "^log \|^history " | sort -u)   # = expected.txt
python3 examples/http/check/crawl.py [--parallel] [--live]
python3 bench/http/mock.py 10000 100000    # pages a second, bench/http/RESULTS.md
python3 bench/http/live.py                 # over the network, against bench/http/crawl.c
python3 bench/http/mirror.py --seed URL --allow HOST --out DIR   # a public site, once, politely
python3 bench/http/public.py DIR --public --local                # the crawler against it
```

Over the network (`crawl_live.ent` says it step by step): `serve.c` on
the site's manifest, a `live.conf` with `proxy http://127.0.0.1:PORT`
and `connections N` where the site is, then the program;
`check/crawl.py --live` does that for every site of its checks.

With a window (`crawl_window.ent`, the same crawl and report): build it
with `-I examples/http`; `close_when_done 1` in live.conf
closes it as the crawl ends, `screenshot shot.png` takes a picture then.

For a public site (sites.md) `crawl.conf` also has `min_delay_ms` (the
least time between asks to a host) and `max_pages` (asks past it are
reported `capped URL` and not sent).

The report: `fetched URL status-or-why tries`, `disallowed URL`, `capped URL`,
`redirect URL target`, `redirect-limit URL`, `slow URL`, `broken URL
why from PAGE`.

The device for the network is `devices/http` (libcurl, any method; its
checks are test/Integration/http_device.test).
