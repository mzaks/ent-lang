"""Heart rhythm through the dish: the control the CL1 AF study did not run.

scottpete.com/articles/biocomputing-neurons-vs-machine-learning (one CL1,
one culture, one 40-minute run, 10 Nov 2026) played 208 ten-second windows
of heart rhythm, half normal sinus rhythm (NSR) and half atrial
fibrillation (AF), into living neurons as pulse patterns, and read the
neurons' response in the second after each with logistic regression:
55.4% right from one example of each rhythm (silicon given the pulse
pattern itself 47.9%), 87.2% from all 83 (nearest centroid on the pulses,
added after the run, 87.2% too). The neurons stay as they are and a
linear readout learns: a reservoir. The question it leaves is whether
anything living mattered. Here the same design runs on the ent dish
(network.ent) with its learning off (stdp=0: a fixed reservoir, its
short-term depression the only memory) and on (stdp=1), and beside it
the baselines a reader would ask for.

What the article does not say, and what is chosen here:
  data      MIT-BIH Atrial Fibrillation Database (PhysioNet afdb, 25
            people, beat annotations .qrs, rhythm annotations .atr); the
            article's came from one person, its source unnamed.
  window    44 beats in a row inside one rhythm episode ((N or (AFIB),
            every gap 0.25-2 s (no missed beats), played as 44 pulses
            squeezed into 10 s: the article's windows had 43-45 pulses
            and were matched for rate and pulse count; here both match
            exactly, so only the rhythm's irregularity tells them apart.
  split     by recording, as theirs: per replicate the 25 recordings are
            shuffled and dealt into train and test until each side has
            its windows (41 + 42 train, 58 + 59 test, 8 warm-ups first),
            at most 6 windows per recording and rhythm.
  stim      each pulse a stim of 1 on the 8 middle electrodes (rows 3-4,
            columns 2-5); 10 s pattern, 1 s read, 0.5 s rest, the 208
            windows in a shuffled order, as one 40-minute session.
  readouts  after:  each channel's spikes in the second after (64)
            bins:   the same in four 250 ms bins (256)
            during: each channel's spikes during the pattern (64)
            echo:   all channels' spikes in 100 ms bins during it (100)
  pulses    the pattern itself, as the silicon got it: pulses in 100 ms
            bins (100); also the 43 gaps between beats (gaps), and the
            one number a cardiologist looks at, how much the gaps vary
            (spread: their standard deviation over their mean).
  models    logistic regression (features standardised on the training
            set, L2 as scikit-learn's C=1), nearest centroid, 1-nearest
            neighbour, a small network (one hidden layer of 32, Adam).
  sizes     1, 2, 4, 8, 16, 32 and all (41/42) of each rhythm, 200 random
            draws each, scored on the 117 test windows.

Results (24 Nov 2026, 8 replicates, each its own split and dish seed;
accuracy on the 117 test windows at 1 example of each rhythm -> all):
  their readout, the second after:   chance (0.50 -> 0.50), frozen or
      plastic: this dish keeps nothing of a pattern a second on (its
      short-term depression recovers within it); their neurons did.
  frozen dish, spikes per channel during the pattern: centroid 0.67 ->
      0.81, logistic 0.55 -> 0.70; the echo (100 ms bins) 0.60 -> 0.82
  plastic dish: the echo the same (0.59 -> 0.82), per channel during the
      pattern chance (0.50 -> 0.51): 40 minutes of STDP move the counts
      more than the rhythm does
  the pulses themselves (their silicon's input): logistic 0.60 -> 0.79,
      small network 0.60 -> 0.82, centroid 0.59 -> 0.82 (theirs 0.48 at
      1: below chance, a weak baseline)
  the gaps' spread, one number: 0.83 -> 0.89, ahead of everything, the
      CL1's 0.55 -> 0.87 too.
A fixed reservoir adds nothing over the pulses' own bins when all are
read; read as per-channel totals, its depression makes a nonlinear
feature that helps at one example (0.67 vs 0.59), as theirs may have.
Table: build/cl_bridge/heart/table.txt.

    python heart.py run REP COND        one session: REP the replicate
                                        (split and dish seed), COND frozen
                                        or plastic; to build/cl_bridge/heart/
    python heart.py analyse             the table, from all sessions there
"""

from __future__ import annotations

import json
import os
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = ROOT / "build" / "cl_bridge" / "heart"
DATA = Path(os.environ.get("AFDB", ROOT / "build" / "cl_bridge" / "afdb"))

