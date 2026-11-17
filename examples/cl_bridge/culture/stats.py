"""Statistics of a spike recording on a multi-electrode array, the same for
a culture's (Wagenaar, Pine and Potter 2006) and the dish's.

A recording is spike times (seconds) and channels. Of it:

- active channels: those firing at 0.1 Hz or more; their median rate;
- network bursts as Wagenaar, Pine and Potter (2006) found them (below),
  on at least 4 channels. Of them: how many a minute, their median length
  (ms) and size (spikes), the share of all
  spikes in them, the median share of active channels in one;
- the inter-burst interval's CV (its deviation over its mean: 0 for a
  clock, 1 for bursts at random), and the median channel's ISI CV.

    stats.py culture.spk.txt.bz2 ...     each recording's, and their quartiles
                                         (into the file STATS_JSON names, if set)
"""

from __future__ import annotations

import bz2
import numpy as np

CHANNELS_IN_BURST = 4
ACTIVE_HZ = 0.1


def read_culture(path: str) -> tuple[np.ndarray, np.ndarray]:
    """A .spk.txt.bz2 of the Potter lab's archive: time (s) and channel."""
    with bz2.open(path, "rt") as f:
        data = np.loadtxt(f, ndmin=2)
    keep = data[:, 1] < 60          # (60 marks stimuli; none in spontaneous)
    return data[keep, 0], data[keep, 1].astype(int)


def stats(t: np.ndarray, ch: np.ndarray, duration: float | None = None) -> dict:
    order = np.argsort(t, kind="stable")
    t, ch = t[order], ch[order]
    duration = duration or float(t[-1] - t[0])
    channels = int(ch.max()) + 1 if len(ch) else 0
    rates = np.bincount(ch, minlength=channels) / duration
    active = rates >= ACTIVE_HZ
    out = {
        "minutes": duration / 60,
        "spikes": len(t),
        "active": int(active.sum()),
        "rate_hz": float(np.median(rates[active])) if active.any() else 0.0,
    }
    # Channel ISI CV (the median of active channels).
    cvs = []
    for c in np.flatnonzero(active):
        isi = np.diff(t[ch == c])
        if len(isi) > 10:
            cvs.append(isi.std() / isi.mean())
    out["isi_cv"] = float(np.median(cvs)) if cvs else float("nan")
    # Network bursts, as Wagenaar, Pine and Potter (2006) found them: on
    # each channel, burstlets (a core of at least 4 spikes all closer than
    # min(1 / (4 f), 100 ms), f the channel's rate, taken on with the
    # spikes either side closer than min(1 / (3 f), 200 ms)); burstlets of
    # all channels that overlap in time, merged; on at least 4 channels.
    burstlets = []
    for c in np.flatnonzero(active):
        tc = t[ch == c]
        f = len(tc) / duration
        core, near = min(1 / (4 * f), 0.100), min(1 / (3 * f), 0.200)
        isi = np.diff(tc)
        tight = np.concatenate(([0], (isi < core).astype(int), [0]))
        e = np.diff(tight)
        for a, b in zip(np.flatnonzero(e == 1), np.flatnonzero(e == -1)):
            if b - a + 1 < 4:              # spikes a..b, b - a intervals
                continue
            while a > 0 and isi[a - 1] < near:
                a -= 1
            while b < len(isi) and isi[b] < near:
                b += 1
            burstlets.append((tc[a], tc[b], c, b - a + 1))
    burstlets.sort()
    bursts = []
    for start, end, c, n in burstlets:
        if bursts and start <= bursts[-1][1]:
            last = bursts[-1]
            last[1] = max(last[1], end)
            last[2].add(c)
        else:
            bursts.append([start, end, {c}])
    found = []
    for start, end, used in bursts:
        if len(used) >= CHANNELS_IN_BURST:
            lo, hi = np.searchsorted(t, start), np.searchsorted(t, end, side="right")
            found.append((start, end, hi - lo, len(used)))
    bursts = found
    n = len(bursts)
    out["bursts_per_min"] = n / (duration / 60)
    if n:
        b = np.array(bursts, dtype=float)
        out["burst_ms"] = float(np.median((b[:, 1] - b[:, 0]) * 1000))
        out["burst_spikes"] = float(np.median(b[:, 2]))
        out["in_bursts"] = float(b[:, 2].sum() / len(t))
        out["recruited"] = float(np.median(b[:, 3] / max(active.sum(), 1)))
        ibi = np.diff(b[:, 0])
        out["ibi_cv"] = float(ibi.std() / ibi.mean()) if len(ibi) > 2 else float("nan")
    else:
        out.update(burst_ms=0.0, burst_spikes=0.0, in_bursts=0.0, recruited=0.0,
                   ibi_cv=float("nan"))
    return out


COLUMNS = ["active", "rate_hz", "isi_cv", "bursts_per_min", "burst_ms",
           "burst_spikes", "in_bursts", "recruited", "ibi_cv"]


def line(name: str, s: dict) -> str:
    return f"{name:>10} " + " ".join(f"{s[k]:>9.3g}" for k in COLUMNS)


def header() -> str:
    return f"{'':>10} " + " ".join(f"{k[:9]:>9}" for k in COLUMNS)


if __name__ == "__main__":
    import sys
    rows = []
    print(header())
    for path in sys.argv[1:]:
        s = stats(*read_culture(path))
        rows.append(s)
        print(line(path.split("/")[-1].split(".")[0], s))
    if len(rows) > 1:
        q = {k: np.nanpercentile([r[k] for r in rows], [25, 50, 75]) for k in COLUMNS}
        for i, name in enumerate(["25%", "median", "75%"]):
            print(line(name, {k: q[k][i] for k in COLUMNS}))
        import json
        import os
        if os.environ.get("STATS_JSON"):
            with open(os.environ["STATS_JSON"], "w") as f:
                json.dump({k: [float(x) for x in q[k]] for k in COLUMNS}, f, indent=1)
