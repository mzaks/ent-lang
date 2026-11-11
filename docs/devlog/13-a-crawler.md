# 13 · A crawler on a pipeline of requests

*6 – 11 November 2026.*

## A talk from 2018, finally compiled

At MCE 2018 I gave a talk called
[ECS pattern explained](https://www.youtube.com/watch?v=DJx0JKuemvA). Its
networking part claims that a request should be an entity, each stage of it a
component, and each concern a small system: time out, warn when slow, retry,
follow a redirect, drop duplicates, log, clean up. No system calls another, so
you can remove the logging or add a mock without touching the rest. The
argument was about decoupling. It was made on slides.

Eight years later there's a language to check it in. To give the pipeline
real work we put a crawler on top: an answered page becomes the asks for the
pages it links to, so the pipeline feeds itself. That's also a good test of
ent-lang, because a crawler is mostly things ent hadn't done yet: text, the
network, and millions of entities of which a few dozen are busy.

The plan had seven steps, C0 to C6, written down in
`examples/http/http-SPEC.md` before any code. All seven are done.

## The pipeline

Here are two of the systems, whole:

```
system time_out() {
  for me, a: Ask, s: Sent with Going where Now >= s.at + Net.timeout {
    me.add(Failed { at: Now, kind: Failure.Timeout })
    me.remove(Going)
  }
}

system warn_slow() {
  for me, a: Ask, s: Sent with Going without Slow
      where Now >= s.at + Net.slow_after {
    me.add(Slow)
  }
}
```

`pipeline.ent` knows nothing of crawling or sockets. `crawl.ent` adds hosts,
depth, links and the report. `mock.ent` and `live.ent` are two devices for
the same pipeline: one answers from a generated site on a clock of its own,
the other is libcurl and the machine's clock. A program is a schedule, a
list of systems in order.

So the talk's claim became a check. `crawl_lean.ent` is the schedule
without the optional systems (no log, no history, no slow warning), and
`crawl_combined.ent` merges four pipeline systems into one. Both must give
the same report as the full crawler, on every test site, on one core and on
all. They do. The slides were right, which is a relief after eight years.

## Text had to come first

A crawler reads HTML and builds URLs, and ent's `text` could not be made
from bytes that C handed over. I had the choice of a stopgap (hash the URLs
to `u64` and never look at them again) or the feature. We took the feature:
an `extern fn` may now return `text`. Then `find`, `slice` and `lower` went
into a text *module*, written as extern fns, not into the compiler. That's
the rule I keep coming back to: the proper language feature over a stopgap,
and a library over a built-in.

With that, the URL rules (normalise, resolve, RFC 9309 robots.txt) are plain
ent fns over `text`, about 580 lines in `urls.ent` and `robots.ent`.

## The mock, and the first three walls

`site.py` generates a site with everything that hurts: redirects, loops,
404s, 500s, slow pages, timeouts, flaky pages, odd links, a robots.txt. It
also writes the report a correct crawl must produce. The crawler was right
quickly. It was not fast:

- **A `for` inside a `for` uses no index.** Finding duplicates that way,
  10⁵ pages took 138 s. Through `get` on a hash index: 4.5 s.
- **Hash indexes were made anew every frame.** Two of them took 2.5 s of
  those 4.5. Mended in place when entities spawn, die or move: 0.11 s.
- **The `Links` relation's mending walked over empty runs.** Most of the 2M
  entity keys at 10⁶ pages have no links, and each new edge walked far across
  them looking for room. One counter fixed it.

That took the mock at 10⁶ pages from 107 s to 25.5 s. All three were costs
in the language, not in the crawler, and every program with a hash index or
a sparse relation got the fix.

## Live: ten times slower than C, and why

`devices/http` is the network device, on libcurl's multi interface, with all
methods, headers, bodies, cookies and TLS, because other programs will want
it. For a reference we wrote the same crawler in C (`bench/http/crawl.c`):
same rules, same report, URL functions one for one. Both crawl a local epoll
server, so the network is honest but not the bottleneck.

First result, 10⁵ pages at 64 connections: C 2.3 s, ent 22.7 s.

The cause was one sentence long. A live crawl runs thousands of small
frames (4,223 here), and every system looked at every waiting ask in every
frame. On the mock the clock jumps to the next thing due, so nobody noticed.
In ECS terms: 100,000 entities, 64 of them interesting, and the loops are
paid by the 100,000.

We got it down step by step, and nearly every step was a language feature:

| step | 10⁵ pages, s |
|---|---:|
| as first built | 22.7 |
| per-frame fixes in the crawler | 10.6 |
| reactive `for`s walk the event log; hash indexes join only what changed | 7.4 |
| `tag Going apart`: the asks that are out live in a small archetype of their own | 5.0 |
| a relation that only reports read is sorted when they read it (once, not 23,202 times) | 3.7 |
| reactive `for ... by`: an ordered loop over just the new entities | 3.3 |
| texts cut from per-thread arenas | 2.9 |
| pages due found by events, not by a sweep | 2.78 |
| C | 2.24 |

The arena step deserves a note. Profiling showed that curl's share was the
same in both crawlers, so receiving was already at parity. The gap was our
own `malloc` per text. With arenas, the mock at 10⁵ went from 1.75 s to
0.68 s as a side effect.

## Against the crawlers people actually use

Then the wider field, on the same generated site, each tool checked against
the server's log of what was really fetched:

| 10⁵ pages, 64 connections | s |
|---|---:|
| C (ours, curl multi) | 2.24 |
| **ent** | 2.79 |
| spider-rs, visited set in memory | 4.3 |
| Colly (Go, 4 cores, 2.2 GB) | 5.9 |
| wget | 85 |
| Scrapy | 155 |
| spider-rs, defaults | 282 |

The last row is not a typo. By default spider spills its visited set to
SQLite past 15,000 URLs. Each tool needed some setup to crawl the site
correctly at all (Colly a mutex and URL normalising, Scrapy a fix for hosts
with a port, spider a saner retry backoff); `bench/http/README.md` lists what
and why.

At 10⁶ pages the order changes: spider in memory 23.0 s (on four cores,
missing 1 % of the images), C 27.9 s, ent 32.3 s. Colly was stopped at
14 GB after 620,000 URLs.

A million pages also found a bug that 10⁵ hides. At the end of a level the
crawler extracted the links of *all* next-level pages in one frame, 2.15 s
long. curl was not driven meanwhile, so 64 asks "timed out" although their
answers were sitting in the socket, and were asked again. Reading a page's
links when it arrives, and only spawning asks when its level is due, fixed
that. It did not make the crawl faster. It made it correct, which I'm told
also counts.

One question I asked along the way: would XDP or io_uring close the gap to
C? We measured instead of guessing. The kernel is a quarter to a third of
the CPU for C and ent alike. What ent spends more is its own code, so no.

## Real sites

`mirror.py` fetches a public site once, politely, and `public.py` holds the
crawler against that mirror. Polite means one ask a second per host,
robots.txt obeyed, a user agent that names the project, and a cap on pages.
On quotes.toscrape.com and books.toscrape.com the report matches line for
line. crawler-test.com, which exists to break crawlers, broke ours: a
redirect to a disallowed host made a crawl that never ended. Fixed.

Then I wanted to see it, so the last step is a window: counts on top, what's
in flight, the slow ones, the errors, and a bar for each second's answers.
It is drawn by the layout library from [post 05](05-games.md), the one
people say ECS can't do. I pointed it at modular.com (192 pages, 12 links to
pages that aren't there) and at mojolang.org (3,001 requests at 9 a second
with four asks out, six broken links). Sorry, friends.

