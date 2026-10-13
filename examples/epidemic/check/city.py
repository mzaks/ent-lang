#!/usr/bin/env python3
"""The city as it is built, against the reference's targets (the spec's
second check).

  examples/epidemic/reference/fetch.sh      once
  examples/epidemic/check/city.py

Builds check/city_check.ent, which makes a city of 100,000 and lists
it, and holds what comes out against the reference:

- the ages and the sizes of the households, against the country's (the
  parameter file's): each share within half a point of a hundredth;
- the networks: everybody in that of their age, but for staff of
  working age in those of the young and the old, 0.2 for each of their
  own;
- the ties of each network: twice what a member meets in a day on
  average, exactly, and spread about that as the reference's own way of
  rewiring a ring spreads them (which is done here as the reference's C
  does it, on a ring of 4,000);
- a day's meetings: at work half of a person's ties on average, at home
  everybody of the household.

Says what fails, and at the end how many numbers were held. Needs the
compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
"""

import collections
import math
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
RUN = os.path.join(ROOT, "build", "epidemic", "run")

AGE_SHARES = [8054, 7528, 8712, 8835, 8500, 8968, 7069, 5488, 3281]
HOUSEHOLD_SIZES = [7452, 9936, 4416, 4140, 1104, 552]
OWN_NETWORK = [0, 1, 2, 2, 2, 2, 2, 3, 4]
MEETS = [10, 10, 7, 3, 3]
STAFF = 0.2
DAILY = 0.5
REWIRE = 0.1


def reference_ring(n, k, generator):
    """The degrees of a ring of n rewired as the reference's
    build_watts_strogatz_network does it (src/network.c)."""
    right = k // 2
    edges = [[(i + j + 1) % n for j in range(right)] for i in range(n)]
    for i in range(n):
        for j in range(right):
            edges[edges[i][j]].append(i)
    for i in range(n):
        j = 0
        while j < len(edges[i]):
            if generator.random() < REWIRE:
                new = generator.randrange(n)
                while new == i or new in edges[i]:
                    new = generator.randrange(n)
                old = edges[i][j]
                edges[old].remove(i)
                edges[i][j] = new
                edges[new].append(i)
            j += 1
    return [len(e) for e in edges]


def moments(values):
    n = len(values)
    mean = sum(values) / n
    return mean, sum((v - mean) ** 2 for v in values) / n


class Report:
    def __init__(self):
        self.held = 0
        self.failed = 0

    def hold(self, what, found, expected, allowed):
        self.held += 1
        if abs(found - expected) > allowed:
            self.failed += 1
            print(f"{what}: {found:.5f}, expected {expected:.5f} (+- {allowed:.5f})")


def main():
    panel = os.path.join(RUN, "baseline_household_demographics.csv")
    if not os.path.isfile(panel):
        sys.exit("no household panel: run examples/epidemic/reference/fetch.sh")
    program = os.path.join(RUN, "city_check")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
         "-I", os.path.join(HERE, ".."),
         os.path.join(HERE, "city_check.ent"), "-o", program],
        check=True, env=environment)
    lines = subprocess.run([program], check=True, capture_output=True,
                           text=True, cwd=RUN).stdout.split("\n")
    words = lines[0].split()
    fit, ties = float(words[1]), int(words[5])
    work = lines.index("work")
    home = lines.index("home")
    people = [tuple(map(int, line.split(","))) for line in lines[1:work]]
    met = {int(a): float(b) for a, b in
           (line.split(",") for line in lines[work + 1:home])}
    at_home = {int(a): float(b) for a, b in
               (line.split(",") for line in lines[home + 1:] if line)}
    n = len(people)
    report = Report()
    report.hold("people", n, 100000, 0)
    report.hold("the taking's misfit", fit, 0.0, 1e-5)

    ages = collections.Counter(age for _, age, _, _, _ in people)
    for age, share in enumerate(AGE_SHARES):
        report.hold(f"age {age}: share", ages[age] / n, share / sum(AGE_SHARES), 0.005)
    size_of = collections.Counter(house for _, _, house, _, _ in people)
    sizes = collections.Counter(size_of.values())
    for size, share in enumerate(HOUSEHOLD_SIZES):
        report.hold(f"households of {size + 1}: share", sizes[size + 1] / len(size_of),
                    share / sum(HOUSEHOLD_SIZES), 0.005)
    report.hold("households bigger than 6", sum(c for s, c in sizes.items() if s > 6), 0, 0)

    report.hold("ties", sum(t for *_, t in people) / 2, ties, 0)
    working = sum(1 for _, age, _, _, _ in people if OWN_NETWORK[age] == 2)
    generator = random.Random(4321)
    for network in range(5):
        members = [p for p in people if p[3] == network]
        own = sum(1 for p in members if OWN_NETWORK[p[1]] == network)
        staff = len(members) - own
        if network == 2:
            report.hold("work: members not of working age", staff, 0, 0)
        else:
            report.hold(f"network {network}: members of another age than its own or "
                        "the working one",
                        sum(1 for p in members if OWN_NETWORK[p[1]] not in (network, 2)),
                        0, 0)
            chance = own * STAFF / working
            report.hold(f"network {network}: staff for each of its own", staff / own,
                        STAFF, 4 * math.sqrt(working * chance * (1 - chance)) / own)
        k = int(MEETS[network] / DAILY)
        mean, spread = moments([p[4] for p in members])
        report.hold(f"network {network}: ties each", mean, k, 1e-9)
        samples = [moments(reference_ring(4000, k, generator))[1] for _ in range(6)]
        expected = sum(samples) / len(samples)
        # (A variance of so many degrees is known to a part in the root
        # of half as many; the six rings give the reference's as well.)
        allowed = 4 * expected * (math.sqrt(2 / len(members)) + math.sqrt(2 / 24000))
        report.hold(f"network {network}: spread of the ties", spread, expected, allowed)
        mean_met, _ = moments([met[p[0]] for p in members])
        report.hold(f"network {network}: met on a day", mean_met, MEETS[network],
                    4 * math.sqrt(k * DAILY * (1 - DAILY) / len(members)) + 0.01)
        print(f"  network {network}: {len(members)} members, {staff} staff, "
              f"ties {mean:.2f} spread {spread:.3f} ({expected:.3f}), "
              f"met {mean_met:.3f}")

    wrong = sum(1 for p in people if at_home[p[0]] != 2 * size_of[p[2]])
    report.hold("people whose meetings at home are not their household", wrong, 0, 0)

    print(f"{report.held} numbers held against the reference, {report.failed} off")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
