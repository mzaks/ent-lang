"""Does DishBrain's feedback teach this dish Pong, with no reward channel?
pong.py's `stim` (a hit: all 8 sensory electrodes at 100 Hz for 100 ms,
predictable; a miss: 4 s of stims on random electrodes at 5 Hz,
unpredictable), against DishBrain's controls `silent` (4 s without
input after a miss) and `none` (no feedback, no pause), and `stim/off`:
`stim` with the weights fixed (stdp=0) at about where scaling settles
them (w_exc=0.0027; at the default w_exc, with no scaling, the dish is
nearly silent), which can only do what the wiring does. Plain STDP, no
dopamine: what a CL1 could do. On the `columns` layout, seeds 1 to
`seeds`, each as is and flipped.

    build/cl_bridge/venv/bin/python examples/cl_bridge/feedback_study.py \\
        [minutes [seeds [out.json [name=value ...]]]]

name=value: dish parameters for every session (axon=0.11, hard=1, ...).
Prints, for each session, hits a rally in the first 5 minutes and after,
mean over the seeds and flips; then, pair by pair (the same seed and
flip), the differences that say whether it learned: each session's late
less its early, and stim less each control, late.
"""

from __future__ import annotations

import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

SESSIONS = [("stim", {}), ("silent", {}), ("none", {}), ("stim/off", {"stdp": 0, "w_exc": 0.0027})]


def run(job):
    minutes, seed, flip, name, parameters = job
    os.environ["OMP_NUM_THREADS"] = "1"
    import pong
    out = pong.play(minutes, seed, name.split("/")[0], flip, "columns", **parameters)
    out["session"] = name
    return out


def main() -> int:
    from pong_study import mean_se
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    out = sys.argv[3] if len(sys.argv) > 3 else None
    extra = {k: float(v) for k, v in (a.split("=", 1) for a in sys.argv[4:])}
    jobs = [(minutes, seed, flip, name, {**extra, **p})
            for name, p in SESSIONS for flip in (False, True) for seed in range(1, seeds + 1)]
    with ProcessPoolExecutor(min(len(jobs), 22)) as pool:
        results = list(pool.map(run, jobs))
    if out:
        with open(out, "w") as f:
            json.dump(results, f)
    by = {(r["session"], r["seed"], r["flip"]): r for r in results}
    keys = [(s, f) for f in (False, True) for s in range(1, seeds + 1)]

    def hits(name, part, k):
        return by[(name, *k)][part].get("hits", float("nan"))

    def cell(xs):
        m, se = mean_se(xs)
        return f"{m:5.2f} ± {se:4.2f}"

    print(f"{minutes:g} minutes, seeds 1 to {seeds} as is and flipped, columns, "
          f"{extra or 'defaults'}; hits a rally, mean ± standard error")
    print(f"{'session':>9} {'rallies':>8} {'first 5 min':>13} {'after':>13}")
    for name, _ in SESSIONS:
        print(f"{name:>9} {sum(by[(name, *k)]['all']['rallies'] for k in keys) / len(keys):8.1f} "
              f"{cell([hits(name, 'early', k) for k in keys]):>13} "
              f"{cell([hits(name, 'late', k) for k in keys]):>13}")
    print("differences, pair by pair (hits a rally):")
    pairs = [(f"{n}: after less first 5 min", lambda k, n=n: hits(n, "late", k) - hits(n, "early", k))
             for n, _ in SESSIONS]
    pairs += [(f"stim less {n}, after", lambda k, n=n: hits("stim", "late", k) - hits(n, "late", k))
              for n, _ in SESSIONS[1:]]
    for label, f in pairs:
        xs = [f(k) for k in keys]
        m, se = mean_se(xs)
        print(f"  {label:>30}: {m:+.3f} ± {se:.3f}   {sum(x > 0 for x in xs)} of {len(xs)} above")
    return 0


if __name__ == "__main__":
    sys.exit(main())
