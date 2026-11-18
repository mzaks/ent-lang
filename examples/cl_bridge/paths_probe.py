"""Where does conditioning.py's learning stop? Its `rpe` loop (columns
layout, the dish alone, gains 0 and 1, seeds 1 to 8), and each minute:
the share of right answers, the mean |reward prediction error|, the
mean excitatory weight (over w_max) from each sensory electrode's
neurons to the region its stims should drive and to the other one
(Dish.paths), that of all paths, and the dish's rate a channel.

    build/cl_bridge/venv/bin/python examples/cl_bridge/paths_probe.py \
        minutes [settle=minutes] [freeze=1] [name=value ...]

settle: the minutes before the first trial (1); freeze: scaling
(home_gain) off from the first trial on, so the dish settles with it.
Uses ENT_DISH_LIBRARY, as ent_dish does.
"""
import os, sys, random
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

def run(job):
    seed, gain, minutes, extra = job
    os.environ["OMP_NUM_THREADS"] = "1"
    import numpy as np, ent_dish, pong
    sensory, up, down, reward, punish = pong.LAYOUTS["columns"]
    upl, downl = list(up), list(down)
    up, down = set(up), set(down)
    extra = dict(extra)
    dish = ent_dish.Dish(seed=seed, modulated=1, reward_channel=reward, punish_channel=punish,
                         reward_gain=gain, **{k: v for k, v in extra.items() if k not in ('settle', 'freeze')})
    wmax = extra.get("w_max", 0.00605)
    rng = random.Random(seed)
    expect = [0.5] * 8
    settle = extra.pop("settle", 1.0)
    freeze = extra.pop("freeze", 0.0)
    dish.step(int(settle * 60000))
    if freeze:
        dish.set_home_gain(0.0)
    out = []
    right = wrong = 0
    absrpe = 0.0; nrpe = 0; allsp = 0
    def snap():
        w, n = dish.paths()
        rows = []
        for i, s in enumerate(sensory):
            wu = w[s, upl].sum() / max(n[s, upl].sum(), 1)
            wd = w[s, downl].sum() / max(n[s, downl].sum(), 1)
            rows.append((wu, wd) if i < 4 else (wd, wu))     # (right path, wrong path)
        r = np.array(rows)
        allw = w.sum() / n.sum()
        return r[:, 0].mean() / wmax, r[:, 1].mean() / wmax, allw / wmax
    out.append((0, None, None) + snap() + (None,))
    for k in range(int(minutes * 120)):
        i = rng.randrange(8)
        dish.stimulate(sensory[i], 1.0)
        sp = dish.step(100)
        u = sum(c in up for _, c in sp); d = sum(c in down for _, c in sp)
        want, other = (u, d) if i < 4 else (d, u)
        if want != other:
            ok = want > other
            right += ok; wrong += not ok
            err = (1.0 if ok else 0.0) - expect[i]
            expect[i] += 0.05 * err
            absrpe += abs(err); nrpe += 1
            if err > 0: dish.stimulate(reward, err)
            elif err < 0: dish.stimulate(punish, -err)
        allsp += len(sp) + len(dish.step(400))
        if (k + 1) % 120 == 0:
            out.append(((k + 1) // 120, right / max(1, right + wrong), absrpe / max(1, nrpe)) + snap() + (allsp / 60 / 64,))
            right = wrong = 0; absrpe = 0.0; nrpe = 0; allsp = 0
    return seed, gain, out

if __name__ == "__main__":
    minutes = float(sys.argv[1]); extra = {k: float(v) for k, v in (a.split("=") for a in sys.argv[2:])}
    jobs = [(s, g, minutes, extra) for g in (0.0, 1.0) for s in range(1, 9)]
    with ProcessPoolExecutor(16) as ex:
        res = list(ex.map(run, jobs))
    import numpy as np
    print(extra)
    for g in (0.0, 1.0):
        rs = [r for r in res if r[1] == g]
        print(f"gain {g}: minute, right share, mean |rpe|, right-path w/w_max, wrong-path, all")
        for m in range(len(rs[0][2])):
            row = [r[2][m] for r in rs]
            f = lambda j: np.mean([x[j] for x in row]) if row[0][j] is not None else float('nan')
            print(f"  {m:3d}  {f(1):.3f}  {f(2):.3f}   {f(3):.3f}  {f(4):.3f}  {f(5):.3f}   {f(6):.2f} Hz/channel")
