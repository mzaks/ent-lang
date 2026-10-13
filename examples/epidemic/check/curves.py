#!/usr/bin/env python3
"""The epidemic's curves against the reference's (the spec's third check,
and with a lockdown its fourth).

  examples/epidemic/reference/fetch.sh      once
  examples/epidemic/check/curves.py [lockdown] [seeds [first seed]]

Builds the reference (its C, with the machine's GSL) and the model, runs
both for 20 seeds with 100,000 people from the same parameter file (the
reference's baseline; with `lockdown`, 500 infected at the start and a
lockdown from day 20 to day 60), and
holds the model's runs against the reference's own spread from seed to
seed, since the two cannot draw the same numbers:

- for each day, the median of the model's new infections is to lie
  between the reference's 5th and 95th percentiles of that day;
- the day of the peak, its height and how many were infected in all are
  to be spread as the reference's are (a Kolmogorov-Smirnov test of the
  two samples, at 5%).

Says what fails. The reference's runs are kept (build/epidemic/run) and
not made again. Needs the compiler built (build/bin) and its LLVM
(LLVM_PREFIX, as for tools/ent).
"""

import concurrent.futures
import csv
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(ROOT, "build", "epidemic")
RUN = os.path.join(OUT, "run")
PEOPLE = 100000
SCENARIOS = {
    "baseline": {},
    # (With 500 infected at the start, every run is as far along when the
    # lockdown begins: what is held is what the lockdown does, not when an
    # epidemic of ten takes off.)
    "lockdown": {"n_seed_infection": "500", "lockdown_time_on": "20",
                 "lockdown_time_off": "60"},
    # To find where the two differ: without one of the networks.
    "no-chance": {"relative_transmission_random": "0"},
    "no-work": {"relative_transmission_occupation": "0"},
    "no-home": {"relative_transmission_household": "0"},
    "chance-only": {"relative_transmission_household": "0",
                    "relative_transmission_occupation": "0"},
    "slow": {"infectious_rate": "3.5"},
}


def build_reference():
    program = os.path.join(OUT, "ref_model")
    if os.path.isfile(program):
        return program
    source = os.path.join(OUT, "reference", "src")
    if not os.path.isdir(source):
        sys.exit("no reference: run examples/epidemic/reference/fetch.sh")
    files = [f for f in sorted(os.listdir(source))
             if f.endswith(".c") and f != "R_C_interface.c" and "_wrap" not in f]
    subprocess.run(["cc", "-O3", "-w", "-o", program] + files +
                   ["-lgsl", "-lgslcblas", "-lm"], check=True, cwd=source)
    return program


def build_model():
    program = os.path.join(RUN, "epidemic")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "--parallel",
         "-I", os.path.join(HERE, ".."), os.path.join(HERE, "..", "epidemic.ent"),
         "-o", program], check=True, env=environment)
    return program


def parameters(scenario, seed):
    """The reference's baseline with this seed and what the scenario
    changes: the two lines of a parameter file."""
    with open(os.path.join(OUT, "reference", "tests", "data",
                           "baseline_parameters.csv")) as file:
        names, values = list(csv.reader(file))[:2]
    names = [name.strip() for name in names]
    values = [value.strip() for value in values]
    changes = {"rng_seed": str(seed), "n_total": str(PEOPLE)}
    changes.update(SCENARIOS[scenario])
    for name, value in changes.items():
        values[names.index(name)] = value
    return names, values


def series(text):
    """Day by day, everybody infected so far and the dead."""
    lines = [line for line in text.split("\n") if line and not line.startswith("#")]
    rows = list(csv.DictReader(lines))
    days = {int(row["time"]): (int(row["total_infected"]), int(row["n_death"]))
            for row in rows}
    return days


def run_reference(program, scenario, seed):
    kept = os.path.join(RUN, f"reference_{scenario}_{seed}.csv")
    if not os.path.isfile(kept):
        folder = os.path.join(RUN, f"reference_{scenario}_{seed}")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "parameters.csv"), "w", newline="") as file:
            csv.writer(file).writerows(parameters(scenario, seed))
        out = subprocess.run(
            [program, "parameters.csv", "1", ".",
             os.path.join(RUN, "baseline_household_demographics.csv")],
            check=True, capture_output=True, text=True, cwd=folder).stdout
        with open(kept, "w") as file:
            file.write(out)
        shutil.rmtree(folder)
    return series(open(kept).read())


