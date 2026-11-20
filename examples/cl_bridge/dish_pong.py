"""Pong played by the dish, the game in C next to it (game.c): no cl-sdk,
one call a frame, and the same games as the web page (web/), which runs
the same C as WebAssembly. Plays the untrained dish (reward gain 0) and
the trained one (gain 1) of one seed side by side, and writes a recording
of both that web/index.html replays.

    build/cl_bridge/venv/bin/python examples/cl_bridge/dish_pong.py \\
        [minutes [seed [out.json]]] [reward] [ego] [record=seconds]

seed: a number, or any text (hashed to one: ent_dish.seed_of, as the page
does). reward: the reward for hits and misses instead of for each move;
ego: the ball's place relative to the paddle (pong.py's BALL_EGO);
record: the seconds at the end that are recorded (120; 0 for all).
Prints each dish's hits a rally after the first 5 minutes.
"""
from __future__ import annotations

import ctypes
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

TICKS = 200
FRAME_TICKS = 4            # a frame each 20 ms


def play(minutes: float, seed: int, gain: float, cond: int, ego: int, record_s: float) -> dict:
    import ent_dish
    lib = ctypes.CDLL(str(ent_dish.build_library()))
    lib.pong_make_dish.restype = ctypes.c_void_p
    lib.pong_make_dish.argtypes = [ctypes.c_int64, ctypes.c_double, ctypes.c_int32]
    lib.pong_create.restype = ctypes.c_void_p
    lib.pong_create.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int32, ctypes.c_int32]
    lib.pong_run.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p]
    lib.pong_state.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    dish = lib.pong_make_dish(seed, gain, cond)
    game = lib.pong_create(dish, seed, cond, ego)
    spikes, stims = (ctypes.c_int32 * 64)(), (ctypes.c_int32 * 64)()
    state = (ctypes.c_double * 12)()
    frames_all = int(round(minutes * 60 * TICKS / FRAME_TICKS))
    first = 0 if record_s <= 0 else max(0, frames_all - int(round(record_s * TICKS / FRAME_TICKS)))
    frames, rallies, seen = [], [], 0
    for f in range(frames_all):
        ctypes.memset(spikes, 0, 256)
        ctypes.memset(stims, 0, 256)
        if lib.pong_run(game, FRAME_TICKS, spikes, stims) < 0:
            raise RuntimeError("the dish had no room for its spikes")
        lib.pong_state(game, state)
        while seen < int(state[4]):
            rallies.append([round(state[0], 3), int(state[6])])
            seen += 1
        if f >= first:
            frames.append([round(state[0], 3), round(state[1], 3), round(state[2], 3),
                           round(state[3], 3), int(state[4]), int(state[5]),
                           "".join(chr(48 + min(n, 74)) for n in spikes),
                           "".join(str(n) for n in stims)])
    late = state[11] / state[10] if state[10] else 0.0
    return {"seed": seed, "gain": gain, "condition": "reward" if cond else "moves",
            "ego": bool(ego), "minutes": minutes, "frame_s": FRAME_TICKS / TICKS,
            "sensory": [3, 12, 20, 27, 35, 44, 52, 59],
            "up": [r * 8 + c for r in range(8) for c in (0, 1)],
            "down": [r * 8 + c for r in range(8) for c in (6, 7)],
            "reward": 28, "punish": 36, "paddle_h": 0.3,
            "parameters": {"reward_gain": gain, "axon": 0.11, "hard": 1,
                           **({"tau_elig": 200} if cond == 0 else {})},
            "late_hits": late, "late_rallies": int(state[10]), "rallies_all": int(state[4]),
            "moves_towards": state[9] / state[8] if state[8] else 0.0,
            "rallies": rallies, "frames": frames}


def main() -> int:
    import ent_dish
    plain = [a for a in sys.argv[1:] if "=" not in a and a not in ("reward", "ego")]
    minutes = float(plain[0]) if plain else 20.0
    text = plain[1] if len(plain) > 1 else "1"
    out = plain[2] if len(plain) > 2 else None
    seed = int(text) if text.isdigit() else ent_dish.seed_of(text)
    cond = 1 if "reward" in sys.argv[1:] else 0
    ego = 1 if "ego" in sys.argv[1:] else 0
    record_s = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("record=")), 120.0)
    ent_dish.build_library()
    with ProcessPoolExecutor(2) as pool:
        clips = list(pool.map(play, [minutes] * 2, [seed] * 2, [0.0, 1.0], [cond] * 2,
                              [ego] * 2, [record_s] * 2))
    for c in clips:
        print(f"seed {seed} ({text!r}), gain {c['gain']:g}: {c['late_hits']:.2f} hits a rally "
              f"after 5 minutes, {c['rallies_all']} rallies, moves towards the ball "
              f"{c['moves_towards']:.3f}")
    if out:
        with open(out, "w") as f:
            json.dump({"seed_text": text, "clips": clips}, f, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
