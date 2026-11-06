# Spec: an HTTP client and a crawler in ent-lang

*For `examples/http/`. Same habits as the other examples: a generator
for the input, numbered checks that the check scripts name, benchmarks
that verify before they time.*

## 1. Goal

Build the networking example from the talk "ECS pattern explained"
(Maxim Zaks, MCE 2018, <https://www.youtube.com/watch?v=DJx0JKuemvA>,
22:42 to 38:45) in ent-lang, and put a web crawler on top of it.

The talk's claim: in place of a networking class that hides request
building, sending, errors, payload transformation and request
management behind one interface, a request is an entity, each stage is
a component, and each step is a system with one responsibility. Systems
never call each other, so logging, slow-request warnings, dedupe,
retry, cleanup and an offline mock are systems that can be added or
removed without touching the others.

The crawler is the business logic: the payload of a page becomes new
request entities, so the pipeline feeds itself. It gives the talk's
small systems real work (dedupe stops most of what is found) and gives
the example a benchmark (thousands of requests in flight).

Underneath both sits a complete HTTP client device, `devices/http`,
meant for other programs too, not only for the crawler.

The questions this example is meant to answer:

1. **Does the talk's decomposition hold up in ent?** Systems added and
   removed without changes elsewhere, with the compiler's access checks
   as the proof that nothing depended on them (§8, K7 and K10).
2. **Is the crawler fast?** Against a C crawler on libcurl's multi
   interface and against established crawlers (§9).
3. **Does it hold up on real sites?** Crawls of public sites that allow
   it, politely (§9.3).
4. **What does ent lack for programs that live on text and I/O?** (§10)

## 2. Scope

### In

- **A complete HTTP client device** (`devices/http`, §6.2) on libcurl:
  every method (`GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE`,
  `OPTIONS`, and any other by name), request headers and bodies,
  response status, headers and bodies, timeouts, TLS (with verification
  on by default), HTTP/1.1 and HTTP/2, compressed responses, connection
  reuse, proxies, a cookie jar per session, and redirects followed by
  the device or left to the program. Auth is headers (Basic, Bearer).
- Status codes, redirects (301, 302, 303, 307, 308) followed by the
  crawler's pipeline, not by the device, so that they are entities too.
- Timeouts, transient failures and retries with backoff (honouring
  `Retry-After`).
- A crawl of one or more sites: links found in HTML (§7.3), kept to
  allowed hosts, up to a depth.
- Politeness for real sites: `robots.txt` (allow/disallow and
  crawl-delay for our user agent), a user agent that names the crawler
  and a project URL, a per-host request rate.
