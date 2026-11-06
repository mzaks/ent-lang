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
| `urls.ent` | Fns: where a link goes (URL resolving and normalising) and where the links of a page are (§7.3). |
| `crawl.ent` | The crawler's layer: link extraction, hosts, depth, politeness, the report (§4.2, §5.2). Knows nothing of sockets. |
| `mock.ent` | The mock: loads the manifest, answers request data from it with a simulated clock (§6.1). |
| `live.ent` | The live device's ent side (§6.2). |
| `devices/http.ent`, `devices/http.c` | The HTTP client device, a module from the start (§6.2), with its own tests in `test/Integration/` (D1 to D10, §8.1). |
| `crawl_mock.ent`, `crawl_live.ent` | The two programs: the same layers, another schedule (§6). |
| `crawl_lean.ent` | crawl_mock.ent without the optional systems (§8, K7). |
| `crawl_combined.ent` | Four of the pipeline's systems as one (§8, K10). |
| `scan_links.ent` | The link scanner alone, every link of every page (§8, K12). |
| `server/` | The local server (§7.2): the generated site plus the endpoints the device tests need. |
| `check/` | The check scripts (§8). |
| `bench/http/` | `mock.py` (§9.1, built in C1); the C reference crawler, the setups of the other crawlers (C3, C4). |
| `sites.md` | The public sites crawled by hand, why they may be, and the results (§9.3). |

## 4. Data

