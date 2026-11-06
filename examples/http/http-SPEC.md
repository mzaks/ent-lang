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
| `devices/http.ent`, `devices/http.c` | The HTTP client device, a module from the start (§6.2), with its own tests in `test/Integration/http_device.test` (D1 to D10, §8.1; built in C2). |
| `crawl_mock.ent`, `crawl_live.ent` | The two programs: the same layers, another schedule (§6). |
| `crawl_lean.ent` | crawl_mock.ent without the optional systems (§8, K7). |
| `crawl_combined.ent` | Four of the pipeline's systems as one (§8, K10). |
| `scan_links.ent` | The link scanner alone, every link of every page (§8, K12). |
| `robots.ent` | Fns: a robots.txt's rules for the crawler and whether they allow a path (§8.2, K11). |
| `serve.c` | The crawl's own server (§7.2, C3): answers a site as its manifest says. |
| `test/Integration/Inputs/http/serve.py` | The device tests' server (§7.2, C2). |
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
| `Got` | `at: f64, status: i32, headers, body, location` | A response; `location` for redirects. (`cut`, for a body past the limit, comes with the live device, C3.) |
| `Failed` | `at: f64, kind: Failure` | No HTTP response: timed out, refused, DNS, TLS (the talk's OS-level errors). |
| tag `Slow` | | Sent long ago and no answer yet (a warning, not a timeout). |
| tag `GaveUp` | | Tries again used up. |
| tag `Followed`, `RedirectLimit` | | Its redirect was followed; or not, past `Net.redirects`. |
| tag `Held` | | The program holds it back from sending. |
| tag `Handled` | | The program is done with its answer; clean-up may drop the body. |
| tag `Known` | | Not a duplicate: the ask for its URL that later ones are duplicates of. |
| unique `Net` | `timeout, slow_after, retries, backoff, redirects` | The pipeline's settings. |
| unique `Now` | | The time, as the device says (the mock's own clock). |
| buffer `History` | `ask, sent, at, status` | A row for each answer and each try that failed (status 0): when it was sent and answered (the talk's history). |
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
| component `Host` | `name, busy: i32, turns: i64, let_go: i64` | A host it may go to (one entity each, from `allow`); how many asks to it are out (or let go and not sent yet); how many of its asks have a turn, and how many of those were let go. |
| component `Turn` | `n: i64` | An ask's place among its host's, in the order they were made (`be_polite`). |
| component `LinkedFrom` | `page` | The page whose link made this ask: the `Links` edge is connected once the ask is known to be no duplicate (`merge`). |
| tag `Html` | | An answer of 200 with HTML: a page whose links may be followed (`classify`, once, as it comes). |
| relation `(Ask)-[Links]->(Ask)` | | Page links to page: the found graph, for the report. |
| component `Robots` | `ask, known, rules, delay: f64, next: f64` | On a `Host`: the ask for its robots.txt, whether it is known (answered or failed for good), its rules for the crawler (none where it was not a 200), its `Crawl-delay` and when the next ask may go. |
| tag `RobotsAsk`, `Disallowed` | | The ask for a robots.txt (nothing holds it back); an ask robots.txt does not allow (held for good). |
| component `Ending` | `word` | How an ask ended, for the report. |
| unique `Rules` | `seed, allow, max_depth, per_host` | The crawl's settings (crawl.conf). |
| unique `Level`, `Open` | | How deep the links followed are so far; asks not settled. |

A link to a URL an ask has that is known to be no duplicate (`Kept`)
adds a `Links` edge to that ask (and lowers its depth where the link is
shorter) and spawns nothing. Links found in one frame to one new URL
spawn an ask each, which carries the page (`LinkedFrom`); all but one
are duplicates, merged by the crawler's `merge` into it, the edge from
each page then connected to the one kept (§5.2). So nothing reads
`Links` before the report.

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
| `time_out` | `Sent` without an answer, `Now` | Adds `Failed { kind: Timeout }`. (Cancels at the device: C3.) | |
| `warn_slow` | `Sent` without an answer, `Now` | Adds `Slow` past `slow_after`, once. | yes |
| `retry` | `Failed`, `Got` with 5xx or 429 | Removes `Sent` and the answer, bumps `tries` and `next_at` (backoff doubling); past the limit adds `GaveUp`. | |
| `follow_redirect` | `Got` with 3xx and a location | Spawns the next `Ask` (`hops` one more) up to `Net.redirects`, else `RedirectLimit`. | |
| `log` | `Got` (`on added`) | One line for each answer. | yes |
| `history` | `Got`, `Failed` (`on added`) | A row of `History` for each answer and each failed try. | yes |
| `say_history` | `History` | At the end, a line `history sent at status url` for each row (for K9). | yes |
| `clean_up` | `Handled` (`on added`) | Drops the body (gives its bytes back). | yes |

Sending and receiving are the device's: `mock_send`, `answer` and `fill`
(mock.ent) in C1, `send` and `receive` of the live device in C3.

### 5.2 The crawler

| System | Reads | Does | Opt. |
|---|---|---|---|
| `setup` | crawl.conf | Sets `Rules` and `Net`, spawns a `Host` for each allowed host with the ask for its robots.txt (`Depth 0`), and the seed's `Ask` with `Depth 0` (the talk's UI event to request data). | |
| `inherit_depth`, `take_depth` | an `Ask` without `Depth` | A redirect's ask is as deep as its origin (two steps: a query that gives entities a `Depth` does not read that of others). | |
| `place` | an `Ask` without `At` | Its host, by the hash index `HostByName`, once the host's robots.txt is known (`Held` till then); an ask to a host not allowed is destroyed, one robots.txt does not allow is `Disallowed`, `Handled` and held for good. | |
| `read_robots` | `Robots`, the robots ask's `Got` or `Failed` | Once it is answered or has failed for good (after `retry`): its rules (robots.ent) where it answered 200, none else; the host's `known`. | |
| `be_polite` | `Host`, `At`, `Sent` | At most `per_host` asks out to a host at once: the others `Held`, the oldest let go first. Each new ask takes the next `Turn` at its host (`in turn by serial`, of the new ones) and is `Held`; each frame the host lets go as many more turns as it has room for, and the asks whose turn has come lose `Held` (no order of all that wait). | yes |
| `pace` | `Robots`, `At` | A host's `Crawl-delay`: of its asks due, the oldest goes once the host's time has come, which is then that much later; the others wait for it (`next_at`). | yes |
| `classify` | `Got`, on added | `Html` where it is a 200 with HTML (the headers lowered and searched once). | |
| `settle` | `Got`, `GaveUp`, `Html` | Every answer but a page whose links are to be followed is `Handled` as it is. | |
| `extract_links` | `Html`, `Depth`, `Level` | Scans the body for links (§7.3); for each to an allowed host an `Ask` with `Depth d + 1` and `LinkedFrom`, or an edge to the one `Kept` has (its depth lowered); adds `Handled`. Only once every ask less deep is settled, and not at `max_depth`. | |
| `merge` | duplicates, `LinkedFrom` | A duplicate's depth and the link that made it go to the ask it is one of; an ask that is none is linked to from its page. | |
| `advance_level` | `Depth`, `Handled` | One level deeper once every ask at this one is settled; `Open`. | |
| `conclude`, `report` | `Got`, `Failed`, `Links` | At the end: `Ending`, then a line for every ask, redirect, redirect not followed, slow ask and broken link (with the page that links to it). | |

### 5.3 Order and the end

One frame (crawl_mock.ent): `inherit_depth take_depth place drop_stale
be_polite pace mock_send answer fill time_out warn_slow retry
read_robots follow_redirect classify settle extract_links number_asks
drop_duplicates merge forget_duplicates log history clean_up
advance_level advance`. crawl_live.ent has `send receive fetch` where
the mock has `drop_stale mock_send answer fill`, `cancel_late` after
`time_out`, and no `advance` (the clock is the machine's). The order is the talk's
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

`devices/http.ent` and `devices/http.c`, on libcurl's multi interface
(linked by `http.link`), as `extern proc`s (no world for device C). A
module any program can import, not only the crawler. Built in C2; the
module's comments say each proc exactly.

```
enum HttpFailure { Ok, Refused, Timeout, Dns, Tls, TooBig, Other }
buffer Done { handle: i64, status: i32, failure: HttpFailure,
              first_byte: f64, total: f64, cut: bool } capacity 4096

extern proc http_session(user_agent: text, cookies: bool, per_host: i32,
                         total: i32, http2: bool, verify_tls: bool,
                         ca_file: text, proxy: text) -> i64
extern proc http_start(session: i64, method: text, url: text, headers: text,
                       body: text, timeout: f64, max_body: i64,
                       follow: i32) -> i64
extern proc http_cancel(handle: i64)
extern proc http_poll(wait: f64, done: mut Done) -> i32
extern proc http_running() -> i32
// Of one request, once answered (-1 while it runs):
extern proc http_status(handle: i64) -> i32
extern proc http_failure(handle: i64) -> HttpFailure
extern proc http_cut(handle: i64) -> bool
extern proc http_first_byte(handle: i64) -> f64
extern proc http_total(handle: i64) -> f64
extern proc http_body(handle: i64) -> text
extern proc http_headers(handle: i64) -> text
extern proc http_url(handle: i64) -> text
extern proc http_location(handle: i64) -> text
extern proc http_forget(handle: i64)
extern proc http_close(session: i64)
fn header(headers: text, name: text) -> text     // in ent-lang
```

- A session is a multi handle (its connection limits, HTTP/2
  multiplexing) and a share handle (DNS, TLS sessions, and the cookie
  jar where it has one).
- `method` by name: GET and HEAD as curl has them, POST by its body (so
  303 turns it into a GET, 307 and 308 keep it), any other as a custom
  request. No `Expect: 100-continue` unless the headers ask for it.
  Compressed bodies (gzip, brotli, zstd) are asked for and decoded.
- `follow` n: curl follows n redirects and then gives the 3xx as the
  answer (not a failure); `follow` 0 gives each 3xx to the program,
  with `http_location` absolute.
- A body past `max_body` is cut there (the transfer stopped) and marked
  `cut`; the status is kept.
- How the answers come to the entities: `http_poll` waits (in
  `curl_multi_poll`, at most `wait`, not at all where none runs) and
  drives the transfers; then a `for` over the entities that sent asks
  the device for each its own answer by handle (`http_status` and the
  rest). The rows of `Done` are there for a program that wants them as
  rows; they cannot be put on the entities by handle, as a `for` sets
  only its own entity's fields (§10.4).
- Procs run on the main thread, which curl's multi interface wants.
- The crawler opens one session with `follow` 0, so its pipeline sees
  every redirect (live.ent, C3): `send` starts each due ask, `receive`
  polls (5 ms at most; where nothing runs it waits a millisecond) and
  sets `Now` from the machine's clock, `fetch` gives each sent ask its
  `Got` or `Failed`, `cancel_late` stops at the device what `time_out`
  gave up on. `live.conf` names the proxy and the connections.

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

For the device tests (§8.1, built in C2):
`test/Integration/Inputs/http/serve.py`, Python's own `http.server`
(no packages), on `127.0.0.1` plain and with TLS (a self-signed
certificate for 127.0.0.1, `cert.pem`, which the tests trust or not),
and a port nothing listens on. Its endpoints are an httpbin's: echo a
request (method, header lines as received, body), the FNV-1a of a body,
N bytes with their FNV-1a, a status, repeated headers, n redirects of a
given status, a delay, cookies set and asked for, gzip and brotli
bodies. It runs the test's program and stops.

For the crawl and its benchmark (C3): `serve.c`, a C server on epoll
(threads with a socket each on one port), in the repo, no
dependencies. It reads the site's manifest, the file the mock reads,
and answers as the mock does: the status, type, location and body,
after the page's delay (a heap of answers due), 503 for the first
`fails` asks of a page, 404 for any other URL; HTTP/1.1 with
keep-alive. A request names the whole URL, as to a proxy, so the
crawlers keep the site's own URLs (`http://site.test/...`) and go
through their proxy setting; `--log` writes when each request came
and was answered. (nginx would need scripting for the delays and the
fails.) `www/` and its `rules.txt` stay for crawlers that want files.

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
| K8 | Live and mock give the same report on the same site: crawl_live against serve.c, the site's times a tenth as long. |
| K9 | Politeness: no host sees more than its limit in flight or more sends per interval than allowed (from `history`): asks out at once at most `per_host`, sends at least robots.txt's `Crawl-delay` apart. |
| K10 | `crawl_combined.ent`, with `time_out`, `warn_slow`, `retry` and `follow_redirect` as one system, gives the same report. (With the live device `send` and `receive` join them, C3.) |
| K11 | `robots.txt`: disallowed paths are never asked for; `Crawl-delay` is kept; a missing `robots.txt` allows all; one that times out holds the host until it answers or fails. (site.py `--robots none`, `rules`, `slow`, `timeout`; the report says `disallowed URL` for each.) |
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

### 10.4 Met in C1 and C3

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
- **A field of another entity is set only in a `for`, and only
  combined** (`+=`, `max=`). So the rows of `Done` cannot be put on the
  entities that sent them by handle (a hash index finds the entity, but
  nothing may set its fields there): each entity asks the device for its
  own answer instead (§6.2), which is the better shape anyway.
- **No running sum in an `on added` for**, no `let` of a unique's text.
  (`else if let` and `while` in a fn, missing in C1, work now: robots.ent
  and crawl.ent's `place` have them.)
- **A `var` holds no text of any length, and says so only when it is
  typed.** `var all: text = ""` is an error, but `var all = ""` is a
  `text[N]` the size of the literal, and `all = all + rule` keeps cutting
  it to that: robots.txt's rules came out as `a` and nothing was
  disallowed, silently (C3). Texts built in a loop are fns calling
  themselves (robots.ent, `rules_from`). The cut should be an error, or
  the var a `text`.
- **Systems that look at every waiting ask each frame** (C3, live).
  The mock jumps from one thing due to the next: 123 frames at 10⁵
  pages. Live, a frame takes what came in a few milliseconds: 4,223
  frames at 64 connections, and `settle` (pages waiting for their
  level, looked at again each frame), `be_polite` and `pace` (every
  unsent ask sorted by serial), and `Links` sorted anew make ent 10
  times C's time there (bench/http/RESULTS.md, 9.1). Not a cost of the
  pipeline's shape: of what each system visits. Partly fixed: `classify`
  (a page `Html` once), `be_polite` by turns, `Links` connected only once
  an ask is no duplicate, and in the compiler and devices/edges.c a
  `for ... by` that sorts only the entities with its tags, room for new
  entities' runs and sorts anew only once mending cost as much: 22.7 to
  10.6 s (C 2.3). Then, in the compiler, `on added` loops that give
  rows to a buffer, connect or change which entities there are go
  through the log of what happened, not every entity (`history`, `log`:
  0.8 to 0.05 s), and the hash indexes are kept by what changed, not a
  pass over every ask (`Kept` and `ByUrl`: 1.8 to 0.12 s): 7.4 s
  (RESULTS.md, "Live, after on added and hash indexes by what
  changed"). Left: plain loops over every ask (0.15 ms a frame each at
  10⁵), `Links` kept in order, taking links out of pages (the pages
  waiting for their level cost little: held apart, 20 ms less).
- **Two spawns alike in what they require went to one archetype** (fixed
  in C3): asks spawned with `LinkedFrom`, which `merge` removes, and the
  seed's without it, both required `Ask, Twin, Depth`; the compiler made
  one archetype for the first family and put the second's spawns in it,
  which holds no `LinkedFrom` (an error at the spawn). Families alike in
  what they require are one now, holding what either holds optionally.
- **A relation's mending walked far for room** (fixed in C3). The sort
  of `Links` was 30 s of 52 at 10⁶ pages, though it sorted anew only 20
  times: the mending before each (devices/edges.c) looked for room past
  every empty run of the 2M keys, its budget counting edges moved and
  not keys passed. Now 25.5 s for the whole crawl, from 62.
- **URLs:** percent-decoding of unreserved bytes (§7.3) is not done;
  site.py makes no links that need it.

## 11. Milestones

| | What | Done when |
|---|---|---|
| **C0** | §10.1 decided and, if a language feature, built with its own tests. | `extract_links` can make a URL `text`. |
| **C1** | `site.py`, `pipeline.ent`, `crawl.ent`, `mock.ent`, `crawl_mock.ent`, `crawl_combined.ent`, `check/`. | K1 to K7, K10 and K12 (generated cases) pass on the mock; mock throughput measured. |
| **C2** | `devices/http` and the local server with its httpbin endpoints and TLS listener. | D1 to D10 pass (done: test/Integration/http_device.test). |
| **C3** | `crawl_live.ent`, `robots.txt`, `bench/http/crawl.c`. | K8, K9 and K11 pass (done: check/crawl.py --live); the live benchmark against C is measured and written down (done: bench/http/live.py, RESULTS.md: C 2.3 s at 10⁵ pages, ent 5.5 to 22.7 s, per-frame work over every waiting ask; after the first fixes 4.8 to 10.6 s, then 4.7 to 7.4 s). |
| **C4** | The other crawlers' setups, the mirrored real site. | §9.2 measured and written down, every crawler verified first. |
| **C5** | `sites.md`, crawls of the public sites. | §9.3 run; what it found is in K12 and D1 to D10. |
| **C6** | A live window (Clay, `layout`, `window`): requests in flight, slow ones, errors, history. | Shows a crawl as it runs. |

## 12. Open questions

1. §10.1: language feature or stopgap? (The preferred way is the
   feature. It decides C0.)
2. Does the live server go in C (in the repo) or use nginx, and
   go-httpbin for the device tests or our own endpoints? (Decided: our
   own endpoints in Python for the device tests, C2; serve.c for the
   crawl, C3.)
3. Keep every `Ask` as history (the report needs the graph) or destroy
   finished ones and keep only `history` rows? (C1 keeps them.)
4. The user agent string and the project URL in it for §9.3.
5. Which of the other crawlers are worth their setup: all four, or
   start with Scrapy and spider and add the others if the numbers are
   interesting?
