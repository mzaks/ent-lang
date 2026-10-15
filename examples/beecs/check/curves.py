#!/usr/bin/env python3
"""Holds the ent-lang model's days against beecs's, over many seeds.

  examples/beecs/reference/fetch.sh        once: beecs, built, and its weather
  python3 examples/beecs/check/curves.py [scenario] [--seeds N] [--claims]

Both are run for seeds 1 to N (100 by default) with the same parameters
(the scenario's, written as beecs's JSON for the reference and as a
parameters.txt for the model). The two cannot give the same numbers for a
seed, since they draw theirs differently; what is held is that they give
the same spread:

- for every column, the share of days on which the model's median lies
  within the reference's 5% to 95% of that day (at least 95% of the days
  where the reference's spread is not nothing, all where it is);
- as many colonies lost (a Fisher exact test at 1%, where OnExtinction ends
  a run);
- for every column, the year's mean of each run, and its peak, spread
  alike over the seeds (a two-sample Kolmogorov-Smirnov test at 1%).

Scenarios: default (beecs's defaults, a year), small (squadrons of 10
bees, ten times as many), years (three years of Berlin's weather, 2000 to
2002), seasonal (two patches whose flowers follow the season, a year),
scarce (two small patches that run out within a day, squadrons of 10),
lean (a smaller, poorer colony, of which some are lost, with
OnExtinction; two years).

With --claims the model forages without turns (claims.ent), held against
the same beecs.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from scipy import stats

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BUILD = os.path.join(ROOT, "build", "beecs")
REFERENCE = os.path.join(BUILD, "ref_beecs")
WEATHER = os.path.join(BUILD, "run", "foraging-period")
PROGRAM = os.path.join(ROOT, "examples", "beecs", "beecs.ent")

COLUMNS = ["w_eggs", "w_larvae", "w_pupae", "w_in_hive", "foragers",
           "d_eggs", "d_larvae", "d_pupae", "d_in_hive",
           "honey_kg", "pollen_g", "aff", "rounds"]

SCENARIOS = {
    "default": {},
    "small": {
        "Foragers.SquadronSize": 10,
    },
    "years": {
        "Termination.MaxTicks": 3 * 365,
        "ForagingPeriod.Files": ["foraging-period/berlin2000.txt",
                                 "foraging-period/berlin2001.txt",
                                 "foraging-period/berlin2002.txt"],
    },
    "seasonal": {
        "Patches": [
            {"DistToColony": 800, "SeasonalPatch": {
                "SeasonShift": 20, "MaxNectar": 30, "MaxPollen": 2,
                "NectarConcentration": 1.5, "DetectionProbability": 0.2}},
            {"DistToColony": 2000, "SeasonalPatch": {
                "SeasonShift": -30, "MaxNectar": 60, "MaxPollen": 1,
                "NectarConcentration": 1.0, "DetectionProbability": 0.15}},
        ],
    },
    "scarce": {
        "Foragers.SquadronSize": 10,
        "Patches": [
            {"DistToColony": 400, "ConstantPatch": {
                "Nectar": 2, "Pollen": 0.2, "NectarConcentration": 1.5,
                "DetectionProbability": 0.5}},
            {"DistToColony": 900, "ConstantPatch": {
                "Nectar": 1, "Pollen": 0.1, "NectarConcentration": 1.2,
                "DetectionProbability": 0.3}},
        ],
    },
    "lean": {
        "Termination.MaxTicks": 2 * 365,
        "Termination.OnExtinction": True,
        "InitialPopulation.Count": 3000,
        "InitialStores.Honey": 3,
        "ForagingPeriod.Files": ["foraging-period/berlin2003.txt",
                                 "foraging-period/berlin2004.txt"],
        "Patches": [
            {"DistToColony": 2500, "ConstantPatch": {
                "Nectar": 5, "Pollen": 0.3, "NectarConcentration": 1.0,
                "DetectionProbability": 0.1}},
        ],
    },
}


def beecs_json(parameters):
    """The scenario as beecs's JSON: groups of fields."""
    groups = {}
    for name, value in parameters.items():
        if name == "Patches":
            groups["InitialPatches"] = {"Patches": value}
            continue
        group, field = name.split(".")
        groups.setdefault(group, {})[field] = value
    return groups


def model_text(parameters, seed, claims):
    """The scenario as the model's parameters.txt."""
    lines = [f"RandomSeed.Seed {seed}"]
    if claims:
        lines.append("Variant.Claims true")
    for name, value in parameters.items():
        if name == "Patches":
            for patch in value:
                if "ConstantPatch" in patch:
                    c = patch["ConstantPatch"]
                    lines.append(f"Patch.Constant {patch['DistToColony']} {c['Nectar']} "
                                 f"{c['Pollen']} {c['NectarConcentration']} "
                                 f"{c['DetectionProbability']}")
                else:
                    s = patch["SeasonalPatch"]
                    lines.append(f"Patch.Seasonal {patch['DistToColony']} {s['SeasonShift']} "
                                 f"{s['MaxNectar']} {s['MaxPollen']} "
                                 f"{s['NectarConcentration']} {s['DetectionProbability']}")
        elif isinstance(value, list):
            lines.append(f"{name} {' '.join(value)}")
        elif isinstance(value, bool):
            lines.append(f"{name} {'true' if value else 'false'}")
        else:
            lines.append(f"{name} {value}")
    return "\n".join(lines) + "\n"