As built in C1 (the files say each field's meaning).

### 4.1 The pipeline (the talk's components)

| Component | Fields | Meaning |
|---|---|---|
| `Ask` | `method: Method, url, headers, body, origin: entity, hops, tries, next_at: f64, serial` | Request data: what is wanted (the talk's base URL, path, method, parameters and payload, here as one URL plus headers and body), what caused it (`origin`: the ask a redirect came from), redirects followed to get here, tries again so far, not to be sent before `next_at`, the order asks were made in. |
| `Twin` | `of: entity` | Every ask has one: the older ask for the same URL of which it is a duplicate, or none. |
| `Sent` | `at: f64, handle: i64` | Handed to the device. |
| `Got` | `at: f64, status: i32, headers, body, location` | A response; `location` for redirects. (`cut`, for a body past the limit, comes with the device, C2.) |
| `Failed` | `at: f64, kind: Failure` | No HTTP response: timed out, refused, DNS, TLS (the talk's OS-level errors). |
| tag `Slow` | | Sent long ago and no answer yet (a warning, not a timeout). |
| tag `GaveUp` | | Tries again used up. |
| tag `Followed`, `RedirectLimit` | | Its redirect was followed; or not, past `Net.redirects`. |
| tag `Held` | | The program holds it back from sending. |
| tag `Handled` | | The program is done with its answer; clean-up may drop the body. |
| tag `Known` | | Not a duplicate: the ask for its URL that later ones are duplicates of. |
| unique `Net` | `timeout, slow_after, retries, backoff, redirects` | The pipeline's settings. |
| unique `Now` | | The time, as the device says (the mock's own clock). |
| buffer `History` | `ask, at, status` | A row for each answer (the talk's history). |
| index `ByUrl`, `Kept` | | Hash indexes by URL of every ask, and of the known ones. |

`enum Method { Get, Head, Post, Put, Patch, Delete, Options }` (another
by name comes with the device), `enum Failure { Refused, Timeout, Dns,
Tls, TooBig, Other }`. The crawler only asks with `Get`; the pipeline
handles them all.

A redirect spawns a new `Ask` whose `origin` is the redirected one and
whose `hops` is one more.

### 4.2 The crawler

| Data | Fields | Meaning |
|---|---|---|
| component `Depth` | `d: i32` | Links from the seed to this page (a redirect's: the redirected ask's). |
| component `At` | `host: entity` | The host it goes to. |
| component `Host` | `name, busy: i32` | A host it may go to (one entity each, from `allow`), and how many asks to it are out. |
| relation `(Ask)-[Links]->(Ask)` | | Page links to page: the found graph, for the report. |
| component `Ending` | `word` | How an ask ended, for the report. |
| unique `Rules` | `seed, allow, max_depth, per_host` | The crawl's settings (crawl.conf). |
| unique `Level`, `Open` | | How deep the links followed are so far; asks not settled. |

A link to a URL `ByUrl` already has adds a `Links` edge to that ask
(and lowers its depth where the link is shorter) and spawns nothing.
Links found in one frame to one new URL spawn an ask each; all but one
are duplicates, merged by the crawler's `merge` into it (§5.2).

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
| `number_asks` | `Ask` with serial 0 | Gives it the next serial. | |
| `drop_duplicates` | new asks, `Kept`, `ByUrl` | The talk's "drop the new one": an ask for a URL a known ask has is its duplicate (`Twin.of`); of new asks for one URL all but the one `ByUrl` names first. Two `get`s, no loop over the asks. | |
| `forget_duplicates` | `Twin` | Destroys the duplicates, after the program had them in the frame. | |
| `time_out` | `Sent` without an answer, `Now` | Adds `Failed { kind: Timeout }`. (Cancels at the device: C2.) | |
| `warn_slow` | `Sent` without an answer, `Now` | Adds `Slow` past `slow_after`, once. | yes |
| `retry` | `Failed`, `Got` with 5xx or 429 | Removes `Sent` and the answer, bumps `tries` and `next_at` (backoff doubling); past the limit adds `GaveUp`. | |
| `follow_redirect` | `Got` with 3xx and a location | Spawns the next `Ask` (`hops` one more) up to `Net.redirects`, else `RedirectLimit`. | |
| `log` | `Got` (`on added`) | One line for each answer. | yes |
| `history` | `Got` (`on added`) | A row of `History` for each answer. | yes |
| `clean_up` | `Handled` (`on added`) | Drops the body (gives its bytes back). | yes |

Sending and receiving are the device's: `mock_send`, `answer` and `fill`
(mock.ent) in C1, `send` and `receive` of the live device in C3.

### 5.2 The crawler

| System | Reads | Does | Opt. |
|---|---|---|---|
| `setup` | crawl.conf | Sets `Rules` and `Net`, spawns a `Host` for each allowed host and the seed's `Ask` with `Depth 0` (the talk's UI event to request data). | |
| `inherit_depth`, `take_depth` | an `Ask` without `Depth` | A redirect's ask is as deep as its origin (two steps: a query that gives entities a `Depth` does not read that of others). | |
| `place` | an `Ask` without `At` | Its host, by the hash index `HostByName`; an ask to a host not allowed is destroyed. | |
| `be_polite` | `Host`, `At`, `Sent` | At most `per_host` asks out to a host at once: the others `Held`, the oldest let go first (`in turn by serial`). | yes |
| `settle` | `Got`, `GaveUp` | Every answer but a page whose links are to be followed is `Handled` as it is. | |
| `extract_links` | `Got` 200 with HTML, `Depth`, `Level` | Scans the body for links (§7.3); for each to an allowed host an `Ask` with `Depth d + 1`, or an edge to the one `ByUrl` has (its depth lowered); adds `Handled`. Only once every ask less deep is settled, and not at `max_depth`. | |
| `merge` | duplicates | A duplicate's depth and the links to it go to the ask it is one of. | |
| `advance_level` | `Depth`, `Handled` | One level deeper once every ask at this one is settled; `Open`. | |
| `conclude`, `report` | `Got`, `Failed`, `Links` | At the end: `Ending`, then a line for every ask, redirect, redirect not followed, slow ask and broken link (with the page that links to it). | |

### 5.3 Order and the end

One frame (crawl_mock.ent): `inherit_depth take_depth place drop_stale
be_polite mock_send answer fill time_out warn_slow retry follow_redirect
settle extract_links number_asks drop_duplicates merge forget_duplicates
log history clean_up advance_level advance`. The order is the talk's
diagram, so a reader can follow it; the compiler keeps it where systems
touch the same data.

Breadth first by depth: the links of a page are followed only once every
ask less deep is settled (`Level`), so a page's depth is the fewest links
to it from the seed, whatever answers first, and the report does not
depend on the clock. The crawl ends when every ask is settled (`Open`
0); `main` loops the frame until then and then runs `conclude` and
`report`.

## 6. The two programs

`crawl_mock.ent` and `crawl_live.ent` import the same `pipeline.ent` and
`crawl.ent`. They differ only in which systems their schedule names.
That is the talk's offline bypass, as a schedule.

### 6.1 Mock

- `mock.ent` reads the manifest of §7.1 (`load_site`) into `Page`
  entities (`url`, `status`, `delay`, `fails`, `kind`, `location`,
  `body`), found by URL with the hash index `PageByUrl`.
- `mock_send` stands for the device's `send`: an `Ask` due, not held and
  no duplicate gets `Sent` and a `Due` at `Now + delay`. `answer` and
  `fill` stand for `receive`: at that time it gets `Got` (503 for the
  first `fails` tries), unless `time_out` was first; `fill` gives it the
  page's headers, location and body. (Two systems: one that adds a
  component with texts reads none; §10.4.)
- The clock is the unique `Now`, which `advance` moves to the next thing
  due (an answer, a timeout, a slow warning, a retry), so a crawl with
  long delays takes no wall time and the same times on every run.

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
its report with `expected.txt` (site.py works it out with Python's
urllib). Every check runs on the mock. K8, K9
and K11 run on live too.

| # | Check |
|---|---|
| K1 | Every page reachable within `max_depth` is fetched exactly once. |
| K2 | The broken links and the pages that link to them are exactly the expected ones. |
| K3 | Redirect chains are followed to the end. The loop and over-long chains stop at the limit (`RedirectLimit`). |
| K4 | Pages failing the first k times are retried and end as 200. Those always failing end as `GaveUp` after `retries`. |
| K5 | No page beyond `max_depth` is fetched. No other host is fetched. |
| K6 | Slow pages are warned about once each. Pages past the timeout end as `Timeout`. |
| K7 | Built without the optional systems (`warn_slow`, `log`, `history`, `clean_up`, `be_polite`), the program compiles with no change to the other systems and gives the same report (less what those systems add). |
| K8 | Live and mock give the same report on the same site. |
| K9 | Politeness: no host sees more than its limit in flight or more sends per interval than allowed (from `history`). |
| K10 | `crawl_combined.ent`, with `time_out`, `warn_slow`, `retry` and `follow_redirect` as one system, gives the same report. (With the live device `send` and `receive` join them, C3.) |
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

### 10.4 Met in C1

What the crawler ran into, for the open-topics list; each was worked
around in the example, not in the compiler:

- **A component with texts is not added while a text is held.** A
  system that holds a `let` of a text cannot add or remove a component
  with text fields (the move would leave the `let` pointing at freed
  bytes). So the mock's answer is `answer` (adds `Got` with empty texts)
  and `fill` (sets them), and a redirect's depth goes through
  `Inherited`.
- **A `for` inside a `for` uses no index.** Dropping duplicates as "the
  asks with this URL" was a loop over all asks for each new one: 7 s at
  3·10⁴ pages, 138 s at 10⁵. As a `Twin` field set by `get` on two hash
  indexes it is 0.97 s and 4.5 s.
- **A hash index was made anew when entities came or moved** (fixed
  after C1). At 10⁵ pages `ByUrl` was made 246 times (1.4 s of 4.5) and
  `Kept` 123 times (0.7 s): every frame spawns asks. It is now mended
  where no key was written (docs/indexes.md, 5.1): 0.11 s for both. And
  a write to any field of `Ask` (`serial`, `tries`) counted as a write
  of its key; now only `url` does.
- **No running sum in an `on added` for**, no `else if let`, no `var`
  of text, no `let` of a unique's text, no `while` in a fn (loops over
  texts are fns calling themselves).
- **URLs:** percent-decoding of unreserved bytes (§7.3) is not done;
  site.py makes no links that need it.

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
