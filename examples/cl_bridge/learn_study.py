"""Can the dish learn Pong with a learning rule told how it did? pong.py's
`reward` condition (reward-modulated STDP, a pulse of dopamine for a hit,
one taken away for a miss) at each of `gains` (reward_gain), with the
sensory map as it is and upside down, on seeds 1 to `seeds`, each a
session of `minutes`, a process each.

    build/cl_bridge/venv/bin/python examples/cl_bridge/learn_study.py \\
        [minutes [seeds [gains [out.json [columns] [moves|shuffled] [name=value ...]]]]]

moves: pong.py's `moves` condition (a pulse for each paddle move towards
the ball or away) instead of `reward` (for hits and misses); shuffled:
its control, each move's outcome a coin flip.
columns: pong.py's second layout, where the stims' direct answers do not
play the game. gains: comma-separated, e.g. 0,0.3,3 (0: the same game and dish, nothing
learnt: the control). name=value: more of the dish's parameters, for all.
For each session, hits a rally in each `WINDOW` of the session (mean
over seeds); then, seed by seed against gain 0, the last window's hits.
Learning is a gain whose hits climb over the session and end above
gain 0's; flipped, the wiring works against the game (0.11 a rally with
STDP off), so there it has the most to learn.
"""

from __future__ import annotations

import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

WINDOW = 5 * 60   # s


def run(job):
    minutes, seed, gain, flip, layout, condition, extra = job
    os.environ["OMP_NUM_THREADS"] = "1"
    import pong
    out = pong.play(minutes, seed, condition, flip, layout, reward_gain=gain, **extra)
    out["gain"], out["seed"] = gain, seed
    return out


def windows(rallies, minutes):
    n = max(1, int(minutes * 60 // WINDOW))
    out = []
    for k in range(n):
        hs = [h for t, h in rallies if k * WINDOW <= t < (k + 1) * WINDOW]
        out.append(sum(hs) / len(hs) if hs else float("nan"))
    return out


def mean_se(xs):
    from pong_study import mean_se as f
    return f(xs)


def main() -> int:
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    gains = [float(g) for g in (sys.argv[3] if len(sys.argv) > 3 else "0,0.3,3,30").split(",")]
    out = sys.argv[4] if len(sys.argv) > 4 else None
    layout = "columns" if "columns" in sys.argv[5:] else "rows"
    condition = next((c for c in ("moves", "shuffled") if c in sys.argv[5:]), "reward")
    extra = {k: float(v) for k, v in (a.split("=", 1) for a in sys.argv[5:] if "=" in a)}
    jobs = [(minutes, s, g, f, layout, condition, extra) for f in (False, True) for g in gains
            for s in range(1, seeds + 1)]
    with ProcessPoolExecutor(min(len(jobs), 22)) as pool:
        results = list(pool.map(run, jobs))
    if out:
        with open(out, "w") as f:
            json.dump(results, f)
    by = {(r["flip"], r["gain"], r["seed"]): r for r in results}
    print(f"{minutes:g} minutes, seeds 1 to {seeds}, {condition}, layout {layout}, {extra or 'defaults'}; "
          f"hits a rally by {WINDOW // 60}-minute window, mean over seeds")
    for flip in (False, True):
        for g in gains:
            rs = [by[(flip, g, s)] for s in range(1, seeds + 1)]
            ws = [windows(r["rallies"], minutes) for r in rs]
            cols = [mean_se([w[k] for w in ws])[0] for k in range(len(ws[0]))]
            base = [windows(by[(flip, gains[0], s)]["rallies"], minutes)[-1] for s in range(1, seeds + 1)]
            m, se = mean_se([w[-1] - b for w, b in zip(ws, base)])
            hz = sum(r["up_hz"] + r["down_hz"] for r in rs) / len(rs)
            towards = sum(r["moves_towards"] for r in rs) / len(rs)
            print(f"  {'flip' if flip else 'as is':>5} gain {g:<6g} "
                  + " ".join(f"{c:5.2f}" for c in cols)
                  + f"   last less gain {gains[0]:g}: {m:+.2f} ± {se:.2f}   motor {hz:6.0f} Hz"
                  + (f"   moves towards {towards:.3f}" if condition != "reward" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
