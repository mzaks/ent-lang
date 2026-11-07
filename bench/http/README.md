# bench/http

The crawler's benchmarks (examples/http/http-SPEC.md, §9); the numbers
are in RESULTS.md.

| Script | What it measures |
|---|---|
| `mock.py` | `crawl_mock` on generated sites, no network (§9.1). |
| `live.py` | `crawl_live` against `crawl.c` (curl multi) through the local server, `serve.c` (§9.1). |
| `others.py` | Both against wget, Scrapy, Colly and spider through the same server, each crawler's asks verified against `crawl.c`'s first (§9.2). |

Run them without the pixi toolchain on `PATH` (its `cc` cannot link
libcurl): `env PATH=/usr/local/bin:/usr/bin:/bin python3 bench/http/...`.

## The other crawlers

Their setups are here; what they are built into is in `build/crawlers/`
(not in the repository). Versions as measured:

| Crawler | Version | Setup | Made with |
|---|---|---|---|
| wget | GNU Wget 1.25.0 | command line in `others.py` | conda-forge |
| Scrapy | 2.19.0 (Python 3.13.18, Twisted 26.10.0, lxml 6.1.3, parsel 1.12.1) | `scrapy/crawl_scrapy.py` | conda-forge |
| Colly | v2.3.0 (Go 1.26.1) | `colly/` | `go build` |
| spider | 2.53.9 (Rust 1.99.0, tokio 1.54.1, reqwest 0.13.5) | `spider/` | `cargo build --release` |

```
# wget and Scrapy: a pixi environment
mkdir -p build/crawlers/py && cd build/crawlers/py
pixi init . && pixi add wget=1.25.0 scrapy=2.19.0 python=3.13
cd -

# Colly (modules kept under build/ too)
cd bench/http/colly
GOPATH=$PWD/../../../build/crawlers/go GOMODCACHE=$PWD/../../../build/crawlers/go/mod \
  go build -o ../../../build/crawlers/colly .
cd -

# spider
cd bench/http/spider
RUSTFLAGS="-C target-cpu=native" CARGO_TARGET_DIR=$PWD/../../../build/crawlers/spider-target \
  cargo build --release
cd -
```

Each is told to do the crawl `crawl.c` does, as near as it can be: the
seed, the one host, no depth limit, `robots.txt` obeyed, no delay, the
same requests out at once (wget: one), 2 tries after the first for
429 and 5xx answers and failures, 10 ms between them where that can be
set, 3 redirects, 2 s for an answer; links from `href` and `src`
(`a`, `area`, `img`). Where a setup had to help (Colly: URLs put in
their normal form before they are visited and one visit at a time, a
retry counted by URL; Scrapy: the host looked at without its port;
spider: 10 ms between retries), its source says why. What each still
does otherwise is in RESULTS.md with its numbers.
