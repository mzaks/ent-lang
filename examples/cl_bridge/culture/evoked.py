"""Statistics of the answers to stims, the same for a culture's recording
of Wagenaar, Pine and Potter (2006) and the dish's.

Their stimulus-evoked recordings stim one electrode every 0.3 s, going
round the electrodes in turn (50 stims of each). The answer to a stim is
the spikes on the other channels until the next stim; of them:

- early: spikes a stim 3 to 20 ms after it (the first 3 ms are lost to
  the stim's artefact on a culture's array), on all channels;
- late: spikes a stim 20 to 300 ms after it;
- channels: the median number of channels with a spike 3 to 100 ms after
  a stim;
- big: the share of stims answered by a network burst (as many spikes
  3 to 300 ms after it as 2 a channel that is active);
- latency: the median latency (ms) of the spikes 3 to 300 ms after;
- sites: the share of stimulated electrodes whose stims have an early
  answer of a spike or more on average.

    evoked.py culture.spk.txt.bz2 ...      the statistics of recordings, and
                                           their quartiles (into the file
                                           EVOKED_JSON names, if set)
"""

from __future__ import annotations

import bz2
import sys

import numpy as np

COLUMNS = ["early", "late", "channels", "big", "latency", "sites"]


def read_stim_culture(path: str):
    """A stimulus-evoked .spk.txt.bz2 and its .tri.txt.bz2: the spikes'
    times and channels, the stims' times and channels."""
    with bz2.open(path, "rt") as f:
        data = np.loadtxt(f, ndmin=2, usecols=(0, 1))
    keep = data[:, 1] < 60
    with bz2.open(path.replace(".spk.", ".tri."), "rt") as f:
        tri = np.loadtxt(f, ndmin=2)
    return data[keep, 0], data[keep, 1].astype(int), tri[:, 0], tri[:, 1].astype(int)


def evoked(t, ch, stim_t, stim_ch, channels: int | None = None) -> dict:
    order = np.argsort(t, kind="stable")
    t, ch = t[order], ch[order]
    channels = channels or int(max(ch.max(), stim_ch.max())) + 1
    span = float(stim_t[-1] - stim_t[0]) + 0.3
    rates = np.bincount(ch, minlength=channels) / span
    active = int((rates >= 0.1).sum())
    early, late, reach, total = [], [], [], []
    lat = []
    for s, c in zip(stim_t, stim_ch):
        lo, hi = np.searchsorted(t, [s + 0.003, s + 0.3])
        dt, dc = t[lo:hi] - s, ch[lo:hi]
        other = dc != c
        dt, dc = dt[other], dc[other]
        early.append(int((dt < 0.020).sum()))
        late.append(int((dt >= 0.020).sum()))
        reach.append(len(np.unique(dc[dt < 0.100])))
        total.append(len(dt))
        lat.append(dt)
    early, late, total = map(np.array, (early, late, total))
    lat = np.concatenate(lat) if lat else np.array([])
    per_site = [early[stim_ch == c].mean() for c in np.unique(stim_ch)]
    return {
        "stims": len(stim_t),
        "early": float(early.mean()),
        "late": float(late.mean()),
        "channels": float(np.median(reach)),
        "big": float((total >= 2 * max(active, 1)).mean()),
        "latency": float(np.median(lat) * 1000) if len(lat) else float("nan"),
        "sites": float(np.mean(np.array(per_site) >= 1.0)),
    }


def run_protocol(dish, rounds: int = 10, every_ms: int = 300, amount: float = 1.0):
    """The archive's protocol on a dish (ent_dish.Dish): a stim of `amount`
    on each of its 64 channels in turn, one every `every_ms`, `rounds`
    times round. The spikes' times and channels, the stims' times and
    channels (seconds)."""
    t, ch, st, sc = [], [], [], []
    for r in range(rounds):
        for c in range(64):
            st.append(dish.tick)
            sc.append(c)
            dish.stimulate(c, amount)
            for step, channel in dish.step(every_ms):
                t.append(step)
                ch.append(channel)
    return (np.array(t, float) / 1000, np.array(ch, int),
            np.array(st, float) / 1000, np.array(sc, int))


def record_dish(rounds: int = 10, settle_ms: int = 1000, **parameters):
    """A new dish of `parameters`, a second to settle, then the protocol."""
    import ent_dish
    dish = ent_dish.Dish(**parameters)
    dish.step(settle_ms)
    out = run_protocol(dish, rounds)
    dish.close()
    return out


def header() -> str:
    return f"{'':>14}" + "".join(f"{c[:9]:>10}" for c in COLUMNS)


def line(name: str, s: dict) -> str:
    return f"{name:>14}" + "".join(f"{s.get(c, float('nan')):>10.3g}" for c in COLUMNS)


if __name__ == "__main__":
    import json
    import os
    rows = {}
    print(header())
    for path in sys.argv[1:]:
        s = evoked(*read_stim_culture(path))
        name = os.path.basename(path).split(".")[0]
        rows[name] = s
        print(line(name, s))
    q = {c: [float(np.nanpercentile([r[c] for r in rows.values()], p)) for p in (25, 50, 75)]
         for c in COLUMNS}
    for i, p in enumerate(("25%", "median", "75%")):
        print(line(p, {c: q[c][i] for c in COLUMNS}))
    if os.environ.get("EVOKED_JSON"):
        with open(os.environ["EVOKED_JSON"], "w") as f:
            json.dump(q, f, indent=1)

