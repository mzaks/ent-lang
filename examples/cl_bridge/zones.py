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

    ... zones.py longer [seeds (16)] [rallies (120)] [out.json]

the relative code, aligned layout, decoder 1 frozen after 5, 10, 20 and
40 minutes of training (16 seeds) and 60 (half of them): does it go on
learning after 20?

22 November 2026, 120 serves: all rows 0.65 at minute 5, 0.67 at 10, 0.71
at 20, 0.64 at 40 (16 seeds), 0.70 at 60 (8). Minute 20 less 10 +0.03 +-
0.03 (10 of 16 better), 40 less 20 -0.07 +- 0.03 (5 of 16), 60 less 40
+0.04 +- 0.04, 60 less 20 -0.02 +- 0.03 (3 of 8). Most is learnt by
minute 5 (no reward at 20: 0.49); after that the score drifts around 0.67
and longer training does not add to it. Every row stays 0.55-0.82.

    ... zones.py autopsy [seeds (16)] [rallies (120)] [out.json]

what limits the best setup: the same serves steered by game.c's
pong_oracle (1 a perfect follower of the ball's height, 2 a perfect
predictor of where it arrives, 3 the last stim's row alone, what the
relative code tells) as ceilings, and the dish trained 20 minutes and
untrained, each miss sorted by what the paddle did in its last second:
parked at an edge, still, dithering (4 or more turns), the wrong way, the
right way but the ball bounced off a wall, the right way but too late.

23 November 2026: every oracle hits every ball (half an hour of the
serves each, 4 seeds), the rows-only one too: the code, the paddle's
speed and the stims' timing allow it all; the gap is the dish's
steering. The dish trained 20 minutes hits 0.71 (16 seeds), untrained
0.34 (8); its misses: dithering 0.63 (untrained 0.52), parked 0.13
(0.01), still 0.12 (0.10), wrong way 0.08 (0.24), late 0.04, bounce
0.01. A dithering miss turns 7.3 times in its last second, gets 0.17
of the way, and the ball arrives 0.37 from the paddle (more than a row
of the code away, so the stims all said the same way). Training cuts
the wrong way; it leaves the paddle unable to hold a way.

    ... zones.py steady [seeds (16)] [rallies (120)] [out.json]

against the dithering the autopsy found: decoder 3 (game.c: decoder 1,
but only what each region fires above its background counts) and
hysteresis (pong_hold 0.3: the paddle turns back only when the other
region leads by 0.3 of the larger, not 0.1), alone and together, in
training and in the test; the autopsy's decoder 1 dishes for comparison.

