"""A random search for the dish's parameters that make its spontaneous
activity and its answers to stims look like the cultures' (stats.py and
evoked.py on Wagenaar, Pine and Potter's recordings at DIV 25 to 31).

Each candidate: a dish of seed 1 with STDP and synaptic scaling on, run
`warmup` minutes (so the weights settle where plasticity takes them) and
then `minutes` more, of which the statistics of spontaneous activity are
taken; then, the same dish, the archive's stim protocol (evoked.py) for
5 rounds of its 64 channels, of which those of the answers. Each against
the cultures' medians: for each statistic the difference of the logs
over the cultures' spread (the log of their 75th percentile over their
25th), squared and summed. Candidates are drawn log-uniformly from
RANGES, or around the parameters of a start file (a calibration.json of
an earlier search: its best, with any parameter it lacks from START),
then, in later rounds, around the best so far, in processes of their own.

    calibrate.py culture_stats.json evoked_stats.json [rounds [per_round [minutes [warmup [start.json]]]]]
"""

from __future__ import annotations

import json
import math
import random
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

FIXED = {"stdp": 1, "hear_reach": 0.03, "a_plus": 0.002, "a_minus": 0.002}
# (The weights start at W_START of w_max: where plasticity takes them does
# not depend on it, but starting higher sets off a burst at the start.)
W_START = 0.32
RANGES = {
    "w_max": (0.004, 1.0),
    "w_inh": (0.005, 1.5),
    "depress": (0.05, 0.6),
    "tau_recover": (150.0, 3000.0),
    "noise_rate": (0.001, 0.03),
    "noise_amp": (0.2, 1.0),
    "adapt_inc": (0.005, 0.3),
    "tau_adapt": (50.0, 2000.0),
    "tau_syn": (1.0, 300.0),
    "home_rate": (0.3, 3.0),
    "home_gain": (0.02, 2.0),
    "axon": (0.05, 1.0),
    "conduct": (0.005, 0.2),
    "stim_gain": (1.0, 100.0),
    "stim_reach": (0.01, 0.2),
    "fast": (1.0, 300.0),
}
START = {"axon": 0.3, "conduct": 0.02, "stim_gain": 10.0, "stim_reach": 0.05, "fast": 30.0}
SCORED = ["rate_hz", "isi_cv", "bursts_per_min", "burst_ms", "in_bursts",
          "recruited", "ibi_cv"]
EVOKED = ["early", "late", "channels", "big", "latency"]


def score(dish: dict, culture: dict, keys=SCORED) -> float:
    total = 0.0
    for k in keys:
        lo, mid, hi = culture[k]
        spread = max(math.log(hi / lo), 0.1)
        value = dish.get(k, 0.0)
        if not value or value != value:          # none, or nan
            total += 9.0
            continue
        total += (math.log(max(value, 1e-3) / mid) / spread) ** 2
    return total


def run(job):
    params, minutes, warmup = job
    import ent_dish
    import evoked
    import numpy as np
    import stats
    dish = ent_dish.Dish(seed=1, **FIXED, w_exc=W_START * params["w_max"], **params)
    t, ch = [], []
    for _ in range(0, int((minutes + warmup) * 60_000), 10_000):
        for step, channel in dish.step(10_000):
            t.append(step)
            ch.append(channel)
    t, ch = np.array(t) / 1000.0, np.array(ch, dtype=int)
    keep = t >= warmup * 60
    t, ch = t[keep] - warmup * 60, ch[keep]
    if len(t) < 10:
        dish.close()
        return params, {}
    out = stats.stats(t, ch, duration=minutes * 60)
    out.update(evoked.evoked(*evoked.run_protocol(dish, rounds=5), channels=64))
    dish.close()
    return params, out


def draw(rng, around=None, width=1.0):
    out = {}
    for k, (lo, hi) in RANGES.items():
        a, b = math.log(lo), math.log(hi)
        if around:
            c = math.log(around[k])
            a, b = max(a, c - width), min(b, c + width)
        out[k] = round(math.exp(rng.uniform(a, b)), 5)
    return out


if __name__ == "__main__":
    culture = json.load(open(sys.argv[1]))
    culture.update(json.load(open(sys.argv[2])))
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    per_round = int(sys.argv[4]) if len(sys.argv) > 4 else 66
    minutes = float(sys.argv[5]) if len(sys.argv) > 5 else 3
    warmup = float(sys.argv[6]) if len(sys.argv) > 6 else 0
    start = None
    if len(sys.argv) > 7:
        start = dict(json.load(open(sys.argv[7]))[0]["params"])
        if "w_exc" in start and "w_max" not in start:
            start["w_max"] = 2 * start.pop("w_exc")
        for k, (lo, hi) in RANGES.items():
            start.setdefault(k, START.get(k, math.sqrt(lo * hi)))
    rng = random.Random(7)
    tried = []
    with ProcessPoolExecutor(22) as pool:
        for r in range(rounds):
            best = sorted(tried, key=lambda x: x[0])[:5]
            width = 1.0 / (r + 1)
            centre = lambda i: best[i % len(best)][1] if best and r else start
            jobs = [(draw(rng, centre(i), width if r or not start else 0.5), minutes, warmup)
                    for i in range(per_round)]
            for params, s in pool.map(run, jobs):
                tried.append((score(s, culture) + score(s, culture, EVOKED) if s else 999.0,
                              params, s))
            tried.sort(key=lambda x: x[0])
            print(f"round {r + 1}: best {tried[0][0]:.2f}", flush=True)
            for sc, params, s in tried[:3]:
                print(f"  {sc:6.2f} {json.dumps(params)}")
                print("        " + " ".join(f"{k}={s.get(k, 0):.3g}" for k in SCORED + EVOKED),
                      flush=True)
    json.dump([{"score": sc, "params": p, "stats": s} for sc, p, s in tried],
              open("calibration.json", "w"), indent=1)