BEATS = 44
SECONDS = 10.0
READ_MS, REST_MS = 1000, 500
ELECTRODES = (26, 27, 28, 29, 34, 35, 36, 37)
TRAIN = (41, 42)            # NSR, AF
TEST = (58, 59)
WARM = 8
PER_RECORD = 6
SIZES = (1, 2, 4, 8, 16, 32, 0)   # 0: all
DRAWS = 200


# --- PhysioNet annotations (MIT format) -------------------------------------

def read_annotations(path: Path):
    """(sample, code, aux) of each annotation in a WFDB annotation file."""
    data = path.read_bytes()
    out, at, t, i = [], 0, 0, 0
    while i + 1 < len(data):
        word = data[i] | data[i + 1] << 8
        i += 2
        code, value = word >> 10, word & 0x3FF
        if code == 0 and value == 0:
            break
        if code == 59:      # SKIP: a 32-bit interval, high word first
            hi, lo = struct.unpack_from("<HH", data, i)
            i += 4
            t += (hi << 16) | lo
        elif code == 63:    # AUX: text for the last annotation
            text = data[i:i + value].decode("latin-1").rstrip("\0")
            i += value + (value & 1)
            if out:
                out[-1] = (out[-1][0], out[-1][1], text)
        elif code in (60, 61, 62):
            pass            # NUM, SUB, CHAN
        else:
            t += value
            out.append((t, code, ""))
    return out


def windows():
    """Every window of each record: (record, rhythm 0 NSR / 1 AF, gaps in s)."""
    found = []
    for record in (DATA / "RECORDS").read_text().split():
        rate = float((DATA / f"{record}.hea").read_text().split()[2])
        beats = np.array([t for t, code, _ in read_annotations(DATA / f"{record}.qrs")
                          if 1 <= code <= 13 or code in (31, 34, 35, 38)], dtype=float) / rate
        episodes, current, since = [], None, 0.0
        for t, code, aux in read_annotations(DATA / f"{record}.atr"):
            if aux.startswith("("):
                if current is not None:
                    episodes.append((current, since, t / rate))
                current, since = aux[1:], t / rate
        if current is not None and len(beats):
            episodes.append((current, since, beats[-1] + 1))
        for rhythm, start, end in episodes:
            if rhythm not in ("N", "AFIB"):
                continue
            inside = beats[(beats > start) & (beats < end)]
            k = 0
            while k + BEATS <= len(inside):
                gaps = np.diff(inside[k:k + BEATS])
                if gaps.min() >= 0.25 and gaps.max() <= 2.0:
                    found.append((record, int(rhythm == "AFIB"), gaps))
                    k += BEATS
                else:
                    k += 1
    return found


def dataset(rep: int):
    """The replicate's 208 windows: (record, rhythm, gaps, side) in play order,
    side 0 warm-up, 1 train, 2 test."""
    rng = np.random.default_rng(1000 + rep)
    pool = windows()
    by = {}
    for record, rhythm, gaps in pool:
        by.setdefault((record, rhythm), []).append(gaps)
    records = sorted({r for r, _, _ in pool})
    rng.shuffle(records)
    chosen = []
    need = {(1, 0): TRAIN[0], (1, 1): TRAIN[1], (2, 0): TEST[0], (2, 1): TEST[1]}
    have = {k: 0 for k in need}
    # Deal recordings to train or test, whichever side still lacks most.
    for record in records:
        lack = {side: sum(need[(side, r)] - have[(side, r)] for r in (0, 1)) for side in (1, 2)}
        side = max(lack, key=lambda s: (lack[s] / sum(need[(s, r)] for r in (0, 1)), s))
        for rhythm in (0, 1):
            ws = by.get((record, rhythm), [])
            take = min(PER_RECORD, need[(side, rhythm)] - have[(side, rhythm)], len(ws))
            for j in rng.choice(len(ws), size=max(take, 0), replace=False) if take > 0 else []:
                chosen.append((record, rhythm, ws[j], side))
            have[(side, rhythm)] += max(take, 0)
    short = {k: need[k] - have[k] for k in need if have[k] < need[k]}
    if short:
        raise SystemExit(f"replicate {rep}: too few windows {short}")
    order = list(rng.permutation(len(chosen)))
    played = [chosen[i] for i in order]
    # Warm-ups: windows from the training recordings, not scored.
    train_records = sorted({r for r, _, _, s in chosen if s == 1})
    warm = []
    for i in range(WARM):
        record = train_records[i % len(train_records)]
        rhythm = i % 2
        ws = by.get((record, rhythm)) or by[(record, 1 - rhythm)]
        warm.append((record, rhythm if by.get((record, rhythm)) else 1 - rhythm,
                     ws[rng.integers(len(ws))], 0))
    return warm + played


