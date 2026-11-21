"""Does the trained dish get worse again, or only its games? The dish of
dish_pong.py (game.c, the reward for each move) trained to a minute, then
frozen (STDP off: no reward-modulated change and no scaling) and played on
the same serves at every minute: the k-th rally of the evaluation starts
the same whatever the paddle did before (game.c's pong_fixed_serves). So
what differs between two minutes' scores is the wiring learnt by then.

    build/cl_bridge/venv/bin/python examples/cl_bridge/frozen_eval.py \\
        [seeds (8)] [minutes (20)] [rallies (60)] [out.json]

A minute's dish is made again by replaying its seed to that minute (the
dish and the game are deterministic). Also the gain 0 dish (no reward;
scaling only) at a few minutes, and each seed's live hits a rally minute
by minute (learning on, its own serves) for comparison.

    ... frozen_eval.py retest out.json [rallies (60)]

Each seed's best minute in out.json and its last minute played again on a
second set of serves: is the best minute better wiring, or did it suit
the first serves? (21 November 2026, 8 seeds: on the new serves the best
minute beat minute 20 for 7 of 8 seeds, 1.42 against 1.11 hits a rally,
against 1.40 and 0.97 on the serves it was picked on.)
"""
from __future__ import annotations

import ctypes
import json
import statistics
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

TICKS = 200
CHUNK = 4
EVAL_SEED = 20261121
SERVES = 0x5EED5EED


def _lib():
    import ent_dish
    lib = ctypes.CDLL(str(ent_dish.build_library()))
    lib.pong_make_dish.restype = ctypes.c_void_p
    lib.pong_make_dish.argtypes = [ctypes.c_int64, ctypes.c_double, ctypes.c_int32]
    lib.pong_create.restype = ctypes.c_void_p
    lib.pong_create.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int32, ctypes.c_int32]
    lib.pong_run.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p]
    lib.pong_state.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.pong_fixed_serves.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.pong_destroy.argtypes = [ctypes.c_void_p]
    lib.dish_set_stdp.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    return lib


def job(seed: int, gain: float, minute: int, rallies: int, live: bool,
        serves: int = SERVES, eval_seed: int = EVAL_SEED) -> dict:
    lib = _lib()
    state = (ctypes.c_double * 12)()
    dish = lib.pong_make_dish(seed, gain, 0)
    game = lib.pong_create(dish, seed, 0, 0)
    per_minute = []
    for m in range(minute):
        before = list(state)
        if lib.pong_run(game, 60 * TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(game, state)
        if live:
            r = state[4] - (before[4] if m else 0)
            h = state[7] - (before[7] if m else 0)
            per_minute.append(h / r if r else 0.0)
    lib.pong_destroy(game)
    # Frozen: no weight changes from here on, the same serves for every minute.
    lib.dish_set_stdp(dish, 0)
    test = lib.pong_create(dish, eval_seed, 0, 0)
    lib.pong_fixed_serves(test, serves)
    hits_at, t0 = [], None
    while True:
        if lib.pong_run(test, CHUNK, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(test, state)
        if t0 is None:
            t0 = state[0]
        while len(hits_at) < int(state[4]):
            hits_at.append(state[7])
        if len(hits_at) >= rallies or state[0] - t0 > 900:
            break
    n = min(len(hits_at), rallies)
    score = hits_at[n - 1] / n if n else 0.0
    return {"seed": seed, "gain": gain, "minute": minute, "frozen": score,
            "rallies": n, "live": per_minute}


def retest(path: str, rallies: int) -> int:
    first = json.loads(Path(path).read_text())
    rows = {}
    for r in first:
        if r["gain"] == 1.0:
            rows.setdefault(r["seed"], {})[r["minute"]] = r["frozen"]
    jobs = []
    for s, row in sorted(rows.items()):
        last = max(row)
        for m in sorted({max(row, key=row.get), last}):
            jobs.append((s, 1.0, m, rallies, False, 0x0BADC0DE, 777))
    _lib()
    with ProcessPoolExecutor(10) as pool:
        again = {(r["seed"], r["minute"]): r["frozen"]
                 for r in pool.map(job, *zip(*jobs))}
    print("seed  best minute  first serves: best  last   new serves: best  last")
    sums = [0.0] * 4
    for s, row in sorted(rows.items()):
        b, last = max(row, key=row.get), max(row)
        v = [row[b], row[last], again[(s, b)], again[(s, last)]]
        sums = [a + x for a, x in zip(sums, v)]
        print(f"{s:4d}  {b:11d}  {v[0]:17.2f}  {v[1]:4.2f}  {v[2]:16.2f}  {v[3]:4.2f}")
    n = len(rows)
    print("mean" + " " * 13 + "  ".join(f"{x / n:4.2f}" for x in sums))
    return 0


def main() -> int:
    plain = [a for a in sys.argv[1:]]
    if plain and plain[0] == "retest":
        return retest(plain[1], int(plain[2]) if len(plain) > 2 else 60)
    seeds = int(plain[0]) if plain else 8
    minutes = int(plain[1]) if len(plain) > 1 else 20
    rallies = int(plain[2]) if len(plain) > 2 else 60
    out = plain[3] if len(plain) > 3 else None
    _lib()   # build once, before the workers
    jobs = []
    for s in range(1, seeds + 1):
        for m in range(minutes + 1):
            jobs.append((s, 1.0, m, rallies, m == minutes))
        for m in sorted({minutes // 4, minutes // 2, minutes}):
            jobs.append((s, 0.0, m, rallies, m == minutes))
    jobs.sort(key=lambda j: -j[2])
    with ProcessPoolExecutor(10) as pool:
        results = list(pool.map(job, *zip(*jobs)))
    results.sort(key=lambda r: (r["gain"], r["minute"], r["seed"]))
    if out:
        Path(out).write_text(json.dumps(results))

    def table(gain):
        ms = sorted({r["minute"] for r in results if r["gain"] == gain})
        return {m: [r["frozen"] for r in results if r["gain"] == gain and r["minute"] == m] for m in ms}

    trained, untrained = table(1.0), table(0.0)
    live = {r["seed"]: r["live"] for r in results if r["gain"] == 1.0 and r["live"]}
    print(f"frozen hits a rally ({rallies} fixed serves), mean over {seeds} seeds +- s.e.")
    print("minute  trained         untrained       live (learning on)")
    for m, v in trained.items():
        se = statistics.stdev(v) / len(v) ** 0.5 if len(v) > 1 else 0.0
        u = untrained.get(m)
        us = f"{statistics.mean(u):.2f}" if u else ""
        lv = [live[s][m - 1] for s in live if m >= 1]
        ls = f"{statistics.mean(lv):.2f}" if lv else ""
        print(f"{m:6d}  {statistics.mean(v):.2f} +- {se:.2f}   {us:14s}  {ls}")
    best = {}
    for s in range(1, seeds + 1):
        row = [r["frozen"] for r in results if r["gain"] == 1.0 and r["seed"] == s]
        best[s] = (max(range(len(row)), key=row.__getitem__), max(row), row[-1])
    print("seed  best minute  best  last  " + "  ".join(f"m{m}" for m in trained))
    for s, (bm, bv, last) in best.items():
        row = [r["frozen"] for r in results if r["gain"] == 1.0 and r["seed"] == s]
        print(f"{s:4d}  {bm:11d}  {bv:.2f}  {last:.2f}  " + " ".join(f"{v:.2f}" for v in row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
