#!/usr/bin/env python3
"""Picks the fixed sample of SAT Competition 2023 main-track problems that
bench/sat/run.py times, and writes it to bench/sat/sample.txt.

  bench/sat/sample.py          # only where sample.txt is to be made anew

The track's problems from the Global Benchmark Database
(benchmark-database.de), those of at most 20 MB compressed (entsat reads a
file whole into a buffer), in the order of their hashes, and of those 100
evenly spaced. A line of sample.txt is a problem's hash and its file name.
"""
import os
import subprocess
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
QUERY = "https://benchmark-database.de/getinstances?query=track%3Dmain_2023&context=cnf"
LIMIT = 20_000_000
COUNT = 100


def head(url):
    out = subprocess.run(["curl", "-sIL", "--max-time", "60", url],
                         capture_output=True, text=True).stdout
    size, name = 0, ""
    for line in out.splitlines():
        key, _, value = line.partition(":")
        if key.lower() == "content-length":
            size = int(value)
        elif key.lower() == "content-disposition":
            name = value.split("filename=")[-1].strip()
    return size, name


def main():
    urls = urllib.request.urlopen(QUERY).read().decode().split()
    urls = [u.replace("http:", "https:", 1) for u in urls]
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(24) as pool:
        heads = list(pool.map(head, urls))
    small = sorted((u.rsplit("/", 1)[1], name) for u, (size, name) in zip(urls, heads)
                   if 0 < size <= LIMIT)
    picked = [small[i * len(small) // COUNT] for i in range(COUNT)]
    with open(os.path.join(HERE, "sample.txt"), "w") as f:
        for hash_, name in picked:
            f.write(f"{hash_} {name.removeprefix(hash_ + '-').removesuffix('.xz')}\n")
    print(f"{len(urls)} problems, {len(small)} of at most {LIMIT // 1_000_000} MB, {COUNT} picked")


if __name__ == "__main__":
    main()
