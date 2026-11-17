"""Does the dish learn Pong? pong.py's conditions on seeds 1 to `seeds`,
each a session of `minutes`, a process each, as DishBrain compared them:
hits a rally, aces and long rallies, in the first 5 minutes and after.

    build/cl_bridge/venv/bin/python examples/cl_bridge/pong_study.py \\
        [minutes [seeds [out.json]]]

Sessions: stim, silent, none and rest with the dish's defaults (STDP on),
and stim with STDP off (`stim/off`), which can only do what the dish's
wiring does at the start: what stim does more than stim/off is learning;
and stim/off with the sensory map upside down (`stim/off/flip`): what
stim/off does more than it is the wiring following the ball, not stims
moving the paddle whatever they say; and `still`, the game alone with the
paddle in the middle all along (no dish), for chance.
For each, the mean over seeds (and its standard error); then, seed by
seed (the same dish in each), the differences that say whether it
learned: late less early within stim and stim/off, and stim less
stim/off, stim less rest, late.
"""

from __future__ import annotations

import json
import math
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

SESSIONS = [("stim", {}), ("silent", {}), ("none", {}), ("rest", {}),
            ("stim/off", {"stdp": 0}), ("stim/off/flip", {"stdp": 0})]


def run(job):
    minutes, seed, name, parameters = job
    os.environ["OMP_NUM_THREADS"] = "1"
    import pong
    out = pong.play(minutes, seed, name.split("/")[0], name.endswith("/flip"), **parameters)
    out["session"] = name
    return out


def still(minutes: float, seed: int) -> dict:
    """The game with no dish: no spikes, the paddle in the middle all along
    (and no feedback, no pause, as `none`)."""
    import pong
    game = pong.Pong("none", seed)
    for _ in range(int(minutes * 60 * pong.TICKS)):
        game.tick(0, 0)
    return {**pong.summary(game.rallies), "paddle_top": 0.0, "paddle_bottom": 0.0,
            "session": "still", "seed": seed}


def mean_se(xs):
    xs = [x for x in xs if x == x]
    if not xs:
        return float("nan"), float("nan")
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, float("nan")
    v = sum((x - m) ** 2 for x in xs) / (len(xs) - 1)
    return m, math.sqrt(v / len(xs))


def get(r, part, key):
    return r[part].get(key, float("nan"))


def main() -> int:
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    jobs = [(minutes, seed, name, p) for name, p in SESSIONS for seed in range(1, seeds + 1)]
    with ProcessPoolExecutor(min(len(jobs), 22)) as pool:
        results = list(pool.map(run, jobs))
    if len(sys.argv) > 3:
        with open(sys.argv[3], "w") as f:
            json.dump(results, f)
    results += [still(minutes, seed) for seed in range(1, seeds + 1)]
    by = {(r["session"], r["seed"]): r for r in results}

    def cell(xs):
        m, se = mean_se(xs)
        return f"{m:6.2f} ±{se:4.2f}"

    print(f"{minutes:g} minutes a session, seeds 1 to {seeds}; mean over seeds ± standard error")
    print(f"{'session':>13} {'rallies':>8} {'hits early':>13} {'hits late':>13} "
          f"{'aces late':>13} {'long late':>13} {'paddle at a wall':>17}")
    for name, _ in SESSIONS + [("still", {})]:
        rs = [by[(name, s)] for s in range(1, seeds + 1)]
        print(f"{name:>13} {sum(r['all']['rallies'] for r in rs) / seeds:8.1f} "
              f"{cell([get(r, 'early', 'hits') for r in rs]):>13} "
              f"{cell([get(r, 'late', 'hits') for r in rs]):>13} "
              f"{cell([get(r, 'late', 'aces') for r in rs]):>13} "
              f"{cell([get(r, 'late', 'long') for r in rs]):>13} "
              f"{cell([r['paddle_top'] + r['paddle_bottom'] for r in rs]):>17}")
    print("differences, seed by seed (hits a rally):")
    pairs = [("stim: late less early", lambda s: get(by[("stim", s)], "late", "hits") - get(by[("stim", s)], "early", "hits")),
             ("stim/off: late less early", lambda s: get(by[("stim/off", s)], "late", "hits") - get(by[("stim/off", s)], "early", "hits")),
             ("stim less stim/off, late", lambda s: get(by[("stim", s)], "late", "hits") - get(by[("stim/off", s)], "late", "hits")),
             ("stim less rest, late", lambda s: get(by[("stim", s)], "late", "hits") - get(by[("rest", s)], "late", "hits")),
             ("stim/off less rest, late", lambda s: get(by[("stim/off", s)], "late", "hits") - get(by[("rest", s)], "late", "hits")),
             ("stim/off less flipped, late", lambda s: get(by[("stim/off", s)], "late", "hits") - get(by[("stim/off/flip", s)], "late", "hits"))]
    for label, f in pairs:
        xs = [f(s) for s in range(1, seeds + 1)]
        m, se = mean_se(xs)
        print(f"  {label:>28}: {m:+.3f} ± {se:.3f} ({m / se if se else 0:+.1f} se)  "
              + " ".join(f"{x:+.2f}" for x in xs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