def run_model(program, scenario, seed):
    folder = os.path.join(RUN, f"model_{scenario}_{seed}")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "parameters.csv"), "w", newline="") as file:
        csv.writer(file).writerows(parameters(scenario, seed))
    shutil.copy(os.path.join(RUN, "baseline_household_demographics.csv"), folder)
    environment = dict(os.environ, OMP_NUM_THREADS="2")
    out = subprocess.run([program], check=True, capture_output=True, text=True,
                         cwd=folder, env=environment).stdout
    shutil.rmtree(folder)
    return series(out)


def daily(days, last, seeded):
    """New infections of each day from 1 to `last` (a run that ended
    before has none after), and the run's own numbers. `seeded` were
    infected before day 1."""
    total = lambda day: days[min(day, max(days))][0] if day >= min(days) else 0
    start = seeded
    new = [total(1) - start] + [total(day) - total(day - 1) for day in range(2, last + 1)]
    peak = max(range(len(new)), key=lambda i: new[i])

    def reached(count):
        """The day, with its part, on which so many were infected."""
        for day in range(1, last + 1):
            if total(day) >= count:
                before = total(day - 1)
                return day - 1 + (count - before) / max(1, total(day) - before)
        return float(last)
    return new, {"days from 1000 infected to 10000":
                 round(10 * (reached(10000) - reached(1000))) / 10,
                 "peak day": peak + 1, "peak height": new[peak],
                 "infected in all": total(last),
                 "dead": days[max(days)][1]}


def percentile(values, part):
    ordered = sorted(values)
    at = part * (len(ordered) - 1)
    low = math.floor(at)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (at - low)


def kolmogorov_smirnov(a, b):
    """The most the two samples' shares below a value differ, and what
    that may be at 5% for samples so big."""
    most = 0.0
    for value in sorted(set(a) | set(b)):
        most = max(most, abs(sum(1 for x in a if x <= value) / len(a) -
                             sum(1 for x in b if x <= value) / len(b)))
    return most, 1.358 * math.sqrt((len(a) + len(b)) / (len(a) * len(b)))


def main():
    arguments = sys.argv[1:]
    named = [a for a in arguments if a in SCENARIOS]
    scenario = named[0] if named else "baseline"
    numbers = [int(a) for a in arguments if a.isdigit()]
    first = numbers[1] if len(numbers) > 1 else 1
    seeds = range(first, first + (numbers[0] if numbers else 20))
    reference_program = build_reference()
    model_program = build_model()
    workers = max(1, (os.cpu_count() or 2) // 2)
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        references = list(pool.map(lambda s: run_reference(reference_program, scenario, s), seeds))
        models = list(pool.map(lambda s: run_model(model_program, scenario, s), seeds))
    last = 200
    seeded = int(SCENARIOS[scenario].get("n_seed_infection", 10))
    reference_runs = [daily(days, last, seeded) for days in references]
    model_runs = [daily(days, last, seeded) for days in models]

    failed = 0
    outside = []
    for day in range(last):
        low = percentile([new[day] for new, _ in reference_runs], 0.05)
        high = percentile([new[day] for new, _ in reference_runs], 0.95)
        median = percentile([new[day] for new, _ in model_runs], 0.5)
        if median < low or median > high:
            outside.append((day + 1, median, low, high))
    print(f"{scenario}, {len(seeds)} seeds of each, {PEOPLE} people")
    print(f"days on which the model's median of new infections is outside the "
          f"reference's 5% to 95%: {len(outside)} of {last}")
    for day, median, low, high in outside[:10]:
        print(f"  day {day}: {median:.0f}, reference {low:.0f} to {high:.0f}")
    failed += 1 if outside else 0
    for what in ("days from 1000 infected to 10000", "peak day", "peak height",
                 "infected in all", "dead"):
        a = [numbers[what] for _, numbers in reference_runs]
        b = [numbers[what] for _, numbers in model_runs]
        distance, allowed = kolmogorov_smirnov(a, b)
        fits = distance <= allowed
        # (The dead are not among what the spec holds; they are said.)
        if not fits and what in ("peak day", "peak height", "infected in all"):
            failed += 1
        print(f"{what}: reference median {percentile(a, 0.5):.1f} "
              f"({min(a)} to {max(a)}), model {percentile(b, 0.5):.1f} "
              f"({min(b)} to {max(b)}); apart {distance:.2f}, "
              f"{'within' if fits else 'MORE THAN'} {allowed:.2f}")
    print("the curves agree" if not failed else f"{failed} of 4 do not agree")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
