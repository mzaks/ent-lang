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
        [seeds (8)] [rallies (120)] [out.json]

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


def job(seed: int, gain: float, minute: int, rallies: int) -> dict:
    lib = fe._lib()
    lib.pong_zones.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dish_paths.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    state = (ctypes.c_double * 12)()
    dish = lib.pong_make_dish(seed, gain, 0)
    game = lib.pong_create(dish, seed, 0, 0)
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
    test = lib.pong_create(dish, EVAL_SEED, 0, 0)
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
    return {"seed": seed, "gain": gain, "minute": minute, "rallies": int(state[4]),
            "seen": list(z[:8]), "hit": list(z[8:]), "wiring": wiring}


def main() -> int:
    args = sys.argv[1:]
    seeds = int(args[0]) if args else 8
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else None
    jobs = [(s, g, m, rallies) for g, m in POINTS for s in range(1, seeds + 1)]
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
