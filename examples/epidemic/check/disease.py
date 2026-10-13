#!/usr/bin/env python3
"""The illness alone, against its tables (the spec's first check).

  examples/epidemic/check/disease.py

Builds check/disease_check.ent, which infects 100,000 people at once
and writes every step of every illness, and holds what comes out against
the numbers in disease.ent:

- how many people are of each age;
- by age, how many of the infected show nothing or little, how many of
  the severe go to hospital, how many there become critical, how many of
  those are taken into intensive care, and how many die there;
- the days each wait takes: their mean and their deviation.

A number passes where it is within four standard errors of what the
table says (for a wait: of what the table's distribution gives once its
days are whole numbers, one at least). Says what fails, and at the end
how many numbers were held against the tables. Needs the compiler built
(build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
"""

import math
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(ROOT, "build", "epidemic")

STAGES = ["Susceptible", "Presymptomatic", "Asymptomatic", "Mild", "Severe",
          "Hospitalised", "Critical", "Recovering", "Recovered", "Dead"]
S = {name: number for number, name in enumerate(STAGES)}

# Which wait a step from one stage to another is.
WAITS = {
    ("Presymptomatic", "Mild"): "Symptoms",
    ("Presymptomatic", "Severe"): "Symptoms",
    ("Asymptomatic", "Recovered"): "AsymptomaticRecovery",
    ("Mild", "Recovered"): "Recovery",
    ("Severe", "Recovered"): "Recovery",
    ("Severe", "Hospitalised"): "Hospital",
    ("Hospitalised", "Critical"): "Critical",
    ("Hospitalised", "Dead"): "Critical",
    ("Hospitalised", "Recovered"): "HospitalRecovery",
    ("Critical", "Dead"): "Death",
    ("Critical", "Recovering"): "IcuLeave",
    ("Recovering", "Recovered"): "HospitalRecovery",
}


def tables():
    """The tables `delays` and `ages` of disease.ent, as it has them."""
    source = open(os.path.join(HERE, "..", "disease.ent")).read()
    number = r"(-?[0-9.]+)"
    delays = {
        name: (float(mean), float(deviation))
        for name, mean, deviation in re.findall(
            r"(\w+):\s*\{ mean: " + number + r",\s*deviation: " + number, source)
    }
    fields = ["share", "susceptible", "asymptomatic", "mild", "hospital",
              "critical", "icu", "die"]
    row = r"\{\s*" + r",\s*".join(f + ": " + number for f in fields) + r"\s*\}"
    ages = [dict(zip(fields, map(float, found)))
            for found in re.findall(row, source)]
    assert len(delays) == 8 and len(ages) == 9, (len(delays), len(ages))
    return delays, ages


def expected_wait(mean, deviation, draws=2_000_000):
    """Mean, deviation and fourth moment about the mean of a wait."""
    if deviation == 0:
        part = mean - math.floor(mean)
        return mean, math.sqrt(part * (1 - part)), part * (1 - part) * (
            (1 - part) ** 3 + part ** 3)
    generator = random.Random(12345)
    shape = (mean / deviation) ** 2
    scale = deviation ** 2 / mean
    values = [max(1, math.floor(generator.gammavariate(shape, scale) + 0.5))
              for _ in range(draws)]
    m = sum(values) / draws
    second = sum((v - m) ** 2 for v in values) / draws
    fourth = sum((v - m) ** 4 for v in values) / draws
    return m, math.sqrt(second), fourth


class Report:
    def __init__(self):
        self.held = 0
        self.failed = 0

    def hold(self, what, found, expected, error, slack=0.0):
        self.held += 1
        if abs(found - expected) > 4 * error + slack:
            self.failed += 1
            print(f"{what}: {found:.4f}, expected {expected:.4f} "
                  f"(+- {4 * error + slack:.4f})")

    def share(self, what, count, of, part):
        """`count` of `of` where a part `part` is expected."""
        if of == 0:
            return
        error = math.sqrt(part * (1 - part) / of)
        # A count is a whole number: one more or less is no finding.
        self.hold(what, count / of, part, error, slack=1.0 / of)


