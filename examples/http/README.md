# A crawler on a pipeline of requests

The request pipeline from the talk "ECS pattern explained" (MCE 2018): a
request is an entity, each stage of it a component (`Ask`, `Sent`, `Got`,
`Failed`), each step a system with one job (time out, warn when slow,
try again, follow a redirect, drop duplicates, log, keep history, clean
up). No system calls another, so any optional one can be left out or
several merged into one, and the program does not change.

On top of it, a crawler: an answered page becomes the asks for the pages
it links to, so the pipeline feeds itself. Breadth first by depth, one
ask per URL, a limit of asks out per host. The spec is
[http-SPEC.md](http-SPEC.md); this is milestone C1, the crawler on a
mock of the network.

| File | What |
|---|---|
| `pipeline.ent` | The request pipeline. Knows nothing of crawling or sockets. |
| `crawl.ent` | The crawler: hosts, depth, links, the report. |
| `urls.ent` | URLs (normalise, resolve) and links on a page, in fns over `text`. |
| `mock.ent` | The device, mocked: answers from a site's manifest on a clock of its own. |
| `crawl_mock.ent` | The program: the schedule. |
| `crawl_lean.ent` | The same without the optional systems (check K7). |
| `crawl_combined.ent` | Four pipeline systems as one (check K10). |
| `scan_links.ent` | Every link of every page, for check K12. |
| `site.py` | Makes a site (pages, redirects, failures, slow pages, odd links) and the report a crawl of it must give. |
| `check/crawl.py` | Builds the programs, runs them on several sites, compares. |

Run (needs the compiler in `build/bin` and `LLVM_PREFIX`):

```sh
python3 examples/http/site.py --pages 200 --out /tmp/site
tools/ent build -I examples/http examples/http/crawl_mock.ent -o /tmp/crawl
(cd /tmp/site && /tmp/crawl | grep -v "^log " | sort -u)   # = expected.txt
python3 examples/http/check/crawl.py [--parallel]
python3 bench/http/mock.py 10000 100000    # pages a second, bench/http/RESULTS.md
```

The report: `fetched URL status-or-why tries`, `redirect URL target`,
`redirect-limit URL`, `slow URL`, `broken URL why from PAGE`.

The device for the network is `devices/http` (libcurl, any method; its
checks are test/Integration/http_device.test). Next (C3): the same
crawler on it, against a local server, and its benchmark against C.
