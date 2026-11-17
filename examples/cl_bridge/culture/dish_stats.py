"""The statistics of stats.py for the dish: `minutes` of spontaneous
activity (no stims) of each seed, through the library (ent_dish.Dish).

    dish_stats.py [minutes [seeds [name=value ...]]]
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import ent_dish  # noqa: E402
import stats  # noqa: E402


def record(minutes: float, **parameters) -> tuple[np.ndarray, np.ndarray]:
    dish = ent_dish.Dish(**parameters)
    dish.step(1000)                      # a second to settle, not counted
    steps = int(minutes * 60_000)
    t, ch = [], []
    for _ in range(0, steps, 10_000):
        for step, channel in dish.step(10_000):
            t.append(step)
            ch.append(channel)
    dish.close()
    return (np.array(t) - 1000) / 1000.0, np.array(ch, dtype=int)


def parse(args: list[str]) -> dict:
    out = {}
    for a in args:
        name, value = a.split("=")
        out[name] = float(value)
    return out


if __name__ == "__main__":
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 10
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    parameters = parse(sys.argv[3:])
    rows = []
    print(stats.header())
    for seed in range(1, seeds + 1):
        s = stats.stats(*record(minutes, seed=seed, **parameters), duration=minutes * 60)
        rows.append(s)
        print(stats.line(f"seed {seed}", s))
    if len(rows) > 1:
        print(stats.line("median", {k: float(np.nanmedian([r[k] for r in rows])) for k in stats.COLUMNS}))
