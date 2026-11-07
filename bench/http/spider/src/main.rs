//! The crawl of http-SPEC.md §9.2 by spider (bench/http/others.py): from
//! the seed, every page of its host by its links, robots.txt obeyed,
//! through the proxy (the local server, which keeps the site's own URLs),
//! at most `--conns` requests out at once; retries `--retries` times,
//! 10 ms apart, follows `--redirects` redirects. Says how many pages it
//! got.
//!
//!   spider-crawl [--seed URL] [--proxy URL] [--conns N] [--retries R]
//!                [--redirects N] [--timeout-ms MS] [--resources]
use spider::retry_strategy::{AttemptOutcome, RetryDirective, RetryStrategy};
use spider::tokio;
use spider::website::Website;
use std::sync::Arc;
use std::time::Duration;

/// Retries after the same 10 ms as the others (crawl.conf's
/// `backoff_ms`), where spider's own backoff starts at a second and goes
/// up to a minute.
struct Retries(u32);

impl RetryStrategy for Retries {
    fn max_retries(&self) -> u32 {
        self.0
    }
    fn on_retry(&self, _: &AttemptOutcome) -> RetryDirective {
        RetryDirective {
            backoff: Some(Duration::from_millis(10)),
            ..RetryDirective::continue_default()
        }
    }
}

#[tokio::main]
async fn main() {
    let mut seed = "http://site.test/".to_string();
    let mut proxy = String::new();
    let (mut conns, mut retries, mut redirects, mut timeout) = (64usize, 2u8, 3usize, 2000u64);
    let mut resources = false;
    let mut args = std::env::args().skip(1);
    while let Some(flag) = args.next() {
        let mut value = || args.next().expect("a value after the flag");
        match flag.as_str() {
            "--seed" => seed = value(),
            "--proxy" => proxy = value(),
            "--conns" => conns = value().parse().unwrap(),
            "--retries" => retries = value().parse().unwrap(),
            "--redirects" => redirects = value().parse().unwrap(),
            "--timeout-ms" => timeout = value().parse().unwrap(),
            "--resources" => resources = true,
            other => panic!("unknown flag {other}"),
        }
    }
    let mut website = Website::new(&seed);
    website
        .with_respect_robots_txt(true)
        .with_subdomains(false)
        .with_tld(false)
        .with_depth(0)
        .with_ignore_sitemap(true)
        .with_concurrency_limit(Some(conns))
        .with_retry(retries)
        .with_redirect_limit(redirects)
        .with_request_timeout(Some(Duration::from_millis(timeout)))
        .with_full_resources(resources);
    website.with_retry_strategy(Arc::new(Retries(retries as u32)));
    if !proxy.is_empty() {
        website.with_proxies(Some(vec![proxy]));
    }
    website.crawl().await;
    println!("pages {}", website.get_links().len());
}