## Stop choosing capacities

Every ent archetype has a capacity, and I was tired of rebuilding the
crawler for each site size. The fix I wanted: say one large capacity, 2²⁶
asks, and pay only for what a crawl uses. That was not true yet. A crawl of
20 pages in a world built for 16M asks wrote 1,126 MB, most of it offset
tables filled for every key the world *could* hold.

Now it writes 12 MB. Relations give runs only to the keys handed out so far,
event logs start in small pages, and with `ENT_WORLD_DIR` the world and its
texts live in files, so a crawl can be larger than memory. The mock crawls
10⁶ pages in a 1 GB limit in 80 s. In memory it takes 10 s, and is killed
at that limit. So that's the price, and it's yours to choose.

The window was the last thing to test this way, and here Claude did
something I'd like on the record. To profile the window crawler it ran the
2²⁶ build in memory with no limit. The layout's tree relations still went
over every possible key each frame: 14 GB in three seconds. The kernel
killed the program, and systemd then stopped the terminal it ran in, Claude
included. The next session read the kernel log, found its own fingerprints,
and fixed the trees to go over the keys in use. The window now peaks at
147 MB. Benchmarks run inside a memory-limited scope since.

## What the language got out of it

- **Text from devices,** and a text module that is a library.
- **Hash indexes mended in place,** then joined by what changed.
- **Reactive `for`s that walk the event log** in row order, may be ordered
  (`by`), and may send and connect from loops in their body.
- **`apart` tags,** for the few entities that are busy among the many that
  wait.
- **Relations sorted only when someone reads them.**
- **Text arenas,** paired text hashes, and inline `len` and indexing, so a
  loop over a text's bytes is vector code.
- **Capacity that costs nothing until used,** and worlds on disk.
- **A check that stops and says why** when a capacity overflows. Before, one
  hash index looped forever.

## What's still open

- ent is 1.16× C at 10⁶ pages (32.3 s against 27.9) and 1.25× at 10⁵. The
  rest is ent's own code per page: a spawn per link, the `apart` moves,
  normalising URLs.
- Memory at 10⁶ is 1.45 GB against C's 0.6 GB. It was 2.8 GB.
- Two apart moves in one `for` are not supported (the last one wins), and
  crawler-test.com still shows two differences we haven't chased.

## What this settled

- **The talk holds.** Systems really can be removed and merged without the
  program changing, and now a check says so, not a slide.
- **Reactive beats sweeping** as soon as most entities are waiting. The
  crawler's speed came from loops that run over what changed.
- **Benchmark against a twin in C.** Every step above started from a
  difference between two profiles of the same crawler.
- **Scale finds bugs.** The false timeouts and the endless crawl appeared
  only at 10⁶ pages and on a hostile site.

Back to the [index](README.md).
