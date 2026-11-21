"""Go back to the best wiring and learn on from there, more gently. Each
seed's Pong dish (dish_pong.py's, the reward for each move) is replayed to
its best minute of frozen_eval.py (the dish and the game are deterministic,
so that is the wiring it had then), learns on for 10 more minutes with a
smaller learning rate (reward_gain), and is then frozen and played on the
second set of serves of frozen_eval.py's retest: serves the best minute was
not picked on, so picking it does not flatter the score.

    build/cl_bridge/venv/bin/python examples/cl_bridge/resume.py \\
        build/cl_bridge/frozen_eval.json [extra minutes (10)] [rallies (60)] [out.json]

Each job's result is kept in out.jsonl as it comes; run it again and it
goes on with the jobs not done.

The ways on, each from the best minute and, for comparison, from minute 20
(where the dish had simply got to):
    1.0      as before
    0.3      a third of the learning rate
    0.1      a tenth
    halving  1.0 halved every 2 minutes (1/32 at the end of 10)

21 November 2026, 8 seeds, frozen hits a rally on the new serves (+- s.e.):
                     from best minute  from minute 20
    at the start     1.41 +- 0.14      1.11 +- 0.16
    +10 min, 1.0     1.32 +- 0.16      1.13 +- 0.20
    +10 min, 0.3     0.81 +- 0.09      0.94 +- 0.13
    +10 min, 0.1     0.80 +- 0.10      1.11 +- 0.21
    +10 min, halving 0.90 +- 0.12      0.84 +- 0.10
A smaller learning rate does not keep the best wiring, it loses it: the
neurons' scaling (home_gain) goes on at its full rate, and with less
reward to hold it the map fades towards the no-reward dish (0.4-0.5).
Learning on at 1.0 keeps most of it; stopping at the best minute keeps all.

Picking the best minute is the host searching the dish's wiring with the
score in hand: it can be done to the model, never to a culture.
"""
from __future__ import annotations

import ctypes
import json
import statistics
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import frozen_eval as fe  # noqa: E402

SERVES, EVAL_SEED = 0x0BADC0DE, 777   # frozen_eval.py's retest serves
WAYS = ["1.0", "0.3", "0.1", "halving"]


def _gain(way: str, minute: int) -> float:
    return 0.5 ** (minute // 2) if way == "halving" else float(way)


def job(seed: int, start: int, way: str, extra: int, rallies: int) -> dict:
    lib = fe._lib()
    lib.dish_set_reward_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]
    state = (ctypes.c_double * 12)()
    dish = lib.pong_make_dish(seed, 1.0, 0)
    game = lib.pong_create(dish, seed, 0, 0)
    for m in range(start + extra):
        if m >= start:
            lib.dish_set_reward_gain(dish, _gain(way, m - start))
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
    lib.pong_state(game, state)
    lib.pong_destroy(game)
    lib.dish_set_stdp(dish, 0)
    test = lib.pong_create(dish, EVAL_SEED, 0, 0)
    lib.pong_fixed_serves(test, SERVES)
    hits_at, t0 = [], None
    while True:
        if lib.pong_run(test, fe.CHUNK, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(test, state)
        if t0 is None:
            t0 = state[0]
        while len(hits_at) < int(state[4]):
            hits_at.append(state[7])
        if len(hits_at) >= rallies or state[0] - t0 > 900:
            break
    lib.pong_destroy(test)
    lib.dish_destroy(dish)   # a worker runs many jobs: the dish is ~170 MB
    n = min(len(hits_at), rallies)
    return {"seed": seed, "start": start, "way": way, "extra": extra,
            "frozen": hits_at[n - 1] / n if n else 0.0, "rallies": n}


def main() -> int:
    args = sys.argv[1:]
    first = json.loads(Path(args[0]).read_text())
    extra = int(args[1]) if len(args) > 1 else 10
    rallies = int(args[2]) if len(args) > 2 else 60
    out = args[3] if len(args) > 3 else None
    rows: dict[int, dict[int, float]] = {}
    for r in first:
        if r["gain"] == 1.0:
            rows.setdefault(r["seed"], {})[r["minute"]] = r["frozen"]
    starts = {s: (max(row, key=row.get), max(row)) for s, row in rows.items()}
    jobs = []
    for s, (best, last) in sorted(starts.items()):
        for way in WAYS:
            jobs.append((s, best, way, extra, rallies))
            if last != best:
                jobs.append((s, last, way, extra, rallies))
        jobs.append((s, best, "1.0", 0, rallies))   # the start itself, on these serves
        jobs.append((s, last, "1.0", 0, rallies))
    jobs = list(dict.fromkeys(jobs))
    jobs.sort(key=lambda j: -(j[1] + j[3]))
    # Each result is written down as it comes (out.json + "l", a line each),
    # and a run started again skips the jobs written there.
    done_path = Path((out or "build/cl_bridge/resume.json") + "l")
    results = []
    if done_path.exists():
        results = [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["start"], r["way"], r["extra"]) for r in results}
    todo = [j for j in jobs if (j[0], j[1], j[2], j[3]) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in todo]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    if out:
        Path(out).write_text(json.dumps(results))
    got = {(r["seed"], r["start"], r["way"], r["extra"]): r["frozen"] for r in results}

    def column(which: int, way: str, ext: int) -> list[float]:
        return [got[(s, st[which], way, ext)] for s, st in sorted(starts.items())]

    def show(v: list[float]) -> str:
        se = statistics.stdev(v) / len(v) ** 0.5 if len(v) > 1 else 0.0
        return f"{statistics.mean(v):.2f} +- {se:.2f}"

    print(f"frozen hits a rally on {rallies} new serves, mean over {len(starts)} seeds +- s.e.")
    print(f"{'':22s}from best minute  from minute 20")
    print(f"{'at the start':22s}{show(column(0, '1.0', 0)):18s}{show(column(1, '1.0', 0))}")
    for way in WAYS:
        print(f"{'+' + str(extra) + ' min, gain ' + way:22s}"
              f"{show(column(0, way, extra)):18s}{show(column(1, way, extra))}")
    print("seed  best  start  " + "  ".join(f"{w:>7s}" for w in WAYS) + "   | m20 start  " +
          "  ".join(f"{w:>7s}" for w in WAYS))
    for s, (best, last) in sorted(starts.items()):
        a = [got[(s, best, w, extra)] for w in WAYS]
        b = [got[(s, last, w, extra)] for w in WAYS]
        print(f"{s:4d}  {best:4d}  {got[(s, best, '1.0', 0)]:5.2f}  " +
              "  ".join(f"{x:7.2f}" for x in a) + f"   | {got[(s, last, '1.0', 0)]:9.2f}  " +
              "  ".join(f"{x:7.2f}" for x in b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