def main():
    os.makedirs(OUT, exist_ok=True)
    program = os.path.join(OUT, "disease_check")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
         "-I", os.path.join(HERE, ".."),
         os.path.join(HERE, "disease_check.ent"), "-o", program],
        check=True, env=environment)
    lines = subprocess.run([program], check=True, capture_output=True,
                           text=True).stdout.split("\n")
    split = lines.index("moves")
    age_of = {}
    for line in lines[:split]:
        person, age = map(int, line.split(","))
        age_of[person] = age
    steps = [tuple(map(int, line.split(","))) for line in lines[split + 1:] if line]

    delays, ages = tables()
    report = Report()
    people = len(age_of)

    all_shares = sum(row["share"] for row in ages)
    for age, row in enumerate(ages):
        report.share(f"age {age}: of all", sum(1 for a in age_of.values() if a == age),
                     people, row["share"] / all_shares)

    # By age: how many took each step, and the days of each wait.
    took = [dict() for _ in ages]
    days = {name: [] for name in delays}
    last = {}
    for person, age, start, end, spent in steps:
        assert age == age_of[person]
        step = (STAGES[start], STAGES[end])
        took[age][step] = took[age].get(step, 0) + 1
        days[WAITS[step]].append(spent)
        last[person] = STAGES[end]

    over = sum(1 for stage in last.values() if stage in ("Recovered", "Dead"))
    report.hold("people whose illness ended", over, people, 0)

    for age, row in enumerate(ages):
        n = lambda start, end: took[age].get((start, end), 0)
        all_of = sum(1 for a in age_of.values() if a == age)
        mild = n("Presymptomatic", "Mild")
        severe = n("Presymptomatic", "Severe")
        asymptomatic = n("Asymptomatic", "Recovered")
        report.hold(f"age {age}: infected", asymptomatic + mild + severe, all_of, 0)
        report.share(f"age {age}: asymptomatic", asymptomatic, all_of,
                     row["asymptomatic"])
        report.share(f"age {age}: mild", mild, all_of, row["mild"])
        hospital = n("Severe", "Hospitalised")
        report.share(f"age {age}: to hospital of severe", hospital, severe,
                     row["hospital"])
        report.hold(f"age {age}: severe, recovered or to hospital",
                    hospital + n("Severe", "Recovered"), severe, 0)
        critical = n("Hospitalised", "Critical") + n("Hospitalised", "Dead")
        report.share(f"age {age}: critical of those in hospital", critical,
                     hospital, row["critical"])
        icu = n("Hospitalised", "Critical")
        report.share(f"age {age}: into intensive care of the critical", icu,
                     critical, row["icu"])
        report.share(f"age {age}: dead of those in intensive care",
                     n("Critical", "Dead"), icu, row["die"])
        report.hold(f"age {age}: out of intensive care, recovered",
                    n("Recovering", "Recovered"), n("Critical", "Recovering"), 0)

    for name, (mean, deviation) in delays.items():
        found = days[name]
        n = len(found)
        m, s, fourth = expected_wait(mean, deviation)
        found_mean = sum(found) / n
        found_deviation = math.sqrt(sum((v - found_mean) ** 2 for v in found) / n)
        report.hold(f"{name}: mean of {n} waits", found_mean, m, s / math.sqrt(n),
                    slack=0.005)
        if s > 0:
            error = math.sqrt(max(fourth - s ** 4, 0) / (4 * n * s * s))
            report.hold(f"{name}: deviation of {n} waits", found_deviation, s,
                        error, slack=0.005)
        if deviation == 0:
            report.hold(f"{name}: shortest", min(found), math.floor(mean), 0)
            report.hold(f"{name}: longest", max(found), math.floor(mean) + 1, 0)
        print(f"  {name}: {n} waits, mean {found_mean:.3f} ({m:.3f}), "
              f"deviation {found_deviation:.3f} ({s:.3f})")

    print(f"{report.held} numbers held against the tables, {report.failed} off")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
