// The crawl of http-SPEC.md §9.2 by Colly (bench/http/others.py): from
// the seed, every page of the allowed host by its links (`href` and
// `src`), robots.txt obeyed, through the proxy (the local server, which
// keeps the site's own URLs), at most `-conns` requests out at once.
// Retries the answers that may come right (5xx, timeouts) `-retries`
// times, follows `-redirects` redirects. Says how many pages it got.
package main

import (
	"errors"
	"flag"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gocolly/colly/v2"
)

func main() {
	seed := flag.String("seed", "http://site.test/", "first URL")
	allow := flag.String("allow", "site.test", "the host to crawl")
	proxy := flag.String("proxy", "", "HTTP proxy")
	conns := flag.Int("conns", 64, "requests out at once")
	retries := flag.Int("retries", 2, "tries after the first")
	redirects := flag.Int("redirects", 3, "redirects followed")
	timeout := flag.Int("timeout-ms", 2000, "timeout of a request")
	flag.Parse()

	c := colly.NewCollector(colly.AllowedDomains(*allow), colly.Async(true))
	c.IgnoreRobotsTxt = false
	c.WithTransport(&http.Transport{
		MaxIdleConns:        *conns,
		MaxIdleConnsPerHost: *conns,
		MaxConnsPerHost:     *conns,
	})
	if *proxy != "" {
		if err := c.SetProxy(*proxy); err != nil {
			panic(err)
		}
	}
	c.SetRequestTimeout(time.Duration(*timeout) * time.Millisecond)
	if err := c.Limit(&colly.LimitRule{DomainGlob: "*", Parallelism: *conns}); err != nil {
		panic(err)
	}
	c.SetRedirectHandler(func(req *http.Request, via []*http.Request) error {
		if len(via) > *redirects {
			return errors.New("too many redirects")
		}
		return nil
	})
	var pages atomic.Int64
	c.OnResponse(func(r *colly.Response) { pages.Add(1) })
	// (Colly asks its store whether a URL was visited and then marks it,
	// two steps: pages parsed at once may both visit one URL. One visit
	// at a time keeps its dedupe as it means to be.)
	var visiting sync.Mutex
	// (And Colly knows a URL as it is written; the others, and the site's
	// expected report, by its normal form: scheme and host in lower case,
	// no default port, no fragment.)
	visit := func(e *colly.HTMLElement, link string) {
		absolute := e.Request.AbsoluteURL(link)
		u, err := url.Parse(absolute)
		if err != nil || absolute == "" {
			return
		}
		u.Scheme = strings.ToLower(u.Scheme)
		u.Host = strings.ToLower(u.Host)
		if (u.Scheme == "http" && u.Port() == "80") || (u.Scheme == "https" && u.Port() == "443") {
			u.Host = u.Hostname()
		}
		u.Fragment, u.RawFragment = "", ""
		visiting.Lock()
		defer visiting.Unlock()
		e.Request.Visit(u.String())
	}
	c.OnHTML("a[href]", func(e *colly.HTMLElement) { visit(e, e.Attr("href")) })
	c.OnHTML("img[src]", func(e *colly.HTMLElement) { visit(e, e.Attr("src")) })
	var tries sync.Map
	c.OnError(func(r *colly.Response, err error) {
		if r.StatusCode != 0 && r.StatusCode < 500 {
			return
		}
		// (Counted by URL: a request a page visits shares the page's Ctx.)
		count, _ := tries.LoadOrStore(r.Request.URL.String(), new(atomic.Int32))
		if int(count.(*atomic.Int32).Add(1)) <= *retries {
			if err := r.Request.Retry(); err != nil {
				fmt.Fprintln(os.Stderr, "retry:", err)
			}
		}
	})
	if err := c.Visit(*seed); err != nil {
		panic(err)
	}
	c.Wait()
	fmt.Printf("pages %d\n", pages.Load())
}
