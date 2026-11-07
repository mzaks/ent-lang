"""The crawl of http-SPEC.md §9.2 by Scrapy (bench/http/others.py): from
the seed, every page of the allowed host by its links (`href` and `src`
of `a`, `area` and `img`), robots.txt obeyed, through the proxy (the
local server, which keeps the site's own URLs; `http_proxy` in the
environment), at most CONNS requests out at once, no delay. Retries the
answers that may come right RETRIES times, follows REDIRECTS redirects.
Says how many pages it got.

  scrapy runspider crawl_scrapy.py -a seed=URL -a allow=HOST \\
      -s CONCURRENT_REQUESTS=64 -s CONCURRENT_REQUESTS_PER_DOMAIN=64 ...
(others.py gives the settings.)
"""
import urllib.parse

import scrapy
from scrapy.linkextractors import LinkExtractor


def normal(url):
    """The URL as the other crawlers know it: scheme and host in lower
    case, no default port, no fragment."""
    parts = urllib.parse.urlsplit(url)
    scheme, host = parts.scheme.lower(), (parts.hostname or "")
    port = parts.port
    if port and not ((scheme, port) in (("http", 80), ("https", 443))):
        host = f"{host}:{port}"
    return urllib.parse.urlunsplit((scheme, host, parts.path or "/",
                                    parts.query, ""))


class Crawl(scrapy.Spider):
    name = "crawl"

    def __init__(self, seed="http://site.test/", allow="site.test", **kw):
        super().__init__(**kw)
        self.start_urls = [seed]
        self.allowed_domains = [allow]
        self.allow = allow
        # (Not `allow_domains`: it holds a host with its port, the
        # default one too, as another: `http://site.test:80/` is not
        # site.test there. The host is looked at below.)
        self.links = LinkExtractor(tags=("a", "area", "img"),
                                   attrs=("href", "src"),
                                   deny_extensions=[], unique=True)
        self.pages = 0

    def parse(self, response):
        self.pages += 1
        if not isinstance(response, scrapy.http.TextResponse):
            return
        for link in self.links.extract_links(response):
            url = normal(link.url)
            if urllib.parse.urlsplit(url).hostname == self.allow:
                yield response.follow(url, self.parse)

    def closed(self, reason):
        print(f"pages {self.pages}", flush=True)
