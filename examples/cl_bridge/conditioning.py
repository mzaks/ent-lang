"""Can reward-modulated STDP (network.ent's `modulated`) teach the dish to
answer a stim with one motor region rather than the other? The simplest
test before Pong, on pong.py's `columns` layout, with the dish alone (no
CL SDK): after a minute to settle, a stim on one of the 8 sensory
electrodes, drawn at random, every 0.5 s; the up and down regions'
spikes in the 100 ms after it; then a pulse on the reward or the punish
channel. Modes:

    bias   the answer is right when up has more (any stim)
    map    up for the electrodes of the court's top half, down for the
           bottom: what Pong asks
    rpe    map, the pulse the reward prediction error (Fremaux, Sprekeler
           and Gerstner 2010): 1 - p for a right answer, p for a wrong
           one, p each electrode's running share of right answers

    build/cl_bridge/venv/bin/python examples/cl_bridge/conditioning.py \\
        mode [minutes [gains [seeds [name=value ...]]]]

name=value sets a dish parameter, but for four of the loop's own: stim,
the sensory stim's size (1), answer, the answer window in ms (100),
layout (0 pong.py's `columns`; 1 game.c's pong_layout(1), the best Pong
setup's: the top half's sensory electrodes on column 3, nearer the up
region, the bottom half's on column 4, reward 28, punish 35) and flip (1:
the map turned over, down for the top half, against what the aligned
layout's wiring leans to).

Prints, for each gain (reward_gain; 0: nothing learnt), the share of
right answers in each 2-minute window, mean over seeds (map, rpe: the top
half's / the bottom half's), and each seed's mean excitatory weight at
the end; then, seed by seed, over the second half of the session, the
score: the right share less 0.5 (bias), the top half's plus the bottom
half's less 1 (map, rpe: 0 where the answer does not depend on the
stim, as for a bias), mean ± standard error, and less gain 0's. Uses ENT_DISH_LIBRARY, as ent_dish does.
"""

from __future__ import annotations

import os
import random
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

SETTLE = 60000        # steps (ms)
EVERY, ANSWER = 500, 100
WINDOW = 240          # trials: 2 minutes
MODES = ("bias", "map", "rpe")


def run(job):
    mode, seed, gain, minutes, extra = job
    extra = dict(extra)
    size = extra.pop("stim", 1.0)
    answer = int(extra.pop("answer", ANSWER))
    layout = int(extra.pop("layout", 0))
    flip = int(extra.pop("flip", 0))
    os.environ["OMP_NUM_THREADS"] = "1"
    import ent_dish
    import pong
    sensory, up, down, reward, punish = pong.LAYOUTS["columns"]
    if layout == 1:
        sensory, reward, punish = [3, 11, 19, 27, 36, 44, 52, 60], 28, 35
    up, down = set(up), set(down)
    dish = ent_dish.Dish(seed=seed, modulated=1, reward_channel=reward,
                         punish_channel=punish, reward_gain=gain, **extra)
    rng = random.Random(seed)
    expect = [0.5] * 8
    dish.step(SETTLE)
    windows, right = [], [[0, 0], [0, 0]]        # per half: right, wrong
    said = trials = 0
    for k in range(int(minutes * 60000 / EVERY)):
        i = rng.randrange(8)
        dish.stimulate(sensory[i], size)
        spikes = dish.step(answer)
        u = sum(c in up for _, c in spikes)
        d = sum(c in down for _, c in spikes)
        said += u + d
        trials += 1
        half = 0 if mode == "bias" or i < 4 else 1
        want, other = (u, d) if (half == 0) != bool(flip) else (d, u)
        if want != other:
            ok = want > other
            right[half][0 if ok else 1] += 1
            if mode == "rpe":
                err = (1.0 if ok else 0.0) - expect[i]
                expect[i] += 0.05 * err
            else:
                err = 1.0 if ok else -1.0
            if err > 0:
                dish.stimulate(reward, err)
            elif err < 0:
                dish.stimulate(punish, -err)
        dish.step(EVERY - answer)
        if (k + 1) % WINDOW == 0:
            windows.append([r / max(1, r + w) for r, w in right])
            right = [[0, 0], [0, 0]]
    return seed, gain, windows, dish.mean_weight(), said / trials


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "map"
    if mode not in MODES:
        raise SystemExit(f"no mode {mode!r}: {', '.join(MODES)}")
    minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    gains = [float(g) for g in (sys.argv[3] if len(sys.argv) > 3 else "0,10,100").split(",")]
    seeds = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    extra = {k: float(v) for k, v in (a.split("=", 1) for a in sys.argv[5:])}
    jobs = [(mode, s, g, minutes, extra) for g in gains for s in range(1, seeds + 1)]
    with ProcessPoolExecutor(min(len(jobs), 22)) as pool:
        results = list(pool.map(run, jobs))
    print(f"{mode}, {minutes:g} minutes after 1 to settle, seeds 1 to {seeds}, "
          f"{extra or 'defaults'}; right answers by 2-minute window")
    for g in gains:
        rs = [r for r in results if r[1] == g]
        n = len(rs[0][2])

        def at(k, h):
            return sum(r[2][k][h] for r in rs) / len(rs)
        cells = [f"{at(k, 0):.2f}" if mode == "bias" else f"{at(k, 0):.2f}/{at(k, 1):.2f}"
                 for k in range(n)]
        print(f"  gain {g:<6g} " + " ".join(cells)
              + "   weight " + " ".join(f"{r[3]:.5f}" for r in rs)
              + f"   motor spikes an answer {sum(r[4] for r in rs) / len(rs):.1f}")
    from pong_study import mean_se

    def score(r):
        late = r[2][len(r[2]) // 2:]
        return sum(w[0] - 0.5 if mode == "bias" else w[0] + w[1] - 1 for w in late) / len(late)
    base = {r[0]: score(r) for r in results if r[1] == gains[0]}
    print("score, second half (seed by seed):")
    for g in gains:
        rs = sorted((r for r in results if r[1] == g), key=lambda r: r[0])
        m, se = mean_se([score(r) for r in rs])
        dm, dse = mean_se([score(r) - base[r[0]] for r in rs])
        print(f"  gain {g:<6g} {m:+.3f} ± {se:.3f}   less gain {gains[0]:g}: {dm:+.3f} ± {dse:.3f}   "
              + " ".join(f"{score(r):+.2f}" for r in rs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