23 November 2026, 16 seeds, 120 serves: decoder 1 0.71 (dithering 0.63
of the misses, 5.0 turns a second in a miss's last), decoder 3 0.65
(dithering 0.28; still 0.26, late 0.22), hold 0.3 0.66 (dithering 0.20;
wrong way 0.32, parked 0.21), both 0.55 (dithering 0.04; late 0.25,
still 0.26, wrong way 0.26). Each takes the dithering away and scores
worse (-0.05, -0.04, -0.16 +- 0.05): the misses move to a paddle that
holds the wrong way or does not go. The turning was not the flaw but
the sign of one: the dish's answer to a stim is too weak against its
noise to give a way for long, and turning often was what let it still
get there.

    ... zones.py yoked [seeds (16)] [rallies (120)] [out.json]

the open-loop control for plain STDP (dishbrain's `none`, which learns:
0.36 to 0.65): each dish gets, tick by tick, the very stims a closed-loop
dish of another seed (seed + 1000) gets while it plays, so the same kind
of input, but its own spikes move nothing; frozen at minute 20 and scored
against dishbrain.jsonl's closed loop and untrained. Does plain STDP
learn from the loop, or from the input and the layout alone?

23 November 2026, 16 seeds, 120 serves, all rows: untrained 0.36, open
loop 0.61, closed loop 0.65. Open loop less untrained +0.25 +- 0.03 (16 of
16 seeds); closed less open +0.03 +- 0.04 (10 of 16). Plain STDP learns
from the input on the aligned layout, nearly as much without the loop as
with it: it strengthens what each sensory electrode already reaches (the
layout puts the right region nearer), not what worked. The loop adds
little if anything; the reward (0.71) a little more.

    ... zones.py motor [seeds (16)] [rallies (120)] [out.json]

the motor regions one column each instead of two (game.c's pong_motor):
1 the inner columns (1 up, 6 down, nearer the sensory electrodes), 2 the
outer (0, 7), against both (decoders.jsonl), on the best setup (relative
code, aligned layout, decoder 1), with the reward and without (half the
seeds).

23 November 2026, 16 seeds, 120 serves, all rows: both columns 0.71
(no reward 0.49), inner 0.68 (0.43), outer 0.66 (0.44). One column is
no better, inner or outer (-0.03 +- 0.04, -0.05 +- 0.04), and the reward
adds the same to each (+0.23, +0.25, +0.23). The inner one leans: the
rows above the paddle 0.70-0.85, those well below 0.46-0.63. The size of
the motor regions is not what limits the dish.

    ... zones.py dishbrain [seeds (16)] [rallies (120)] [out.json]

DishBrain's feedback (game.c's cond 2 `stim`: a hit, the 8 sensory
electrodes at 100 Hz for 100 ms; a miss, 4 s of stims on random
electrodes at 5 Hz and a pause) against its controls (3 `silent`, 4
`none`) on the best setup (relative code, aligned layout, decoder 1),
plain STDP (no dopamine, as DishBrain's cultures had no reward channel),
20 minutes, frozen and scored as above; and the dish untrained.
(Run with ENT_DISH_LIBRARY while another study holds libdish.so.)

22 November 2026, 16 seeds, 120 serves, all rows: untrained 0.36, stim
0.60, silent 0.63, none 0.65 (with dopamine, 0.71; no reward, only the
scaling, 0.49). Plain STDP in the loop learns on this setup whatever the
feedback (none less untrained +0.28 +- 0.03, 16 of 16 seeds); the
feedback adds nothing to it: stim less silent -0.03 +- 0.03 (7 of 16),
less none -0.05 +- 0.03 (5 of 16). As on the old layout (feedback_study.py),
the model has no term for the feedback's predictability to act on.

    ... zones.py settle [seeds (16)] [rallies (120)] [out.json]

the same setup with the reward gain and the neurons' scaling (home_gain)
slowed together from the start, no best minute picked (resume.py's "both
halving", there begun at minute 20): h2 halved every 2 minutes, h4 every
4, 5h2 the full rate for 5 minutes and then halved every 2; frozen at
minute 20 (5h2 also 40), against the constant rate (longer.jsonl).

22 November 2026, 16 seeds, 120 serves: all rows h2 0.68, h4 0.70, 5h2
0.70 at minute 20 and 0.69 at 40, against the constant rate's 0.71 and
0.64. No schedule beats the constant rate at 20 (h4 and 5h2 -0.00 +-
0.03, h2 -0.03); what slowing does is hold: 5h2 from minute 20 to 40
-0.01 +- 0.01 (the constant rate -0.07 +- 0.03), and the seeds closer
together (s.e. 0.02 against 0.04). The map learnt in the first minutes
is kept, not bettered: as from the best minute (resume.py), slowing ends
the drift but does not lift the ceiling.

    ... zones.py two [seeds (16)] [rallies (120)] [out.json]

the relative code with decoder 1 on the aligned layout (1, from
decoders.jsonl) and on the same with two electrodes a row (2).

22 November 2026, 16 seeds: one electrode a row 0.71 (no reward 0.49),
two 0.64 (no reward 0.57); worse for 11 of 16 seeds (-0.07 +- 0.05).
The two give a stronger map from the start (wiring up less down about
1.3 times one's) but the reward adds only +0.08 to it.

    ... zones.py decoders [seeds (16)] [rallies (120)] [out.json] [minute (20)]

the relative code on the aligned layout with game.c's pong_decoder 0, 1
and 2 (as before; each motor region against its own background; only
the spikes just after a ball stim), in training and in the test, with
the reward and (half the seeds) without. Decoder 0 at minute 20 is taken
from layouts.jsonl where it is there.

22 November 2026, 16 seeds (8 without reward), the share of balls hit:
    decoder 0  0.54 (no reward 0.42)
    decoder 1  0.71 (no reward 0.49), rows 3-4 0.82; better than 0 for 11
               of 16 seeds (+0.17 +- 0.05), than no reward for 8 of 8
               (+0.23 +- 0.05)
    decoder 2  0.66 (no reward 0.52)
Against its background a region's answer to the ball is what moves the
paddle, not how much it fires anyway; the reward for each move then
judges the map, and the map learnt is larger.

    ... zones.py layouts [seeds (16)] [rallies (120)] [out.json]

both codes on the layout as before and the aligned one (game.c's
pong_layout: rows 0-3 on column 3, nearer the up regions, 4-7 on column
4), 20 minutes of training, frozen, scored by row. Each result is kept in
out.jsonl as it comes.

22 November 2026, 16 seeds, the share of balls hit (all rows):
    height code    layout as before 0.52, aligned 0.52 (no reward 0.43)
    relative code  layout as before 0.42, aligned 0.54 (no reward 0.42)
The relative code on the aligned layout hits 0.49-0.70 in every row, the
middle included (rows 3-4 0.49, 0.51; height code 0.17, 0.14 on either
layout); better than its old layout for 11 of 16 seeds (+0.12 +- 0.07),
and than no reward for 6 of 8 (+0.16 +- 0.07): the layout does not hand
it the game, it lets the reward teach it (row 4, ball just below the
paddle, down: -0.13 to -0.30). The height code's middle stays unlearnt.

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
SENSORY_ALIGNED = [3, 11, 19, 27, 36, 44, 52, 60]   # game.c's pong_layout(1)
SENSORY_TWO = [[3, 11], [19, 27], [35, 43], [51, 59], [4, 12], [20, 28], [36, 44], [52, 60]]   # (2)
POINTS = [(1.0, 0), (1.0, 5), (1.0, 10), (1.0, 20), (0.0, 20)]


def _home_gain() -> float:
    import ent_dish
    p = ent_dish.DishParams()
    ent_dish._load(None).dish_defaults(ctypes.byref(p))
    return p.home_gain


def slowing(schedule: str, m: int) -> float:
    """The share of the learning rate and of the scaling in minute m:
    h2 halved every 2 minutes from the start, h4 every 4, 5h2 the full
    rate for 5 minutes and then halved every 2."""
    if schedule == "h2":
        return 0.5 ** (m // 2)
    if schedule == "h4":
        return 0.5 ** (m // 4)
    if schedule == "5h2":
        return 1.0 if m < 5 else 0.5 ** ((m - 5) // 2 + 1)
    raise ValueError(schedule)


def job(seed: int, gain: float, minute: int, rallies: int, ego: int = 0,
        far: float = 0.0, layout: int = 0, decoder: int = 0, schedule: str = "", cond: int = 0,
        motor: int = 0) -> dict:
    lib = fe._lib()
    lib.dish_set_reward_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.dish_set_home_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]
    home = _home_gain() if schedule else 0.0
    lib.pong_layout.argtypes = [ctypes.c_int32]
    lib.pong_layout(layout)
    electrodes = (SENSORY_TWO if layout == 2 else
                  [[c] for c in (SENSORY_ALIGNED if layout else SENSORY)])
    lib.pong_zones.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dish_paths.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    state = (ctypes.c_double * 12)()
    dish = lib.pong_make_dish(seed, gain, cond)
    game = lib.pong_create(dish, seed, cond, ego)
    if far:
        lib.pong_far_serves.argtypes = [ctypes.c_void_p, ctypes.c_double]
        lib.pong_far_serves(game, far)
    lib.pong_decoder.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.pong_motor.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    if decoder:
        lib.pong_decoder(game, decoder)
    lib.pong_motor(game, motor)
    for m in range(minute):
        if schedule:
            f = slowing(schedule, m)
            lib.dish_set_reward_gain(dish, gain * f)
            lib.dish_set_home_gain(dish, home * f)
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
    lib.pong_destroy(game)
    lib.dish_set_stdp(dish, 0)
    w = (ctypes.c_float * 4096)()
    n = (ctypes.c_float * 4096)()
    lib.dish_paths(dish, w, n)
    wiring = []
    for cs in electrodes:
        up = sum(w[c * 64 + b] for c in cs for b in range(64) if b % 8 <= 1)
        down = sum(w[c * 64 + b] for c in cs for b in range(64) if b % 8 >= 6)
        wiring.append(up - down)
    test = lib.pong_create(dish, EVAL_SEED, 4 if cond >= 2 else 0, ego)
    if decoder:
        lib.pong_decoder(test, decoder)
    lib.pong_motor(test, motor)
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
    return {"seed": seed, "gain": gain, "minute": minute, "ego": ego, "far": far, "layout": layout,
            "decoder": decoder, "schedule": schedule, "cond": cond, "motor": motor,
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


def layouts_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/layouts.json"
    done_path = Path(out + "l")
    results = []
    if done_path.exists():
        results = [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["gain"], r["ego"], r["layout"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, e, 0.0, lay) for e in (0, 1) for lay in (0, 1)
            for s in range(1, seeds + 1)]
    # The aligned layout's own map, with no reward, for half the seeds.
    jobs += [(s, 0.0, 20, rallies, e, 0.0, 1) for e in (0, 1) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[1], j[4], j[6]) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"20 minutes of training, frozen, {rallies} uniform fixed serves, {seeds} seeds; "
          "layout 1: rows 0-3 on column 3 (up side), 4-7 on column 4")
    print("hit rate by court row        " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    for ego in (0, 1):
        for lay, gain in ((0, 1.0), (1, 1.0), (1, 0.0)):
            rs = [r for r in results if r["ego"] == ego and r["layout"] == lay and r["gain"] == gain]
            seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
            hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
            each = [score(r) for r in rs]
            se = statistics.stdev(each) / len(each) ** 0.5
            name = f"{'relative' if ego else 'height'} layout {lay}" + ("" if gain else " no reward")
            print(f"{name:28s} " + "  ".join(f"{h / s:5.2f}" for h, s in zip(hit, seen)) +
                  f"   {mean(each):5.2f}  {se:.2f}")
        a = {r["seed"]: score(r) for r in results if r["ego"] == ego and r["layout"] == 0 and r["gain"]}
        b = {r["seed"]: score(r) for r in results if r["ego"] == ego and r["layout"] == 1 and r["gain"]}
        d = [b[s] - a[s] for s in a]
        print(f"{'  aligned less as before':28s} {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, "
              f"better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    print("wiring up less down")
    for ego in (0, 1):
        for lay, gain in ((0, 1.0), (1, 1.0), (1, 0.0)):
            rs = [r for r in results if r["ego"] == ego and r["layout"] == lay and r["gain"] == gain]
            name = f"{'relative' if ego else 'height'} layout {lay}" + ("" if gain else " no reward")
            print(f"{name:28s} " + "  ".join(f"{mean(r['wiring'][k] for r in rs):5.2f}" for k in range(8)))
    return 0


def decoders_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/decoders.json"
    minute = int(args[3]) if len(args) > 3 else 20
    done_path = Path(out + "l")
    results = []
    # Decoder 0 on the aligned layout: what `layouts` found, where it has it.
    old = Path("build/cl_bridge/layouts.jsonl")
    if minute == 20 and old.exists():
        results += [r for r in map(json.loads, old.read_text().splitlines())
                    if r["ego"] == 1 and r["layout"] == 1]
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    for r in results:
        r.setdefault("decoder", 0)
    have = {(r["seed"], r["gain"], r["decoder"]) for r in results}
    jobs = [(s, 1.0, minute, rallies, 1, 0.0, 1, d) for d in (0, 1, 2) for s in range(1, seeds + 1)]
    jobs += [(s, 0.0, minute, rallies, 1, 0.0, 1, d) for d in (0, 1, 2) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[1], j[7]) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"relative code, aligned layout, {minute} minutes of training, frozen, {rallies} uniform "
          f"fixed serves; decoder 0 as before, 1 against each region's background, 2 evoked only")
    print("hit rate by court row     " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    for d in (0, 1, 2):
        for gain in (1.0, 0.0):
            rs = [r for r in results if r["decoder"] == d and r["gain"] == gain]
            if not rs:
                continue
            seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
            hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
            each = [score(r) for r in rs]
            se = statistics.stdev(each) / len(each) ** 0.5 if len(each) > 1 else 0.0
            name = f"decoder {d}" + ("" if gain else " no reward")
            print(f"{name:25s} " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
                  f"   {mean(each):5.2f}  {se:.2f}  ({len(rs)})")
    base = {r["seed"]: score(r) for r in results if r["decoder"] == 0 and r["gain"] == 1.0}
    for d in (1, 2):
        got = {r["seed"]: score(r) for r in results if r["decoder"] == d and r["gain"] == 1.0}
        dd = [got[s] - base[s] for s in got if s in base]
        if len(dd) > 1:
            print(f"decoder {d} less decoder 0: {mean(dd):+.2f} +- {statistics.stdev(dd) / len(dd) ** 0.5:.2f}, "
                  f"better for {sum(x > 0 for x in dd)} of {len(dd)} seeds")
        nr = {r["seed"]: score(r) for r in results if r["decoder"] == d and r["gain"] == 0.0}
        dl = [got[s] - nr[s] for s in nr if s in got]
        if len(dl) > 1:
            print(f"decoder {d} reward less none: {mean(dl):+.2f} +- {statistics.stdev(dl) / len(dl) ** 0.5:.2f}, "
                  f"better for {sum(x > 0 for x in dl)} of {len(dl)} seeds")
    return 0


def two_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/two.json"
    done_path = Path(out + "l")
    results = []
    old = Path("build/cl_bridge/decoders.jsonl")
    if old.exists():
        results += [r for r in map(json.loads, old.read_text().splitlines()) if r["decoder"] == 1]
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["gain"], r["layout"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, 1, 0.0, lay, 1) for lay in (1, 2) for s in range(1, seeds + 1)]
    jobs += [(s, 0.0, 20, rallies, 1, 0.0, lay, 1) for lay in (1, 2) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[1], j[6]) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"relative code, decoder 1, 20 minutes of training, frozen, {rallies} uniform fixed serves; "
          "layout 1 an electrode a row, 2 two")
    print("hit rate by court row     " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    for lay in (1, 2):
        for gain in (1.0, 0.0):
            rs = [r for r in results if r["layout"] == lay and r["gain"] == gain]
            seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
            hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
            each = [score(r) for r in rs]
            se = statistics.stdev(each) / len(each) ** 0.5
            name = f"layout {lay}" + ("" if gain else " no reward")
            print(f"{name:25s} " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
                  f"   {mean(each):5.2f}  {se:.2f}  ({len(rs)})")
    a = {r["seed"]: score(r) for r in results if r["layout"] == 1 and r["gain"] == 1.0}
    b = {r["seed"]: score(r) for r in results if r["layout"] == 2 and r["gain"] == 1.0}
    n = {r["seed"]: score(r) for r in results if r["layout"] == 2 and r["gain"] == 0.0}
    d = [b[s] - a[s] for s in b if s in a]
    print(f"two less one: {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, better for "
          f"{sum(x > 0 for x in d)} of {len(d)} seeds")
    d = [b[s] - n[s] for s in n if s in b]
    print(f"two, reward less none: {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, better for "
          f"{sum(x > 0 for x in d)} of {len(d)} seeds")
    print("wiring up less down")
    for lay in (1, 2):
        rs = [r for r in results if r["layout"] == lay and r["gain"] == 1.0]
        print(f"layout {lay:<18d} " + "  ".join(f"{mean(r['wiring'][k] for r in rs):5.2f}" for k in range(8)))
    return 0


CAUSES = ("parked", "still", "dithering", "wrong way", "bounce", "late")


def autopsy_job(seed: int, minute: int, rallies: int, oracle: int = 0, decoder: int = 1,
                hold: float = 0.0) -> dict:
    """The best setup (relative code, aligned layout, decoder 1, the reward
    for each move), trained `minute` minutes, frozen, played on the fixed
    serves tick by tick; each miss sorted by what the paddle did in the
    last second before it (CAUSES). oracle: game.c's pong_oracle steers in
    the test instead of the dish."""
    lib = fe._lib()
    lib.pong_layout.argtypes = [ctypes.c_int32]
    lib.pong_layout(1)
    lib.pong_decoder.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.pong_oracle.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.pong_hold.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.pong_zones.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    dish = lib.pong_make_dish(seed, 1.0, 0)
    game = lib.pong_create(dish, seed, 0, 1)
    lib.pong_decoder(game, decoder)
    lib.pong_hold(game, hold)
    for _ in range(minute):
        if lib.pong_run(game, 60 * fe.TICKS, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
    lib.pong_destroy(game)
    lib.dish_set_stdp(dish, 0)
    test = lib.pong_create(dish, EVAL_SEED, 0, 1)
    lib.pong_decoder(test, decoder)
    lib.pong_hold(test, hold)
    lib.pong_oracle(test, oracle)
    lib.pong_fixed_serves(test, SERVES)
    state = (ctypes.c_double * 12)()
    window = fe.TICKS            # a second
    trail: list[tuple[float, float]] = []
    causes = dict.fromkeys(CAUSES, 0)
    misses = []
    edge_lo, edge_hi = 0.15 + 1e-6, 0.85 - 1e-6
    lib.pong_state(test, state)
    seen = int(state[4])
    while seen < rallies and state[0] < 1800:
        if lib.pong_run(test, 1, None, None) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(test, state)
        if int(state[4]) > seen:
            seen = int(state[4])
            w = trail[-window:]
            ys, ps = [a for a, _ in w], [b for _, b in w]
            y_end, p0, p1 = ys[-1], ps[0], ps[-1]
            moves = [b - a for a, b in zip(ps, ps[1:]) if abs(b - a) > 1e-9]
            flips = sum((a > 0) != (b > 0) for a, b in zip(moves, moves[1:]))
            dys = [b - a for a, b in zip(ys, ys[1:]) if abs(b - a) > 1e-9]
            bounced = any((a > 0) != (b > 0) for a, b in zip(dys, dys[1:]))
            dp, need = p1 - p0, y_end - p0
            if all(q <= edge_lo for q in ps) or all(q >= edge_hi for q in ps):
                cause = "parked"
            elif abs(dp) < 0.05 and flips < 4:
                cause = "still"
            elif flips >= 4:
                cause = "dithering"
            elif abs(y_end - p1) > abs(need):   # ended further from where it arrived
                cause = "wrong way"
            elif bounced:
                cause = "bounce"
            else:
                cause = "late"
            causes[cause] += 1
            misses.append({"cause": cause, "off": y_end - p1, "flips": flips, "moved": dp})
            trail = []
            continue
        trail.append((state[2], state[3]))
    z = (ctypes.c_int32 * 16)()
    lib.pong_zones(test, z)
    lib.pong_destroy(test)
    lib.dish_destroy(dish)
    return {"seed": seed, "minute": minute, "oracle": oracle, "decoder": decoder, "hold": hold,
            "rallies": seen,
            "seen": list(z[:8]), "hit": list(z[8:]), "causes": causes, "misses": misses}


def steady_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/steady.json"
    done_path = Path(out + "l")
    results = []
    old = Path("build/cl_bridge/autopsy.jsonl")
    if old.exists():
        results += [r for r in map(json.loads, old.read_text().splitlines())
                    if r["minute"] == 20 and r["oracle"] == 0]
    for r in results:
        r.setdefault("decoder", 1)
        r.setdefault("hold", 0.0)
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    ways = [(1, 0.0), (3, 0.0), (1, 0.3), (3, 0.3)]
    have = {(r["seed"], r["decoder"], r["hold"]) for r in results}
    jobs = [(s, 20, rallies, 0, d, h) for d, h in ways[1:] for s in range(1, seeds + 1)
            if (s, d, h) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(autopsy_job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"the dish's steering: relative code, aligned layout, 20 minutes with the reward; frozen, "
          f"{rallies} uniform fixed serves; misses by cause (share)")
    print(f"{'decoder, hold':24s} {'hit':>5s} {'s.e.':>5s}  " + "  ".join(f"{c:>9s}" for c in CAUSES) +
          "  turns/s")
    names = {(1, 0.0): "1 (background)", (3, 0.0): "3 (rectified)", (1, 0.3): "1, hold 0.3",
             (3, 0.3): "3, hold 0.3"}
    by = {}
    for w in ways:
        rs = sorted((r for r in results if (r["decoder"], r["hold"]) == w and r["seed"] <= seeds),
                    key=lambda r: r["seed"])
        if len(rs) < 2:
            continue
        by[w] = {r["seed"]: score(r) for r in rs}
        total = sum(sum(r["causes"].values()) for r in rs)
        share = [sum(r["causes"][c] for r in rs) / total if total else 0 for c in CAUSES]
        each = list(by[w].values())
        flips = [m["flips"] for r in rs for m in r["misses"]]
        print(f"{names[w]:24s} {mean(each):5.2f} {statistics.stdev(each) / len(each) ** 0.5:5.2f}  " +
              "  ".join(f"{x:9.2f}" for x in share) + f"  {mean(flips) if flips else 0:7.1f}  ({len(rs)})")
    for w in ways[1:]:
        if w in by and (1, 0.0) in by:
            d = [by[w][s] - by[1, 0.0][s] for s in by[w] if s in by[1, 0.0]]
            print(f"{names[w]} less decoder 1: {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, "
                  f"better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    return 0


def autopsy_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/autopsy.json"
    done_path = Path(out + "l")
    results = []
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["minute"], r["oracle"]) for r in results}
    jobs = [(s, 0, rallies, o) for o in (1, 2, 3) for s in range(1, 5)]
    jobs += [(s, 20, rallies, 0) for s in range(1, seeds + 1)]
    jobs += [(s, 0, rallies, 0) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[1], j[3]) not in have]
    jobs.sort(key=lambda j: -j[1])
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(autopsy_job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"relative code, aligned layout, decoder 1; {rallies} uniform fixed serves. Misses by what "
          f"the paddle did in the last second")
    print(f"{'who steers':34s} {'hit':>5s}  " + "  ".join(f"{c:>9s}" for c in CAUSES) + "   (share of misses)")
    rows = (("perfect predictor (oracle 2)", 0, 2), ("perfect follower (oracle 1)", 0, 1),
            ("rows only, as told (oracle 3)", 0, 3), ("dish, trained 20 min", 20, 0),
            ("dish, untrained", 0, 0))
    for label, m, o in rows:
        rs = [r for r in results if r["minute"] == m and r["oracle"] == o and r["seed"] <= seeds]
        if not rs:
            continue
        total = sum(sum(r["causes"].values()) for r in rs)
        share = [sum(r["causes"][c] for r in rs) / total if total else 0 for c in CAUSES]
        print(f"{label:34s} {mean(score(r) for r in rs):5.2f}  " +
              "  ".join(f"{x:9.2f}" for x in share) + f"   ({len(rs)})")
    rs = [r for r in results if r["minute"] == 20 and r["oracle"] == 0]
    if rs:
        print("dish, trained: by cause, mean |ball less paddle| at the miss, flips, |moved| in the last second")
        for c in CAUSES:
            ms = [m for r in rs for m in r["misses"] if m["cause"] == c]
            if ms:
                print(f"  {c:10s} {len(ms):5d}  off {mean(abs(m['off']) for m in ms):.2f}  "
                      f"flips {mean(m['flips'] for m in ms):5.1f}  moved {mean(abs(m['moved']) for m in ms):.2f}")
    return 0


def yoked_job(seed: int, minute: int, rallies: int, yoke: int = 1000) -> dict:
    """Plain STDP (cond 4, `none`) on the best setup, open loop: dish `seed`
    gets, tick by tick, the very stims that the closed-loop game of dish
    seed + yoke gets while it plays, and its own spikes move nothing. Then
    frozen and scored as job() does."""
    lib = fe._lib()
    lib.pong_layout.argtypes = [ctypes.c_int32]
    lib.pong_layout(1)
    lib.pong_decoder.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.pong_zones.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dish_stimulate.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_float]
    lib.dish_step.restype = ctypes.c_int64
    lib.dish_step.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.dish_spikes.restype = ctypes.c_int64
    lib.dish_spikes.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int64]
    leader_dish = lib.pong_make_dish(seed + yoke, 1.0, 4)
    leader = lib.pong_create(leader_dish, seed + yoke, 4, 1)
    lib.pong_decoder(leader, 1)
    dish = lib.pong_make_dish(seed, 1.0, 4)
    stims = (ctypes.c_int32 * 64)()
    ch, st = (ctypes.c_int32 * 4096)(), (ctypes.c_int32 * 4096)()
    sent = 0
    for _ in range(minute * 60 * fe.TICKS):
        ctypes.memset(stims, 0, 256)
        if lib.pong_run(leader, 1, None, stims) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        for c in range(64):
            if stims[c]:
                lib.dish_stimulate(dish, c, 1.0)
                sent += 1
        if lib.dish_step(dish, 5) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        while lib.dish_spikes(dish, ch, st, 4096) > 0:
            pass
    lib.pong_destroy(leader)
    lib.dish_destroy(leader_dish)
    lib.dish_set_stdp(dish, 0)
    state = (ctypes.c_double * 12)()
    test = lib.pong_create(dish, EVAL_SEED, 4, 1)
    lib.pong_decoder(test, 1)
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
    return {"seed": seed, "minute": minute, "yoke": yoke, "stims": sent, "rallies": int(state[4]),
            "seen": list(z[:8]), "hit": list(z[8:])}


def yoked_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/yoked.json"
    done_path = Path(out + "l")
    results = []
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {r["seed"] for r in results}
    jobs = [(s, 20, rallies) for s in range(1, seeds + 1) if s not in have]
    fe._lib()
    with ProcessPoolExecutor(6) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(yoked_job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    closed = {}
    old = Path("build/cl_bridge/dishbrain.jsonl")
    if old.exists():
        closed = [r for r in map(json.loads, old.read_text().splitlines()) if r["cond"] == 4]
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"plain STDP, best setup: closed loop against open loop (yoked to seed + 1000's stims); "
          f"frozen, {rallies} uniform fixed serves")
    print("                       " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    by = {}
    for label, rs in (("untrained", [r for r in closed if r["minute"] == 0]),
                      ("closed loop m20", [r for r in closed if r["minute"] == 20]),
                      ("open loop m20", results)):
        rs = sorted((r for r in rs if r["seed"] <= seeds), key=lambda r: r["seed"])
        if len(rs) < 2:
            continue
        by[label] = {r["seed"]: score(r) for r in rs}
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        each = list(by[label].values())
        print(f"{label:22s} " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
              f"   {mean(each):5.2f}  {statistics.stdev(each) / len(each) ** 0.5:.2f}  ({len(rs)})")
    for a, b in (("untrained", "open loop m20"), ("open loop m20", "closed loop m20")):
        if a in by and b in by:
            d = [by[b][s] - by[a][s] for s in by[b] if s in by[a]]
            print(f"{b} less {a}: {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, "
                  f"better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    print(f"stims a minute, open loop: {mean(r['stims'] for r in results) / 20:.0f}")
    return 0


def motor_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/motor.json"
    done_path = Path(out + "l")
    results = []
    old = Path("build/cl_bridge/decoders.jsonl")
    if old.exists():
        results += [r for r in map(json.loads, old.read_text().splitlines())
                    if r["decoder"] == 1 and r["layout"] == 1 and r["ego"] == 1 and r["minute"] == 20]
    for r in results:
        r["motor"] = 0
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["gain"], r["motor"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, 1, 0.0, 1, 1, "", 0, m) for m in (1, 2) for s in range(1, seeds + 1)]
    jobs += [(s, 0.0, 20, rallies, 1, 0.0, 1, 1, "", 0, m) for m in (1, 2) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[1], j[10]) not in have]
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"motor regions: relative code, aligned layout, decoder 1, 20 minutes; frozen, "
          f"{rallies} uniform fixed serves")
    print("motor regions          " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    names = {0: "columns 0-1, 6-7", 1: "inner 1, 6", 2: "outer 0, 7"}
    by = {}
    for m in (0, 1, 2):
        for g in (1.0, 0.0):
            rs = sorted((r for r in results if r["motor"] == m and r["gain"] == g and r["seed"] <= seeds),
                        key=lambda r: r["seed"])
            if len(rs) < 2:
                continue
            by[m, g] = {r["seed"]: score(r) for r in rs}
            seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
            hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
            each = list(by[m, g].values())
            label = names[m] + ("" if g else " no reward")
            print(f"{label:22s} " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
                  f"   {mean(each):5.2f}  {statistics.stdev(each) / len(each) ** 0.5:.2f}  ({len(rs)})")
    for a, b in (((0, 1.0), (1, 1.0)), ((0, 1.0), (2, 1.0)), ((1, 0.0), (1, 1.0)), ((2, 0.0), (2, 1.0)),
                 ((0, 0.0), (0, 1.0))):
        if a in by and b in by:
            d = [by[b][s] - by[a][s] for s in by[b] if s in by[a]]
            print(f"{names[b[0]]}{'' if b[1] else ' no reward'} less {names[a[0]]}"
                  f"{'' if a[1] else ' no reward'}: {mean(d):+.2f} +- "
                  f"{statistics.stdev(d) / len(d) ** 0.5:.2f}, better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    return 0


def dishbrain_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/dishbrain.json"
    done_path = Path(out + "l")
    results = []
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["minute"], r["cond"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, 1, 0.0, 1, 1, "", c) for c in (2, 3, 4) for s in range(1, seeds + 1)]
    jobs += [(s, 1.0, 0, rallies, 1, 0.0, 1, 1, "", 4) for s in range(1, seeds + 1)]
    jobs = [j for j in jobs if (j[0], j[2], j[9]) not in have]
    jobs.sort(key=lambda j: -j[2])
    fe._lib()
    with ProcessPoolExecutor(8) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"DishBrain's feedback, plain STDP: relative code, aligned layout, decoder 1; "
          f"frozen, {rallies} uniform fixed serves")
    print("condition        minute  " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    names = {2: "stim", 3: "silent", 4: "none"}
    by = {}
    for c, m in ((4, 0), (2, 20), (3, 20), (4, 20)):
        rs = sorted((r for r in results if r["minute"] == m and r["cond"] == c and r["seed"] <= seeds),
                    key=lambda r: r["seed"])
        if len(rs) < 2:
            continue
        by[c, m] = {r["seed"]: score(r) for r in rs}
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        each = list(by[c, m].values())
        print(f"{'untrained' if m == 0 else names[c]:14s} {m:6d}  " +
              "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
              f"   {mean(each):5.2f}  {statistics.stdev(each) / len(each) ** 0.5:.2f}  ({len(rs)})")
    for a, b in (((4, 0), (2, 20)), ((3, 20), (2, 20)), ((4, 20), (2, 20)), ((4, 0), (4, 20))):
        if a in by and b in by:
            d = [by[b][s] - by[a][s] for s in by[b] if s in by[a]]
            la = "untrained" if a[1] == 0 else names[a[0]]
            print(f"{names[b[0]]} m{b[1]} less {la}: {mean(d):+.2f} +- "
                  f"{statistics.stdev(d) / len(d) ** 0.5:.2f}, better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    return 0


def settle_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/settle.json"
    done_path = Path(out + "l")
    results = []
    for old in ("build/cl_bridge/decoders.jsonl", "build/cl_bridge/longer.jsonl"):
        if Path(old).exists():
            results += [r for r in map(json.loads, Path(old).read_text().splitlines())
                        if r.get("decoder") == 1 and r["gain"] == 1.0 and r.get("layout", 1) == 1
                        and r.get("ego", 1) == 1]
    for r in results:
        r["schedule"] = ""
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["minute"], r["schedule"]) for r in results}
    jobs = [(s, 1.0, 20, rallies, 1, 0.0, 1, 1, sc) for sc in ("h2", "h4", "5h2")
            for s in range(1, seeds + 1)]
    jobs += [(s, 1.0, 40, rallies, 1, 0.0, 1, 1, "5h2") for s in range(1, seeds + 1)]
    jobs = [j for j in jobs if (j[0], j[2], j[8]) not in have]
    jobs.sort(key=lambda j: -j[2])
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"relative code, aligned layout, decoder 1; reward gain and scaling slowed together "
          f"from the start; frozen, {rallies} uniform fixed serves")
    print("schedule     minute  " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    by = {}
    for sc, m in (("", 5), ("", 20), ("", 40), ("h2", 20), ("h4", 20), ("5h2", 20), ("5h2", 40)):
        rs = {r["seed"]: r for r in results if r["minute"] == m and r["schedule"] == sc}
        rs = [rs[s] for s in sorted(rs) if s <= seeds]
        if len(rs) < 2:
            continue
        by[sc, m] = {r["seed"]: score(r) for r in rs}
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        each = list(by[sc, m].values())
        print(f"{sc or 'constant':10s} {m:6d}  " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
              f"   {mean(each):5.2f}  {statistics.stdev(each) / len(each) ** 0.5:.2f}  ({len(rs)})")
    for a, b in ((("", 20), ("h2", 20)), (("", 20), ("h4", 20)), (("", 20), ("5h2", 20)),
                 (("", 5), ("5h2", 20)), (("", 40), ("5h2", 40)), (("5h2", 20), ("5h2", 40))):
        if a in by and b in by:
            d = [by[b][s] - by[a][s] for s in by[b] if s in by[a]]
            print(f"{b[0] or 'constant'} m{b[1]} less {a[0] or 'constant'} m{a[1]}: {mean(d):+.2f} +- "
                  f"{statistics.stdev(d) / len(d) ** 0.5:.2f}, better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    return 0


def longer_main(args: list[str]) -> int:
    seeds = int(args[0]) if args else 16
    rallies = int(args[1]) if len(args) > 1 else 120
    out = args[2] if len(args) > 2 else "build/cl_bridge/longer.json"
    done_path = Path(out + "l")
    results = []
    old = Path("build/cl_bridge/decoders.jsonl")
    if old.exists():
        results += [r for r in map(json.loads, old.read_text().splitlines())
                    if r["decoder"] == 1 and r["gain"] == 1.0]
    if done_path.exists():
        results += [json.loads(line) for line in done_path.read_text().splitlines() if line]
    have = {(r["seed"], r["minute"]) for r in results}
    jobs = [(s, 1.0, m, rallies, 1, 0.0, 1, 1) for m in (5, 10, 20, 40) for s in range(1, seeds + 1)]
    jobs += [(s, 1.0, 60, rallies, 1, 0.0, 1, 1) for s in range(1, seeds // 2 + 1)]
    jobs = [j for j in jobs if (j[0], j[2]) not in have]
    jobs.sort(key=lambda j: -j[2])
    fe._lib()
    with ProcessPoolExecutor(10) as pool, done_path.open("a") as log:
        for f in as_completed([pool.submit(job, *j) for j in jobs]):
            r = f.result()
            log.write(json.dumps(r) + "\n")
            log.flush()
            results.append(r)
    Path(out).write_text(json.dumps(results))
    mean = statistics.mean

    def score(r):
        return sum(r["hit"]) / sum(r["seen"])

    print(f"relative code, aligned layout, decoder 1: frozen after so many minutes of training, "
          f"{rallies} uniform fixed serves")
    print("minute  hit rate by court row  " + "  ".join(f" row{k}" for k in range(8)) + "    all  +- s.e.")
    by = {}
    for m in (5, 10, 20, 40, 60):
        rs = [r for r in results if r["minute"] == m]
        if not rs:
            continue
        by[m] = {r["seed"]: score(r) for r in rs}
        seen = [sum(r["seen"][k] for r in rs) for k in range(8)]
        hit = [sum(r["hit"][k] for r in rs) for k in range(8)]
        each = list(by[m].values())
        se = statistics.stdev(each) / len(each) ** 0.5
        print(f"{m:6d}  {'':22s} " + "  ".join(f"{h / s if s else 0:5.2f}" for h, s in zip(hit, seen)) +
              f"   {mean(each):5.2f}  {se:.2f}  ({len(rs)})")
    for a, b in ((10, 20), (20, 40), (40, 60), (20, 60)):
        if a in by and b in by:
            d = [by[b][s] - by[a][s] for s in by[b] if s in by[a]]
            print(f"minute {b} less {a}: {mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}, "
                  f"better for {sum(x > 0 for x in d)} of {len(d)} seeds")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "steady":
        return steady_main(args[1:])
    if args and args[0] == "autopsy":
        return autopsy_main(args[1:])
    if args and args[0] == "yoked":
        return yoked_main(args[1:])
    if args and args[0] == "motor":
        return motor_main(args[1:])
    if args and args[0] == "dishbrain":
        return dishbrain_main(args[1:])
    if args and args[0] == "settle":
        return settle_main(args[1:])
    if args and args[0] == "longer":
        return longer_main(args[1:])
    if args and args[0] == "two":
        return two_main(args[1:])
    if args and args[0] == "decoders":
        return decoders_main(args[1:])
    if args and args[0] == "layouts":
        return layouts_main(args[1:])
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