- A mock mode that answers from a generated site in memory, with no
  network (the talk's offline bypass).
- A live mode against a local server serving the same site.
- Crawls of public sites that permit it (§9.3).

### Not here (for now)

- JavaScript rendering, a full HTML parser (§7.3 lists what the link
  scanner handles), sitemaps.
- Streaming bodies larger than `max_body` (a body past the limit is cut
  and marked so), WebSockets, HTTP/3.
- Public sites in the checks or in the benchmarks: those go to
  `127.0.0.1` only, so they are repeatable and load nobody else's
  servers. Public crawls are run by hand (§9.3).

## 3. Layout

| File | Contents |
|---|---|
| `README.md` | What is here, how to run it, what is checked, what is not here. |
| `http-SPEC.md` | This file. |
| `site.py` | Generates a site (§7.1): a folder of HTML files, a manifest the mock reads, and the expected report. |
| `pipeline.ent` | The talk's layer: request data, sending, responses, errors, timestamps, retry, dedupe, slow warnings, logging, cleanup (§4.1, §5.1). Knows nothing of crawling. |
| `crawl.ent` | The crawler's layer: link extraction, URL normalising, hosts, depth, politeness, the report (§4.2, §5.2). Knows nothing of sockets. |
| `mock.ent` | The mock: loads the manifest, answers request data from it with a simulated clock (§6.1). |
| `live.ent` | The live device's ent side (§6.2). |
| `devices/http.ent`, `devices/http.c` | The HTTP client device, a module from the start (§6.2), with its own tests in `test/Integration/` (D1 to D10, §8.1). |
| `crawl_mock.ent`, `crawl_live.ent` | The two programs: the same layers, another schedule (§6). |
| `crawl_combined.ent` | The five-small-systems vs one-combined variant (§8, K10). |
| `server/` | The local server (§7.2): the generated site plus the endpoints the device tests need. |
| `check/` | The check scripts (§8). |
| `bench/http/` | The C reference crawler, the setups of the other crawlers, the bench script (§9). |
| `sites.md` | The public sites crawled by hand, why they may be, and the results (§9.3). |

## 4. Data

Sketches: names and fields are settled here, the syntax when C1 is
built.

### 4.1 The pipeline (the talk's components)

| Component | Fields | Meaning |
|---|---|---|
| `Ask` | `method: Method, url, headers, body, origin: entity` | Request data: what is wanted. The talk's base URL, path components, method, parameters and payload, here as one URL plus headers and body. `origin` is what caused it (an event, a page, a redirect). |
| `Sent` | `at: f64, handle: i64` | Handed to the device. |
| `Got` | `at: f64, status: i32, headers, body, location, cut: bool` | A response. `location` for redirects; `cut` where the body was past the limit. |
| `Failed` | `at: f64, kind: Failure, code: i32` | No HTTP response: refused, timed out, DNS, TLS (the talk's OS-level errors). |
| `Attempt` | `n: i32, next_at: f64` | Retries so far and when the next may go. |
| tag `Slow` | | Sent long ago and no answer yet (a warning, not a timeout). |
| tag `GaveUp` | | Retries used up. |
| tag `Handled` | | The business layer is done with the payload; cleanup may drop the body. |

`enum Method { Get, Head, Post, Put, Patch, Delete, Options, Other }`
(`Other` takes its name from a header-like line, for methods such as
`PROPFIND`), `enum Failure { Refused, Timeout, Dns, Tls, TooBig, Other }`.
The crawler only asks with `Get` and `Head`; the pipeline handles them
all.

A redirect spawns a new `Ask` whose `origin` is the redirected request,
so a chain is a path of entities; a relation `(Ask)-[RedirectOf]->(Ask)
tree` lets the report walk it up.

### 4.2 The crawler

| Data | Fields | Meaning |
|---|---|---|
| component `Depth` | `d: i32` | Links from the seed to this page. |
| entity `Host` | `name, next_at: f64, in_flight: i32` | Per-host politeness: at most `in_flight` open and one send per interval. |
| relation `(Ask)-[On]->(Host)` | | Which host a request goes to. |
| relation `(Ask)-[Links]->(Ask)` | | Page links to page: the found graph, for the report. |
| component `Seed` | `url` | The talk's UI event: what the user asked for. |
| unique `Rules` | `max_depth, allowed hosts, retries, slow_after, timeout` | The crawl's settings. |

Dedupe: `index ByUrl of Ask by url hash`. A link whose normalised URL
`ByUrl.get` already has adds a `Links` edge to the existing entity and
spawns nothing.

### 4.3 Bodies and URLs

Bodies and headers are `text` fields (of any length): the device gives
them back as texts (§10.1), and a field gives its bytes back when it
gets another or its component goes. URLs are `text` too, so that `ByUrl`
can hash them; `extract_links` makes them with `slice`. (Before C0 this
was a byte arena with windows into it.)

## 5. Systems

Each system says what it reads and what it adds. "Opt." marks the
systems that can be left out with no other change (K7).

### 5.1 The pipeline

| System | Reads | Does | Opt. |
|---|---|---|---|
| `drop_duplicates` | `Ask`, `ByUrl` | Destroys an `Ask` whose URL another live `Ask` has (the talk's "drop the new one while the same is pending"). | |
| `send` | `Ask` without `Sent`, `Attempt.next_at`, the clock | Hands it to the device; adds `Sent`. | |
| `receive` | the device's completions | Adds `Got` or `Failed` with the time. | |
| `follow_redirect` | `Got` with 3xx and `location` | Spawns the next `Ask`, connected by `RedirectOf`; up to `Rules.redirects`, then `Failed`. | |
| `retry` | `Failed` with a transient kind, `Got` with 5xx or 429 | Removes `Sent`/`Failed`/`Got`, bumps `Attempt` with backoff; past the limit adds `GaveUp`. | |
| `time_out` | `Sent` without an answer, the clock | Cancels at the device, adds `Failed { kind: Timeout }`. | |
| `warn_slow` | `Sent` without an answer, the clock | Adds `Slow` past `slow_after`, prints a warning once. | yes |
| `log` | `Sent`, `Got`, `Failed` (`on [added]`) | One line per event. | yes |
| `history` | `Got`, `Failed` (`on [added]`) | Keeps a row per answer (URL, status, times) for the report and K9. | yes |
| `clean_up` | `Handled` | Drops the body (gives its bytes back). | yes |

### 5.2 The crawler

| System | Reads | Does | Opt. |
|---|---|---|---|
| `seed` | `Seed` | Spawns the first `Ask` with `Depth 0` (the talk's UI event to request data). | |
| `place` | `Ask` without `On` | Parses the host, finds or spawns its `Host`, connects `On`; a host not allowed: destroyed. | |
| `read_robots` | a new `Host` | Asks for its `/robots.txt` first; holds the host's other asks until it is in (or failed: then all allowed, as the convention is). | |
| `obey_robots` | `Ask` with `On`, the host's rules | Destroys asks the rules disallow for our user agent; takes `Crawl-delay` into the host's interval. | |
| `be_polite` | `Host`, `On` | Holds back `send` per host by `next_at` and `in_flight` (writes `Attempt.next_at`). | yes |
| `extract_links` | `Got` 200 with HTML, `Depth` | Scans the body for links (§7.3), normalises each, spawns an `Ask` with `Depth d + 1` below `max_depth`, or links to the one `ByUrl` has; adds `Handled`. | |
| `report` | `Ask`, `Got`, `Failed`, `GaveUp`, `Links`, `RedirectOf` | At the end: pages, broken links with the pages that link to them, redirect chains, given up, slow. | |

### 5.3 Order and the end

One frame: `seed place read_robots obey_robots drop_duplicates be_polite send receive time_out
follow_redirect retry extract_links warn_slow log history clean_up`.
The compiler orders by access anyway. The order written is the talk's
diagram, so a reader can follow it.

The crawl ends when no `Ask` lacks an answer (`Got`, `Failed` with
`GaveUp`, or destroyed). `main` loops the frame while there is one, then
runs `report`.

## 6. The two programs

`crawl_mock.ent` and `crawl_live.ent` import the same `pipeline.ent` and
`crawl.ent`. They differ only in which systems their schedule names.
That is the talk's offline bypass, as a schedule.

### 6.1 Mock

- `mock.ent` reads the manifest of §7.1 into `Page` entities (`url`,
  `status`, `location`, `body`, `delay`, `fails_first`) in `world`.
- `answer` replaces `send` and `receive`: for each `Ask` without `Sent`,
  it looks up the `Page` by URL and adds `Got` or `Failed` at simulated
  time `now + delay`, so `time_out`, `warn_slow` and `retry` see the
  same times on every run.
- The clock is a unique that `answer` moves to the next answer's time,
  so a crawl with long delays takes no wall time.

### 6.2 Live: the `http` device

`devices/http.ent` and `devices/http.c`, on libcurl's multi interface,
as `extern proc`s (no world for device C). A module any program can
import, not only the crawler. Sketch:

```
// A session: settings its requests share, and a cookie jar. Several
// sessions may be open at once (a crawler and an API client in one
// program).
extern proc http_session(user_agent: text, cookies: bool,
                         per_host: i32, total: i32,        // connections
                         http2: bool, verify_tls: bool,
                         proxy: text) -> i64

// Starts a request; gives its handle. `method` is a name ("GET",
// "PROPFIND", ..). `headers` is "Name: value" lines. `follow` > 0 lets
// the device follow that many redirects itself; 0 hands every 3xx to
// the program.
extern proc http_start(session: i64, method: text[16], url: text,
                       headers: text, body: text, timeout: f64,
                       max_body: i32, follow: i32) -> i64
extern proc http_cancel(handle: i64)

// Waits at most `wait` seconds for answers. Gives a row in `done` for
// each (handle, status, failure, time to first byte, total time, `cut`).
// How many answered.
extern proc http_poll(wait: f64, done: mut Done) -> i32
// What an answered request got, as texts (§10.1); then it is gone.
extern proc http_body(handle: i64) -> text
extern proc http_headers(handle: i64) -> text
extern proc http_forget(handle: i64)

extern proc http_close(session: i64)
```

- Procs run on the main thread, which curl multi needs.
- `http_poll` blocks in `curl_multi_poll` up to `wait`, the time to the
  next `Attempt.next_at` or timeout, so `main` does not spin.
- Response headers come back as received, one line each. A fn in the
  module (`header(headers, name)`), written in ent with `find`, `slice`
  and `lower`, finds one by name.
- The crawler opens one session with `follow` 0, so its pipeline sees
  every redirect.
- The exact parameters are settled at C2 and written in the module's
  comments, as the other devices do.

## 7. Inputs

### 7.1 The site generator

`site.py --pages N --seed S --out DIR` writes, deterministically:

- `DIR/www/…html`: N pages, each with a number of links (a fixed
  distribution) to other pages; some to pages that do not exist (404),
  some to other hosts (not to be fetched), some with fragments and
  `../` to normalise, some duplicated within a page.
- Redirects: chains of length 1 to 3, a loop, and redirects to missing
  pages. Served by a small rule file the live server reads.
- Faults: pages that answer 503 the first k times (retried, then 200),
  pages that always answer 500 (given up), pages slower than
  `slow_after`, pages slower than the timeout.
- `DIR/manifest`: every URL with status, location, delay, fails-first
  and body, for the mock.
- `DIR/expected.json`: the report the crawl must give.

### 7.2 The live server

Serves `DIR/www` with the redirect rules, faults and a `robots.txt` on
`127.0.0.1`. It must not be the bottleneck of §9: a small C server on
epoll or an existing one (nginx, if the faults can be expressed there).
Decided in C2.

For the device tests (§8.1) it also has the endpoints of an httpbin:
echo a request's method, headers and body; give a status; set and ask
for cookies; redirect n times; delay; send a gzip body; send a large
body. [go-httpbin](https://github.com/mccutchen/go-httpbin) has them
all and could stand in for this part, with a TLS listener in front
(self-signed, trusted by the test).

### 7.3 Links

What the link scanner of `extract_links` handles, so that it works on
real pages and not only on generated ones: `href` and `src` in double
quotes, single quotes and without quotes; any case of the attribute
name; `&amp;` and the other entities in URLs; `<base href>`; relative
URLs, `./`, `../`, `//host/path`, fragments dropped; `javascript:`,
`mailto:` and `data:` skipped; links in comments ignored. URL
normalising: lowercase scheme and host, default ports dropped, percent
encodings of unreserved bytes decoded, an empty path made `/`. Pages
that are not HTML (by `Content-Type`) are not scanned.

## 8. Checks

### 8.1 The device

Integration tests of `devices/http` against the local server's httpbin
endpoints (§7.2), in `test/Integration/`, so they run with the others
and need no network beyond `127.0.0.1`:

| # | Check |
|---|---|
| D1 | Every method of `Method`, and one by name (`PROPFIND`), reaches the server as sent; `HEAD` gives no body. |
| D2 | Request headers arrive as sent, a repeated header too. Response headers come back in order, repeated ones too. |
| D3 | Request bodies (empty, small, 10 MB, binary with zero bytes) arrive byte for byte; response bodies likewise. |
| D4 | Statuses 100 to 599 come back as given; none is taken for a failure. |
| D5 | `follow` 0 gives each 3xx to the program; `follow` n follows n and then gives the 3xx; 303 turns a `POST` into a `GET`, 307 and 308 keep it. |
| D6 | Timeouts: a delayed endpoint past the timeout gives `Timeout`; a refused port gives `Refused`; an unknown host gives `Dns`. |
| D7 | TLS: the self-signed listener fails with `Tls` unless trusted; trusted, it works. |
| D8 | Cookies: a session with a jar sends back what was set; one without does not. |
| D9 | gzip and br bodies come back decoded; a body over `max_body` is cut and marked. |
| D10 | Many at once: 10,000 requests over 256 connections all answer, each with its own body (handles not mixed up). |

### 8.2 The crawler

`check/crawl.py` runs a program against a generated site and compares
its report with `expected.json`. Every check runs on the mock. K8, K9
and K11 run on live too.

| # | Check |
|---|---|
| K1 | Every page reachable within `max_depth` is fetched exactly once. |
| K2 | The broken links and the pages that link to them are exactly the expected ones. |
| K3 | Redirect chains are followed to the end. The loop and over-long chains stop at the limit as `Failed`. |
| K4 | Pages failing the first k times are retried and end as 200. Those always failing end as `GaveUp` after `retries`. |
| K5 | No page beyond `max_depth` is fetched. No other host is fetched. |
| K6 | Slow pages are warned about once each. Pages past the timeout end as `Timeout`. |
| K7 | Built without the optional systems (`warn_slow`, `log`, `history`, `clean_up`, `be_polite`), the program compiles with no change to the other systems and gives the same report (less what those systems add). |
| K8 | Live and mock give the same report on the same site. |
| K9 | Politeness: no host sees more than its limit in flight or more sends per interval than allowed (from `history`). |
| K10 | `crawl_combined.ent`, with `send`, `receive`, `time_out`, `follow_redirect` and `retry` as one system, gives the same report. |
| K11 | `robots.txt`: disallowed paths are never asked for; `Crawl-delay` is kept; a missing `robots.txt` allows all; one that times out holds the host until it answers or fails. |
| K12 | The link scanner finds exactly the links of a page of every case in §7.3 (a generated page per case, plus saved real pages with their expected links). |

## 9. Benchmarks

`bench/http/` (by the other benches' rules: native builds, on mains
power, verify before timing).

### 9.1 Ours

- **Mock throughput:** pages per second of `crawl_mock` on a site of
  10⁴, 10⁵ and 10⁶ pages. This measures the pipeline and the crawler
  without I/O, one thread and all threads.
- **Live against C:** `crawl_live` against a C crawler on curl multi
  (`bench/http/crawl.c`, same rules, same dedupe by URL hash). Wall
  time and CPU time for 10⁴ to 10⁵ pages with 64 to 1024 requests in
  flight against the local server.
- **Small against combined:** K10's two programs timed against each
  other. The talk said you could try both and keep the faster; here it
  gets a number.

### 9.2 Against other crawlers

Against the local server, so that the network and someone else's
server are not what is measured:

| Crawler | Why |
|---|---|
| `wget --recursive` | The one everyone has; C, one connection: the floor. |
| Scrapy | The most used crawler framework (Python, Twisted, asynchronous). |
| Colly | Go, goroutines; a common choice when Scrapy is too slow. |
| spider (spider-rs) | Rust, tokio; says it is the fastest. |
| `bench/http/crawl.c` | Ours in C on curl multi: what ent should match. |

The rules, so that they do the same work:

- The same site, seed, depth, allowed hosts, `robots.txt` obeyed, no
  politeness delay (local), the same connections per host and in
  total, timeouts and retries as close as each allows; each with its
  own HTML parser (that is part of what it costs).
- **Verify before timing:** from the server's log, each crawler asked
  for the expected set of URLs, each once. Where one cannot be
  configured to do the same work (its URL normalising differs, it
  cannot be kept from following redirects), the difference is written
  down with the numbers, not hidden.
- Measured: wall time, CPU time (user + system), peak memory, pages per
  second; at 10⁴, 10⁵ and 10⁶ pages.
- Two sites: the generated one, and a mirror of a real one (§9.3: a
  public practice site fetched once, politely, and served locally),
  so that real HTML is in the measurement too.
- Versions pinned in `bench/http/README.md`; the crawlers' setups
  (their projects, configs and scripts) in `bench/http/`.

### 9.3 Public sites

Run by hand, never in checks or benchmarks, with these rules:

- Only sites that invite crawlers or that the project owns: sites made
  for scraping practice (`books.toscrape.com`, `quotes.toscrape.com`),
  sites made for testing crawlers (`crawler-test.com`), the project's
  own pages. The list and why each may be crawled are in `sites.md`.
- `robots.txt` obeyed; at most one request per second per host, or the
  `Crawl-delay` if longer; a user agent that names the crawler and the
  project's URL; a cap on pages and depth for every run.
- What is measured: correctness, not speed (the rate limit sets the
  wall time). The set of URLs found, against one other crawler (§9.2)
  run with the same caps; what the scanner missed or got wrong (each
  becomes a case in K12); failures by kind; pages that broke the
  device (each becomes a case in D1 to D10 where it can).

## 10. What ent lacks for this

### 10.1 Text from C (done in C0)

`text` had no slices or search, and C could not give a text back. C0
made the language feature, not the stopgap: an extern fn or proc may
give a `text` back (C makes it with `ent_text_new`), and the module
`text` has `find`, `slice` and `lower` as extern fns over it, a library
and not compiler built-ins (test/Integration/text_extern.test takes the
links out of a page with them). So the device gives a body back as a
`text`, and `extract_links` makes URL texts with `slice`.

### 10.2 Bodies in an arena (not needed after C0)

With bodies as `text` fields, a body's bytes are given back when its
field gets another or its component goes (`clean_up` removes it), so
§4.3's byte arena is no longer needed for bodies. Request bodies to
send stay a `text` too.

### 10.3 The device request/response shape

`docs/sync-points.md` lists devices as request and response entities,
not yet built. This example is the first full one. If it works, the
shape (a request component in, a completion buffer out, polled with a
wait) is a candidate for that proposal, with `console` (`Print`) as the
simple case.

## 11. Milestones

| | What | Done when |
|---|---|---|
| **C0** | §10.1 decided and, if a language feature, built with its own tests. | `extract_links` can make a URL `text`. |
| **C1** | `site.py`, `pipeline.ent`, `crawl.ent`, `mock.ent`, `crawl_mock.ent`, `crawl_combined.ent`, `check/`. | K1 to K7, K10 and K12 (generated cases) pass on the mock; mock throughput measured. |
| **C2** | `devices/http` and the local server with its httpbin endpoints and TLS listener. | D1 to D10 pass. |
| **C3** | `crawl_live.ent`, `robots.txt`, `bench/http/crawl.c`. | K8, K9 and K11 pass; the live benchmark against C is measured and written down. |
| **C4** | The other crawlers' setups, the mirrored real site. | §9.2 measured and written down, every crawler verified first. |
| **C5** | `sites.md`, crawls of the public sites. | §9.3 run; what it found is in K12 and D1 to D10. |
| **C6** | A live window (Clay, `layout`, `window`): requests in flight, slow ones, errors, history. | Shows a crawl as it runs. |

## 12. Open questions

1. §10.1: language feature or stopgap? (The preferred way is the
   feature. It decides C0.)
2. Does the live server go in C (in the repo) or use nginx, and
   go-httpbin for the device tests or our own endpoints?
3. Keep every `Ask` as history (the report needs the graph) or destroy
   finished ones and keep only `history` rows? (C1 keeps them.)
4. The user agent string and the project URL in it for §9.3.
5. Which of the other crawlers are worth their setup: all four, or
   start with Scrapy and spider and add the others if the numbers are
   interesting?