def parse(text):
    """seed -> array of days x columns."""
    runs = {}
    header = None
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        cells = line.split(",")
        if cells[0] == "seed":
            header = cells
            continue
        row = dict(zip(header, cells))
        runs.setdefault(int(row["seed"]), []).append(
            [float(row[c]) for c in COLUMNS])
    return {seed: np.array(rows) for seed, rows in runs.items()}


def run_reference(parameters, seeds, work):
    path = os.path.join(work, "reference.json")
    with open(path, "w") as f:
        json.dump(beecs_json(parameters), f)
    out = subprocess.run([REFERENCE, "-params", path, "-from", "1", "-to", str(seeds)],
                         cwd=os.path.dirname(WEATHER), capture_output=True, text=True,
                         check=True)
    return parse(out.stdout)


def run_model(parameters, seeds, work, claims):
    program = os.path.join(work, "model")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", PROGRAM,
                    "-o", program], check=True, env=dict(os.environ,
                    ENT_CFLAGS=os.environ.get("ENT_CFLAGS", "-march=native")))

    def one(seed):
        place = os.path.join(work, f"seed{seed}")
        os.makedirs(place, exist_ok=True)
        link = os.path.join(place, "foraging-period")
        if not os.path.exists(link):
            os.symlink(WEATHER, link)
        with open(os.path.join(place, "parameters.txt"), "w") as f:
            f.write(model_text(parameters, seed, claims))
        out = subprocess.run([program], cwd=place, capture_output=True, text=True,
                             check=True)
        return parse(out.stdout)

    runs = {}
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        for result in pool.map(one, range(1, seeds + 1)):
            runs.update(result)
    return runs


def stack(runs, days):
    """seeds x days x columns, a run that ended early (extinct) held at its
    last day."""
    out = np.zeros((len(runs), days, len(COLUMNS)))
    for i, seed in enumerate(sorted(runs)):
        rows = runs[seed]
        out[i, :len(rows)] = rows
        out[i, len(rows):] = rows[-1]
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", nargs="?", default="default", choices=SCENARIOS)
    parser.add_argument("--seeds", type=int, default=100)
    parser.add_argument("--claims", action="store_true")
    args = parser.parse_args()
    if not os.path.exists(REFERENCE):
        sys.exit("no reference: run examples/beecs/reference/fetch.sh first")
    parameters = SCENARIOS[args.scenario]

    with tempfile.TemporaryDirectory(dir=BUILD) as work:
        reference = run_reference(parameters, args.seeds, work)
        model = run_model(parameters, args.seeds, work, args.claims)
    days = max(max(len(r) for r in reference.values()), max(len(r) for r in model.values()))
    ref = stack(reference, days)
    mod = stack(model, days)
    ref_days = [len(r) for r in reference.values()]
    mod_days = [len(r) for r in model.values()]

    print(f"scenario {args.scenario}{', without turns' if args.claims else ''}, "
          f"{args.seeds} seeds each, {days} days")
    lost_ref = sum(1 for d in ref_days if d < days)
    lost_mod = sum(1 for d in mod_days if d < days)
    if lost_ref + lost_mod > 0:
        print(f"colonies lost before the end: reference {lost_ref}, model {lost_mod}")
    lost = stats.fisher_exact([[lost_ref, len(ref_days) - lost_ref],
                               [lost_mod, len(mod_days) - lost_mod]]).pvalue
    print(f"{'column':10} {'in band':>8} {'mean ref':>10} {'mean model':>10} "
          f"{'KS mean':>8} {'KS peak':>8}")
    failed = [] if lost >= 0.01 else ["colonies lost"]
    for c, name in enumerate(COLUMNS):
        low = np.percentile(ref[:, :, c], 5, axis=0)
        high = np.percentile(ref[:, :, c], 95, axis=0)
        median = np.median(mod[:, :, c], axis=0)
        inside = (median >= low - 1e-9) & (median <= high + 1e-9)
        spread = high > low
        share = inside[spread].mean() if spread.any() else 1.0
        flat_ok = inside[~spread].all() if (~spread).any() else True
        ref_mean = ref[:, :, c].mean(axis=1)
        mod_mean = mod[:, :, c].mean(axis=1)
        ks_mean = stats.ks_2samp(ref_mean, mod_mean).pvalue
        ks_peak = stats.ks_2samp(ref[:, :, c].max(axis=1), mod[:, :, c].max(axis=1)).pvalue
        good = share >= 0.95 and flat_ok and ks_mean >= 0.01 and ks_peak >= 0.01
        if not good:
            failed.append(name)
        print(f"{name:10} {share:8.1%} {ref_mean.mean():10.2f} {mod_mean.mean():10.2f} "
              f"{ks_mean:8.3f} {ks_peak:8.3f} {'' if good else '  <-- differs'}")
    if failed:
        print("differs: " + ", ".join(failed))
        sys.exit(1)
    print("agrees")


if __name__ == "__main__":
    main()
