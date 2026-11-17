"""The dish's statistics against the cultures', seed by seed: what the
README's tables are.

    validate.py culture_stats.json evoked_stats.json [seeds [name=value ...]]

Each seed's dish (the library's defaults, or these): 10 minutes of its
time to settle (its weights go where STDP and scaling take them), 10
minutes of spontaneous activity, of which stats.py's statistics, then the
archive's stim protocol, 10 rounds of the 64 channels (evoked.py). Then
the median of the seeds, the cultures' quartiles, and each statistic's
place: `in` the cultures' middle half, or `low`/`high` of it.

    validate.py culture_stats.json evoked_stats.json --long [seeds [name=value ...]]

Does it keep so? 45 minutes of each seed (no stims), the statistics of
minutes 0 to 10, 15 to 25 and 35 to 45, their medians over the seeds.
"""

from __future__ import annotations

import json
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import calibrate  # noqa: E402
import evoked  # noqa: E402
import stats  # noqa: E402

KEYS = calibrate.SCORED + calibrate.EVOKED
WINDOWS = [(0, 10), (15, 25), (35, 45)]
PARAMETERS: dict = {}


def record(dish, minutes: float):
    t, ch = [], []
    for _ in range(0, int(minutes * 60_000), 10_000):
        for step, channel in dish.step(10_000):
            t.append(step)
            ch.append(channel)
    return np.array(t) / 1000.0, np.array(ch, dtype=int)


def joint(seed: int) -> dict:
    import ent_dish
    dish = ent_dish.Dish(seed=seed, **PARAMETERS)
    t, ch = record(dish, 20)
    keep = t >= 600
    out = stats.stats(t[keep] - 600, ch[keep], duration=600)
    out.update(evoked.evoked(*evoked.run_protocol(dish, rounds=10), channels=64))
    dish.close()
    return out


def long(seed: int) -> list[dict]:
    import ent_dish
    dish = ent_dish.Dish(seed=seed, **PARAMETERS)
    t, ch = record(dish, WINDOWS[-1][1])
    dish.close()
    out = []
    for a, b in WINDOWS:
        keep = (t >= a * 60) & (t < b * 60)
        out.append(stats.stats(t[keep] - a * 60, ch[keep], duration=(b - a) * 60))
    return out


def row(name: str, s: dict, keys) -> str:
    return f"{name:>12}" + "".join(f"{s.get(k, float('nan')):>10.3g}" for k in keys)


def place(value: float, quartiles) -> str:
    lo, _, hi = quartiles
    return "low" if value < lo else "high" if value > hi else "in"


def init(parameters: dict) -> None:
    PARAMETERS.update(parameters)


if __name__ == "__main__":
    culture = json.load(open(sys.argv[1]))
    culture.update(json.load(open(sys.argv[2])))
    rest = sys.argv[3:]
    is_long = bool(rest) and rest[0] == "--long"
    if is_long:
        rest = rest[1:]
    seeds = int(rest[0]) if rest and "=" not in rest[0] else 10
    parameters = {k: float(v) for k, v in (a.split("=") for a in rest if "=" in a)}
    if parameters:
        print(json.dumps(parameters))
    with Pool(min(seeds, 22), initializer=init, initargs=(parameters,)) as pool:
        results = pool.map(long if is_long else joint, range(1, seeds + 1))

    def score(s: dict) -> float:
        return calibrate.score(s, culture) + (0 if is_long else calibrate.score(s, culture, calibrate.EVOKED))

    keys = calibrate.SCORED if is_long else KEYS
    print(" " * 12 + "".join(f"{k[:9]:>10}" for k in keys) + "     score")
    windows = WINDOWS if is_long else [None]
    for w, window in enumerate(windows):
        rows = [r[w] for r in results] if is_long else results
        label = f" {window[0]}-{window[1]}" if window else ""
        for seed, s in enumerate(rows, 1):
            print(row(f"s{seed}{label}", s, keys) + f"{score(s):>10.2f}")
        median = {k: float(np.nanmedian([s.get(k, np.nan) for s in rows])) for k in keys}
        print(row(f"median{label}", median, keys) + f"{score(median):>10.2f}")
        print(" " * 12 + "".join(f"{place(median[k], culture[k]):>10}" for k in keys))
    for i, name in ((0, "cultures 25%"), (1, "median"), (2, "75%")):
        print(row(name, {k: culture[k][i] for k in keys}, keys))