def pulse_times(gaps):
    """The 44 pulses, ms from the window's start, squeezed into 10 s."""
    t = np.concatenate([[0.0], np.cumsum(gaps)])
    return np.round(t / t[-1] * (SECONDS * 1000 - 1)).astype(int)


# --- one session --------------------------------------------------------------

def run(rep: int, cond: str):
    import ent_dish
    d = ent_dish.Dish(seed=rep, axon=0.11, stdp=1 if cond == "plastic" else 0)
    d.step(2000)                               # settle
    OUT.mkdir(parents=True, exist_ok=True)
    out = open(OUT / f"{cond}_{rep}.jsonl", "w")
    for record, rhythm, gaps, side in dataset(rep):
        times = pulse_times(gaps)
        during = np.zeros(64, int)
        echo = np.zeros(100, int)
        now, start = 0, d.tick       # the spikes' steps are the dish's ticks

        for t in list(times) + [int(SECONDS * 1000)]:
            if t > now:
                for s, c in d.step(t - now):
                    during[c] += 1
                    echo[min((s - start) // 100, 99)] += 1
                now = t
            if t < SECONDS * 1000:
                for c in ELECTRODES:
                    d.stimulate(c, 1.0)
        bins = np.zeros((4, 64), int)
        start = d.tick
        for s, c in d.step(READ_MS):
            bins[min((s - start) // 250, 3), c] += 1
        d.step(REST_MS)
        out.write(json.dumps({"record": record, "rhythm": rhythm, "side": side,
                              "gaps": [round(g, 4) for g in gaps],
                              "during": during.tolist(), "echo": echo.tolist(),
                              "bins": bins.tolist()}) + "\n")
    out.close()


# --- models -------------------------------------------------------------------

def standardise(X, Y):
    mu, sd = X.mean(0), X.std(0)
    sd[sd == 0] = 1
    return (X - mu) / sd, (Y - mu) / sd


def logistic(X, y, T):
    from scipy.optimize import minimize
    X, T = standardise(X, T)
    Xb = np.hstack([X, np.ones((len(X), 1))])

    def loss(w):
        z = Xb @ w
        p = 1 / (1 + np.exp(-z))
        reg = 0.5 * (w[:-1] @ w[:-1])
        return np.sum(np.logaddexp(0, z) - y * z) + reg, Xb.T @ (p - y) + np.r_[w[:-1], 0]
    w = minimize(loss, np.zeros(Xb.shape[1]), jac=True, method="L-BFGS-B").x
    return (np.hstack([T, np.ones((len(T), 1))]) @ w > 0).astype(int)


def centroid(X, y, T):
    a, b = X[y == 0].mean(0), X[y == 1].mean(0)
    return (((T - b) ** 2).sum(1) < ((T - a) ** 2).sum(1)).astype(int)


def neighbour(X, y, T):
    d = ((T[:, None, :] - X[None, :, :]) ** 2).sum(2)
    return y[d.argmin(1)]


def network(X, y, T, seed=0):
    X, T = standardise(X, T)
    rng = np.random.default_rng(seed)
    h = 32
    W1 = rng.normal(0, 1 / np.sqrt(X.shape[1]), (X.shape[1], h)); b1 = np.zeros(h)
    W2 = rng.normal(0, 1 / np.sqrt(h), h); b2 = 0.0
    params = [W1, b1, W2, b2]
    m = [np.zeros_like(p) for p in params[:3]] + [0.0]
    v = [np.zeros_like(p) for p in params[:3]] + [0.0]
    for step in range(1, 301):
        H = np.maximum(X @ W1 + b1, 0)
        p = 1 / (1 + np.exp(-(H @ W2 + b2)))
        g = (p - y) / len(y)
        gW2 = H.T @ g + 1e-4 * W2; gb2 = g.sum()
        gH = np.outer(g, W2) * (H > 0)
        gW1 = X.T @ gH + 1e-4 * W1; gb1 = gH.sum(0)
        for k, grad in enumerate((gW1, gb1, gW2, gb2)):
            m[k] = 0.9 * m[k] + 0.1 * grad
            v[k] = 0.999 * v[k] + 0.001 * grad * grad
            params[k] = params[k] - 0.01 * (m[k] / (1 - 0.9 ** step)) / (np.sqrt(v[k] / (1 - 0.999 ** step)) + 1e-8)
        W1, b1, W2, b2 = params
    H = np.maximum(T @ W1 + b1, 0)
    return (H @ W2 + b2 > 0).astype(int)


MODELS = {"logistic": logistic, "centroid": centroid, "neighbour": neighbour, "network": network}


def features(rows):
    gaps = np.array([r["gaps"] for r in rows])
    pulses = np.zeros((len(rows), 100))
    for i, g in enumerate(gaps):
        for t in pulse_times(g):
            pulses[i, min(t // 100, 99)] += 1
    bins = np.array([r["bins"] for r in rows], float)
    return {
        "after": bins.sum(1),
        "bins": bins.reshape(len(rows), -1),
        "during": np.array([r["during"] for r in rows], float),
        "echo": np.array([r["echo"] for r in rows], float),
        "pulses": pulses,
        "gaps": gaps / gaps.mean(1, keepdims=True),
        "spread": (gaps.std(1) / gaps.mean(1))[:, None],
    }


def score(F, y, train, test, model, size, rng):
    if size == 0:
        picks = [train]
    else:
        picks = []
        for _ in range(DRAWS):
            idx = np.concatenate([rng.choice(train[y[train] == c], size=size, replace=False) for c in (0, 1)])
            picks.append(idx)
    acc = [np.mean(MODELS[model](F[i], y[i], F[test]) == y[test]) for i in picks]
    return float(np.mean(acc))


def analyse():
    runs = sorted(OUT.glob("*.jsonl"))
    if not runs:
        raise SystemExit(f"no sessions in {OUT}")
    results = {}
    for path in runs:
        cond, rep = path.stem.rsplit("_", 1)
        rows = [json.loads(l) for l in path.open()]
        y = np.array([r["rhythm"] for r in rows])
        side = np.array([r["side"] for r in rows])
        train, test = np.where(side == 1)[0], np.where(side == 2)[0]
        F = features(rows)
        sets = [("dish " + cond, k) for k in ("after", "bins", "during", "echo")]
        if cond == "frozen":    # the pattern's own features: the same windows either way
            sets += [("pattern", k) for k in ("pulses", "gaps", "spread")]
        for who, feat in sets:
            for model in MODELS:
                if feat == "spread" and model == "network":
                    continue
                for size in SIZES:
                    rng = np.random.default_rng(hash((rep, feat, model, size)) % 2**32)
                    acc = score(F[feat], y, train, test, model, size, rng)
                    results.setdefault((who, feat, model, size), {})[int(rep)] = acc
        print(f"{path.name}: done", file=sys.stderr, flush=True)
    (OUT / "results.json").write_text(json.dumps(
        [{"who": k[0], "feat": k[1], "model": k[2], "size": k[3], "acc": v} for k, v in results.items()]))
    table(results)


def table(results):
    print(f"accuracy on the 117 test windows (chance 0.50), mean over replicates +- s.e.; "
          f"sizes: examples of each rhythm, {DRAWS} draws each")
    head = "".join(f"{('all' if s == 0 else s):>11}" for s in SIZES)
    print(f"{'':34}{head}   n")
    for key in sorted({k[:3] for k in results}):
        cells = []
        for size in SIZES:
            v = np.array(list(results[key + (size,)].values()))
            se = v.std(ddof=1) / np.sqrt(len(v)) if len(v) > 1 else 0
            cells.append(f"{v.mean():.3f}+-{se:.3f}")
        print(f"{key[0] + ' ' + key[1] + ' ' + key[2]:34}" + "".join(f"{c:>11}" for c in cells) +
              f"   {len(results[key + (SIZES[0],)])}")


if __name__ == "__main__":
    if sys.argv[1] == "run":
        run(int(sys.argv[2]), sys.argv[3])
    elif sys.argv[1] == "analyse":
        analyse()
    elif sys.argv[1] == "windows":
        w = windows()
        print(len(w), "windows;", sum(r for _, r, _ in w), "AF;",
              len({rec for rec, _, _ in w}), "records")
        for rep in range(3):
            d = dataset(rep)
            print(rep, [sum(1 for x in d if x[3] == s and x[1] == r) for s in (0, 1, 2) for r in (0, 1)],
                  "test records", len({x[0] for x in d if x[3] == 2}))
