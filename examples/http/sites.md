# Public sites the crawler may crawl

http-SPEC.md §9.3. Run by hand, never by the checks or the benchmarks,
which stay on `127.0.0.1`.

| Site | Why it may be crawled |
|---|---|
| `http://quotes.toscrape.com/` | Made by Zyte for people learning to scrape: "a sandbox for testing your web scraping skills". No robots.txt (404): every page allowed. |
| `http://books.toscrape.com/` | The same, a book shop of 1,000 books with their pictures, made to be crawled. No robots.txt. |
| `https://crawler-test.com/` | Made for testing crawlers (by Lumar, once DeepCrawl): pages of odd links, redirects, status codes, robots.txt rules, encodings. Its robots.txt shuts off the endless parts (`/infinite`, `/speed_test`), and those are obeyed. |

## The rules

- robots.txt obeyed (RFC 9309: the group naming `ent-crawl`, else `*`'s;
  wildcards; the longest rule wins).
- One ask out to a host at a time, and a second between asks at least,
  or the `Crawl-delay` where longer (`per_host 1`, `min_delay_ms 1000`).
- The user agent names the crawler and the project:
  `ent-crawl/0.1 (+https://github.com/mzaks/ent-lang)`.
- A cap on depth and on asks for every run (`max_depth`, `max_pages`;
  past the cap an ask is reported `capped` and not sent).

## How

Each site is fetched twice: once by `bench/http/mirror.py` (Python's
standard library alone: its own HTML parser, urllib, its own robots.txt
reader; none of the crawler's code), which keeps what it got, and once
by `crawl_live.ent` with the same caps. Then the two are held against
each other, and the mirror is crawled locally, with no delay:

```sh
python3 bench/http/mirror.py --seed http://quotes.toscrape.com/ \
    --allow quotes.toscrape.com --max-depth 12 --max-pages 1500 --out /tmp/quotes
python3 bench/http/public.py /tmp/quotes --public --local
```

`public.py` says, for the mirror's HTML pages, which links the scanner
found and Python did not and the other way round (K12 on real pages),
and which URLs either crawl asked for that the other did not. The
mirror of books.toscrape.com is the second site of §9.2
(`bench/http/others.py --site DIR`).

## What it found (2026-11-10)

| Site | Asks | Links on its pages | Scanner against Python | ent's crawl against mirror.py's | Its mirror crawled locally |
|---|---|---|---|---|---|
| quotes.toscrape.com (depth 12) | 267 | 4,451 | all alike | 317 report lines, all alike | all alike |
| books.toscrape.com (all of it) | 3,204 | 43,724 | all alike | 3,204 lines, all alike | all alike |
| crawler-test.com (depth 3) | 1,218 | 6,997 | all alike, after the fixes below | 1,250 lines alike, 18 not: below | (HTTPS: serve.c answers as a proxy for plain HTTP only) |

Politeness, from the crawler's own history: on quotes 267 asks over
266 s, on books 3,204 over 3,205 s; never two out at once, never less
than 1.000 s apart.

crawler-test.com had what the generated site had not, each now a case of
site.py's /cases.html (K12) and of §7.3:

- `href= '..'`, `href =\n'..'` and `href=' http://..'`: spaces and line
  ends around `=` and at a value's ends. The scanner wanted `href=`
  written together, and kept the spaces.
- `&nbsp;` in a URL: only `&amp;` was decoded.
- `URL with spaces` and bytes past ASCII: kept raw, so the URL was sent
  as no server reads it. Now percent-encoded as the URL standard does
  (`%20`, `%C2%A0`).
- `href=` in a script's text and in another attribute's value
  (`onchange="var href=this[..]"`): taken for links. Scripts are now read
  as comments are (their `src` kept), and an attribute counts only in a
  tag, outside quotes.
- The reference was wrong too: urllib leaves `../` in an absolute
  `<base href>`, and Python's `strip()` takes away U+00A0. site.py now
  removes dot segments itself and strips ASCII spaces only.
- `crawler-test.com:8080` is another host (mirror.py had counted it the
  same).

On crawler-test.com the two crawls differ where they cannot agree:

- URLs made anew at each visit (`expiring_page_for_removed_test/<time>`,
  `with_session_id?sessionID=<random>`): 12 lines each way.
- `status_100`, `status_101`, `status_102` (on http and https): libcurl
  takes a 1xx for an answer to come and waits for the last one, which
  never comes (`timeout`); Python's http.client takes 101 and 102 for
  the answer and fails at 100.

And the crawl at first never ended: a redirect to a host the crawl may
not go to (`external_redirect`, `crawler-test.com:8080`) was an ask
kept, and counted, in the frame it was made and destroyed in the next,
never counted settled. Fixed in `place`; site.py's /cases.html has such
a redirect, which the crawler of before stalls on.
