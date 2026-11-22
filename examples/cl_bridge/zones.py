"""Where on the court does the dish miss? Each seed's Pong dish
(dish_pong.py's, the reward for each move) replayed to a minute, frozen
(STDP off) and played on the same fixed serves as frozen_eval.py's retest;
every ball that reaches the paddle's line counts in the eighth of the
court's height it arrives in (game.c's pong_zones; 0 the top, the rows of
the 8 sensory electrodes), hit or missed. Beside it, the wiring the dish
learnt from each row's sensory electrode to the motor regions: the
excitatory weight to the "up" columns (0, 1) less that to the "down"
columns (6, 7) (bridge.c's dish_paths). A ball in the top rows wants the
paddle up, so a good map is positive at the top and negative at the bottom.

    build/cl_bridge/venv/bin/python examples/cl_bridge/zones.py \\
        [seeds (8)] [rallies (120)] [out.json] [ego (0)]

    ... zones.py rows [seeds (8)] [minutes (20)] [out.json]

how often each row of the ball's code is used in training, both codes:
the share of the sensory stims, of the moves judged (the row of the last
stim before), and of those how many were towards the ball (pong_rows).

    ... zones.py structure [seeds (8)] [minutes (20)] [out.json]

relative code, no and half far serves: each row's use in training, and
the direct synapses from each row's electrode to the up and down regions,
how many and their mean weight as a share of w_max, at the start and the
end (bridge.c's dish_paths).

21 November 2026, 8 seeds: half far serves raise the far rows' share of
the stims only from 0.19 to 0.29 (rows 1-2 still right 0.49 of the time).
The synapse counts follow the electrodes' columns (game.c's SENSORY: rows
0, 3, 4, 7 on column 3, nearer the up regions; rows 1, 2, 5, 6 on column
4, nearer the down ones): rows 0, 3, 4, 7 have 1.3-1.8 times more up
synapses than down, rows 1, 2, 5, 6 1.6-1.9 times more down. The relative
code wants rows 0-3 up and 4-7 down, so the layout agrees in rows 0, 3, 5,
6 and works against it in 1, 2, 4, 7, the rows that stay wrong. Training
raises a row's up and down weights together (row 3: 0.32 to 0.80 and
0.77 of w_max; none at the bound), shifting the balance by a few per cent,
too little to undo a count of 2 to 1.

    ... zones.py practice [seeds (16)] [rallies (120)] [out.json] [ego (1)]

weak-spot practice: 20 minutes of training with no, half or all of the
serves from the half of the court the paddle is not in (game.c's
pong_far_serves), then frozen and scored by row on the usual uniform
fixed serves. Each result is kept in out.jsonl as it comes.

21 November 2026, relative code, 16 seeds (rows mode first: in training
rows 3-5, the ball near the paddle, get ~80% of the stims, rows 0-2 and
6-7 0-6% each, and rows 1-2 move the right way 0.46-0.48 of the time):
far 0.0 all rows 0.42 +- 0.04, far 0.5 0.39 +- 0.03, far 1.0 0.33 +- 0.03.
The practice does not rewire the far rows (rows 1-2 up less down -0.10,
-0.14 with none, -0.07, -0.12 with half) and the middle loses what the
usual serves taught it (rows 3-4 0.32, 0.34 to 0.19, 0.22). Too little
use was not what kept the far rows wrong.

ego 1: the ball told as its height less the paddle's (game.c's ego, pong.py's
BALL_EGO=1; the row the ball arrives in is still the court's), in training
and in the test; the wiring's rows are then the ball's place against the
paddle, the top ones "ball above it", so a good map is again positive at
the top.

Minutes 0, 5, 10 and 20 with the reward, and minute 20 of the gain 0 dish
(no reward; scaling only).

21 November 2026, 8 seeds, 120 serves: the reward teaches the edges (rows
0, 1 and 6 from about 0.3 to 0.7-0.85 by minute 10) and not the middle
(rows 3 and 4 at 0.15-0.22 throughout, as with no reward). The wiring
agrees: only rows 0, 5 and 6 change (up, down, down); rows 3 and 4 keep
the bias they were grown with. The ball's row is the court's height, not
the paddle's, so in the middle the right way depends on where the paddle
is, which the dish is not told: the reward for a middle row's moves is
up as often as down and teaches nothing. Edge rows want the same way
wherever the paddle is. Some seeds learn one edge only (seed 5: rows 0-1
all hit, rows 4-7 none).

The same with ego 1 (relative code), minute 20: rows 3 and 4 at 0.31 and
0.27 (height code 0.22, 0.20; no reward 0.17, 0.19), the edges lower (row
6 0.36 against 0.68), all rows 0.44 against 0.54. Two seeds of 8 (3 and 4)
track the ball over the whole court, 0.43-0.73 in every row, which no
seed does with the height code; most park the paddle at the top. The
reward rewires one row only (3, ball just above the paddle: up, 0.12 to
0.31); rows 1 and 2 ("ball well above") stay wired down as grown.
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
SENSORY = [3, 12, 20, 27, 35, 44, 52, 59]   # game.c's, a row each
POINTS = [(1.0, 0), (1.0, 5), (1.0, 10), (1.0, 20), (0.0, 20)]


def job(seed: int, gain: float, minute: int, rallies: int, ego: int = 0,
        far: float = 0.0) -> dict:
    lib = fe._lib()
    lib.pong_zones.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dish_paths.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    state = (ctypes.c_double * 12)()
    dish = lib.pong_make_dish(seed, gain, 0)
    game = lib.pong_create(dish, seed, 0, ego)
    if far:
        lib.pong_far_serves.argtypes = [ctypes.c_void_p, ctypes.c_double]
        lib.pong_far_serves(game, far)
    for _ in range(minute):
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
    lib.pong_destroy(game)
    lib.dish_set_stdp(dish, 0)
    w = (ctypes.c_float * 4096)()
    n = (ctypes.c_float * 4096)()
    lib.dish_paths(dish, w, n)
    wiring = []
    for c in SENSORY:
        up = sum(w[c * 64 + b] for b in range(64) if b % 8 <= 1)
        down = sum(w[c * 64 + b] for b in range(64) if b % 8 >= 6)
        wiring.append(up - down)
    test = lib.pong_create(dish, EVAL_SEED, 0, ego)
    lib.pong_fixed_serves(test, SERVES)
    t0 = None
    while True:
        if lib.pong_run(test, fe.CHUNK, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(test, state)
        if t0 is None:
            t0 = state[0]
        if state[4] >= rallies or state[0] - t0 > 1800:
            break
    z = (ctypes.c_int32 * 16)()
    lib.pong_zones(test, z)
    lib.pong_destroy(test)
    lib.dish_destroy(dish)
    return {"seed": seed, "gain": gain, "minute": minute, "ego": ego, "far": far,
            "rallies": int(state[4]),
            "seen": list(z[:8]), "hit": list(z[8:]), "wiring": wiring}


def rows_job(seed: int, ego: int, minutes: int) -> dict:
    lib = fe._lib()
    lib.pong_rows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    dish = lib.pong_make_dish(seed, 1.0, 0)
    game = lib.pong_create(dish, seed, 0, ego)
    marks = []
    r = (ctypes.c_int32 * 24)()
    for m in range(1, minutes + 1):
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_rows(game, r)
        marks.append(list(r))
    lib.pong_destroy(game)
    lib.dish_destroy(dish)
    return {"seed": seed, "ego": ego, "rows": marks}


def rows_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 8
    minutes = int(args[1]) if len(args) > 1 else 20
    out = args[2] if len(args) > 2 else None
    jobs = [(s, e, minutes) for e in (0, 1) for s in range(1, seeds + 1)]
    fe._lib()
    with ProcessPoolExecutor(10) as pool:
        results = [f.result() for f in as_completed([pool.submit(rows_job, *j) for j in jobs])]
    if out:
        Path(out).write_text(json.dumps(results))
    first = 5
    for ego in (0, 1):
        rs = [r for r in results if r["ego"] == ego]
        print(f"\n{'relative code (ego 1): row 0-3 ball above the paddle' if ego else 'height code: row 0 the top of the court'}"
              f", {len(rs)} seeds")
        print("                  " + "  ".join(f" row{k}" for k in range(8)))
        for name, a, b in (("minutes 0-5", None, first - 1), (f"minutes {first * 3}-{minutes}", first * 3 - 1, minutes - 1)):
            tot = [0] * 24
            for r in rs:
                hi = r["rows"][b]
                lo = r["rows"][a] if a is not None else [0] * 24
                tot = [t + h - l for t, h, l in zip(tot, hi, lo)]
            sensed, judged, towards = tot[:8], tot[8:16], tot[16:]
            print(f"{name:14s} stims " + "  ".join(f"{x / sum(sensed):5.2f}" for x in sensed))
            print(f"{'':14s} moves " + "  ".join(f"{x / sum(judged):5.2f}" for x in judged))
            print(f"{'':14s} right " + "  ".join(f"{t / j if j else 0:5.2f}" for t, j in zip(towards, judged)))
    return 0


def practice_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else None
    ego = int(args[3]) if len(args) > 3 else 1
    shares = [0.0, 0.5, 1.0]
    done_path = Path((out or "build/cl_bridge/practice.json") + "l")
    results = []
    if done_path.exists():
        results = [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["far"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, ego, f) for f in shares for s in range(1, seeds + 1)
            if (s, f) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    if out:
        Path(out).write_text(json.dumps(results))
    print(f"{'relative' if ego else 'height'} code, 20 minutes of training, a share of the serves "
          f"far from the paddle; frozen, {rallies} uniform fixed serves, {seeds} seeds")
    print("hit rate by court row  " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    for f in shares:
        rs = [r for r in results if r["far"] == f]
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        each = [sum(r["hit"]) / sum(r["seen"]) for r in rs]
        se = statistics.stdev(each) / len(each) ** 0.5
        print(f"far {f:.1f}               " + "  ".join(f"{h / s:5.2f}" for h, s in zip(hit, seen)) +
              f"   {statistics.mean(each):5.2f}  {se:.2f}")
    print("wiring up less down")
    for f in shares:
        rs = [r for r in results if r["far"] == f]
        print(f"far {f:.1f}               " + "  ".join(
            f"{statistics.mean(r['wiring'][k] for r in rs):5.2f}" for k in range(8)))
    print("all rows by seed")
    for f in shares:
        rs = sorted((r for r in results if r["far"] == f), key=lambda r: r["seed"])
        print(f"far {f:.1f}  " + " ".join(f"{sum(r['hit']) / sum(r['seen']):.2f}" for r in rs))
    return 0


W_MAX = 0.00605   # network.ent's Dish.w_max (an excitatory synapse starts at 0.00194)


def _paths(lib, dish) -> dict:
    w = (ctypes.c_float * 4096)()
    n = (ctypes.c_float * 4096)()
    lib.dish_paths(dish, w, n)
    out = {"n_up": [], "n_down": [], "w_up": [], "w_down": []}
    for c in SENSORY:
        for way, cols in (("up", (0, 1)), ("down", (6, 7))):
            ns = sum(n[c * 64 + b] for b in range(64) if b % 8 in cols)
            ws = sum(w[c * 64 + b] for b in range(64) if b % 8 in cols)
            out["n_" + way].append(ns)
            out["w_" + way].append(ws / ns / W_MAX if ns else 0.0)
    return out


def structure_job(seed: int, ego: int, far: float, minutes: int) -> dict:
    lib = fe._lib()
    lib.pong_rows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.pong_far_serves.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.dish_paths.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    dish = lib.pong_make_dish(seed, 1.0, 0)
    game = lib.pong_create(dish, seed, 0, ego)
    if far:
        lib.pong_far_serves(game, far)
    start = _paths(lib, dish)
    r = (ctypes.c_int32 * 24)()
    for _ in range(minutes):
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
    lib.pong_rows(game, r)
    end = _paths(lib, dish)
    lib.pong_destroy(game)
    lib.dish_destroy(dish)
    return {"seed": seed, "ego": ego, "far": far, "rows": list(r), "start": start, "end": end}


def structure_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 8
    minutes = int(args[1]) if len(args) > 1 else 20
    out = args[2] if len(args) > 2 else None
    jobs = [(s, 1, f, minutes) for f in (0.0, 0.5) for s in range(1, seeds + 1)]
    fe._lib()
    with ProcessPoolExecutor(10) as pool:
        results = [f.result() for f in as_completed([pool.submit(structure_job, *j) for j in jobs])]
    if out:
        Path(out).write_text(json.dumps(results))
    mean = statistics.mean
    print(f"relative code, {seeds} seeds, {minutes} minutes of training; rows 0-3 the ball above the paddle")
    print("                       " + "  ".join(f" row{k}" for k in range(8)))
    for far in (0.0, 0.5):
        rs = [r for r in results if r["far"] == far]
        tot = [sum(r["rows"][k] for r in rs) for k in range(24)]
        sensed, judged, towards = tot[:8], tot[8:16], tot[16:]
        print(f"far {far:.1f}  stims share  " + "  ".join(f"{x / sum(sensed):5.2f}" for x in sensed))
        print(f"          moves right  " + "  ".join(f"{t / j if j else 0:5.2f}" for t, j in zip(towards, judged)))
    print("\ndirect synapses from the row's electrode to the up (columns 0, 1) and down (6, 7) regions")
    rs = [r for r in results if r["far"] == 0.0]
    for way in ("up", "down"):
        print(f"{way:5s} synapses      " + "  ".join(f"{mean(r['start']['n_' + way][k] for r in rs):5.1f}" for k in range(8)))
    for when in ("start", "end"):
        for far in ((0.0,) if when == "start" else (0.0, 0.5)):
            rs = [r for r in results if r["far"] == far]
            for way in ("up", "down"):
                label = f"{way} w/w_max {when}" + (f" far {far}" if when == "end" else "")
                print(f"{label:22s} " + "  ".join(f"{mean(r[when]['w_' + way][k] for r in rs):5.2f}" for k in range(8)))
    return 0


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "structure":
        return structure_main(args[1:])
    if args and args[0] == "practice":
        return practice_main(args[1:])
    if args and args[0] == "rows":
        return rows_main(args[1:])
    seeds = int(args[0]) if args else 8
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else None
    ego = int(args[3]) if len(args) > 3 else 0
    jobs = [(s, g, m, rallies, ego) for g, m in POINTS for s in range(1, seeds + 1)]
    jobs.sort(key=lambda j: -j[2])
    fe._lib()
    results = []
    with ProcessPoolExecutor(10) as pool:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            results.append(f.result())
    results.sort(key=lambda r: (-r["gain"], r["minute"], r["seed"]))
    if out:
        Path(out).write_text(json.dumps(results))

    def rows(gain, minute):
        return [r for r in results if r["gain"] == gain and r["minute"] == minute]

    print(f"hit rate by the row the ball reaches the paddle's line in (0 the top), "
          f"{seeds} seeds, {rallies} fixed serves")
    print("                   " + "  ".join(f"  row{k}" for k in range(8)) + "    all")
    for gain, minute in POINTS:
        rs = rows(gain, minute)
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        name = f"{'reward' if gain else 'no reward'} m{minute}"
        print(f"{name:18s} " + "  ".join(f"{h / s if s else 0:6.2f}" for h, s in zip(hit, seen)) +
              f"  {sum(hit) / sum(seen):5.2f}")
    rs = rows(1.0, 20)
    print(f"{'balls (m20)':18s} " + "  ".join(f"{sum(r['seen'][k] for r in rs):6d}" for k in range(8)))
    print("\nwiring up less down (sum of weights), mean over seeds")
    for gain, minute in POINTS:
        rs = rows(gain, minute)
        name = f"{'reward' if gain else 'no reward'} m{minute}"
        print(f"{name:18s} " + "  ".join(f"{statistics.mean(r['wiring'][k] for r in rs):6.2f}"
                                         for k in range(8)))
    print("\nper seed at minute 20 with the reward: hit rate by row")
    for r in rows(1.0, 20):
        print(f"seed {r['seed']:2d}            " + "  ".join(
            f"{h / s if s else 0:6.2f}" for h, s in zip(r["hit"], r["seen"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
