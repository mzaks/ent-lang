"""How much of the motor regions' answer to a sensory stim comes along the
axons (a stim excites the neurons whose axons pass near the electrode),
and how much through the synapses? On pong.py's `columns` layout, with
the dish alone (no CL SDK), STDP off: on seeds 1 to 4, 5 s to settle,
then 10 rounds of a stim on each of the 8 sensory electrodes, 0.5 s
apart; the up and down regions' spikes in the 100 ms after each, less
those of a 100 ms window without a stim; then the same with every synapse
at 0 (w_exc=0 w_inh=0), which leaves only the direct answer.

    build/cl_bridge/venv/bin/python examples/cl_bridge/reach.py \\
        [axons [name=value ...]]

axons: lengths to try, comma-separated (default 0.274,0.2,0.15,0.11,0.08,
the first the dish's default). Prints, for each, the motor answer with
synapses and without, the share of it that is direct, and the spikes on
all channels in the 100 ms after a stim. Uses ENT_DISH_LIBRARY, as
ent_dish does.
"""

from __future__ import annotations

import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

SEEDS = (1, 2, 3, 4)
ROUNDS = 10


def probe(params):
    os.environ["OMP_NUM_THREADS"] = "1"
    import ent_dish
    import pong
    sensory, up, down, _, _ = pong.LAYOUTS["columns"]
    motor = set(up) | set(down)
    out = []
    for synapses in (True, False):
        p = dict(params) if synapses else dict(params, w_exc=0, w_inh=0)
        answer = spikes = quiet = 0
        for seed in SEEDS:
            dish = ent_dish.Dish(seed=seed, stdp=0, **p)
            dish.step(5000)
            for _ in range(ROUNDS):
                quiet += sum(c in motor for _, c in dish.step(100))
                dish.step(400)
                for c in sensory:
                    dish.stimulate(c, 1.0)
                    got = dish.step(100)
                    answer += sum(ch in motor for _, ch in got)
                    spikes += len(got)
                    dish.step(400)
        stims = len(SEEDS) * ROUNDS * len(sensory)
        out.append((answer / stims - quiet / (len(SEEDS) * ROUNDS), spikes / stims))
    return out


def main() -> int:
    axons = [float(a) for a in (sys.argv[1] if len(sys.argv) > 1 else "0.274,0.2,0.15,0.11,0.08").split(",")]
    extra = {k: float(v) for k, v in (a.split("=", 1) for a in sys.argv[2:])}
    jobs = [dict(extra, axon=a) for a in axons]
    with ProcessPoolExecutor(len(jobs)) as pool:
        results = list(pool.map(probe, jobs))
    print(f"seeds 1 to {len(SEEDS)}, {extra or 'defaults'}; motor spikes in the 100 ms after a sensory stim, "
          "above those without one")
    for a, ((m, s), (m0, s0)) in zip(axons, results):
        print(f"  axon {a:<6g} with synapses {m:5.2f}   without {m0:5.2f}   direct {m0 / max(m, 1e-9):4.2f}"
              f"   all channels {s:5.1f} ({s0:4.1f} without)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
