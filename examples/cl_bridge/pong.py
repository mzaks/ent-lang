"""Pong in closed loop with the dish, through the CL API, after DishBrain
(Kagan et al. 2022, Neuron 110:3952): the ball's place is stimulated into
a sensory region of the array, the paddle follows the activity of two
motor regions, and the dish's misses and hits are answered with
stimulation (or not, by condition).

    build/cl_bridge/venv/bin/python examples/cl_bridge/pong.py \\
        [minutes [seed [condition [flip] [columns] [name=value ...]]]]

conditions:
    stim      a hit: all 8 sensory electrodes at 100 Hz for 100 ms
              (predictable); a miss: 4 s of stims on random electrodes at
              5 Hz (unpredictable), then a new ball
    silent    no feedback; a miss is followed by 4 s with no sensory input
    none      no feedback, no pause: a miss is a new ball at once
    rest      no sensory input at all (the paddle moves by the dish's own
              activity); the ball and its feedback as in `none`
    reward    a hit: a pulse on REWARD (dopamine), a miss: one on PUNISH,
              then a new ball at once; each pulse the reward prediction
              error, its current 1 - p uA for a hit and p for a miss, p
              the running share of hits (RPE_RATE); the dish with
              reward-modulated STDP
              (modulated=1, reward_channel=REWARD, punish_channel=PUNISH,
              unless given): channels no CL1 has, for whether the model
              could learn the game were it told how it did

columns: the second layout (below), where the stims' direct answers in
the motor regions do not play the game.

flip: the sensory map upside down (the ball at the top stimulates the
electrode of the bottom), a control: a dish that follows the ball by its
wiring then plays worse, one whose paddle only moves with any stim not.

name=value: any of the dish's parameters (stdp=0, ...). Prints a line of
JSON: the rallies (each its start in seconds and its hits) and the
summary DishBrain gave: hits a rally, aces (a miss before any hit) and
long rallies (3 hits or more), in the first 5 minutes and after.

The layout on the 8 x 8 grid (channel = row * 8 + column):

    rows 0-2   motor "up"     24 electrodes
    rows 3-4   sensory        columns 2-5: 26 27 28 29 / 34 35 36 37
    rows 5-7   motor "down"   24 electrodes

A stim excites the neurons whose axons pass the electrode, which reach
about two electrodes from it, so the sensory electrodes of rows 3-4 drive
the motor regions beside them directly (row 3 up, row 4 down): with every
synapse at 0 they answer as with them. That is what plays the game in this
layout. The `columns` layout spreads that bias over the court instead:

    columns 0-1   motor "up"     16 electrodes
    columns 6-7   motor "down"   16 electrodes
    columns 3-4   sensory, a row each, the column alternating so that each
                  half of the court has two of each: 3 12 20 27 35 44 52 59
    (reward 28, punish 36)

The game, on a 1 x 1 court with the paddle at x = 0: the ball runs at
`BALL_SPEED` (court widths a second) and bounces off the other three
walls; a paddle of height `PADDLE` moves at up to `PADDLE_SPEED`. A
serve, and the ball off the paddle, go at a random angle of up to 45
degrees (with the ball only reflected, a paddle parked at a wall could
return some paths forever).

- Place: the sensory electrode stimulated is the one of the ball's y,
  eighths of the court from top to bottom: 26 27 28 29 37 36 35 34
  (round the region, so neighbouring places are neighbouring
  electrodes);
- rate: its stims come at 4 Hz with the ball at the far wall to 40 Hz at
  the paddle;
- motor: each tick, the spikes on the up and the down region, each
  averaged over `MOTOR_TAU`; the paddle moves at `PADDLE_SPEED` towards
  the bigger, not at all where they are within `DEAD_ZONE` of each other.

The loop runs at `TICKS` a second; a stim is a single pulse (the SDK's
1 uA, 160 us a phase: 1 in the dish's units), scheduled here, so a run
replays the same.
"""

from __future__ import annotations

import json
import math
import os
import random
import sys

TICKS = 200                  # loop iterations a second (5 ms each)
BALL_SPEED = 0.5             # court widths a second, along x
PADDLE = 0.3                 # the paddle's height
PADDLE_SPEED = 1.0           # court heights a second
MOTOR_TAU = 0.1              # s
DEAD_ZONE = 0.1              # of the larger of the two
HIT_HZ, HIT_S = 100, 0.1     # the predictable feedback
MISS_HZ, MISS_S = 5, 4.0     # the unpredictable one, and the pause
EARLY_S = 5 * 60             # DishBrain's first 5 minutes, against the rest
RPE_RATE = 0.05              # how fast `reward`'s expected share of hits follows

SENSORY = [26, 27, 28, 29, 37, 36, 35, 34]
UP = list(range(0, 24))
DOWN = list(range(40, 64))
REWARD, PUNISH = 31, 32      # free electrodes of rows 3 and 4 (`reward`)
# Each layout: sensory (top to bottom), up, down, reward, punish.
LAYOUTS = {
    "rows": (SENSORY, UP, DOWN, REWARD, PUNISH),
    "columns": ([r * 8 + c for r, c in zip(range(8), (3, 4, 4, 3, 3, 4, 4, 3))],
                [r * 8 + c for r in range(8) for c in (0, 1)],
                [r * 8 + c for r in range(8) for c in (6, 7)], 28, 36),
}
CONDITIONS = ("stim", "silent", "none", "rest", "reward")


class Pong:
    """The game and the protocol, with no I/O: `tick(up, down)` is given the
    spikes of a tick on each motor region and gives the channels to
    stimulate now (each a single pulse)."""

    def __init__(self, condition: str, seed: int, flip: bool = False, layout: str = "rows"):
        if condition not in CONDITIONS:
            raise ValueError(f"no condition {condition!r}: {', '.join(CONDITIONS)}")
        self.condition = condition
        sensory, self.up_channels, self.down_channels, self.reward, self.punish = LAYOUTS[layout]
        # (Flipped: the ball at the top is stimulated at the bottom.)
        self.sensory = sensory[::-1] if flip else sensory
        self.hit_channels = sensory
        self.rng = random.Random(seed * 7919 + 1)
        self.dt = 1.0 / TICKS
        self.t = 0.0
        self.paddle = 0.5               # its centre
        self.up = self.down = 0.0       # the motor regions' averages
        self.next_sense = 0.0           # when the next sensory stim is due
        self.feedback: list[tuple[float, list[int]]] = []   # (when, channels)
        self.paused_until = 0.0
        self.rallies: list[tuple[float, int]] = []          # (start, hits)
        self.hits = 0
        self.start = 0.0
        # (What the paddle did: ticks at the top, at the bottom, in all;
        # the motor regions' spikes.)
        self.at_top = self.at_bottom = self.ticks = 0
        self.up_spikes = self.down_spikes = 0
        self.expect = 0.5                # `reward`: the share of hits expected
        self.doses: dict[int, float] = {}   # this tick's stims not of 1 uA
        self.serve()

    def aim(self) -> None:
        angle = self.rng.uniform(-math.pi / 4, math.pi / 4)
        self.vy = abs(self.vx) * math.tan(angle)

    def serve(self) -> None:
        self.x = 1.0
        self.y = self.rng.uniform(0.1, 0.9)
        self.vx = -BALL_SPEED
        self.aim()
        self.hits = 0
        self.start = self.t

    def schedule(self, hz: float, seconds: float, channels) -> None:
        for k in range(int(hz * seconds)):
            pick = channels() if callable(channels) else channels
            self.feedback.append((self.t + k / hz, pick))

    def outcome(self, got: float, out: list[int]) -> None:
        """`reward`: the pulse of a hit (got 1) or a miss (0), the reward
        less what was expected."""
        err = got - self.expect
        self.expect += RPE_RATE * err
        if err != 0.0:
            channel = self.reward if err > 0 else self.punish
            out.append(channel)
            self.doses[channel] = abs(err)

    def tick(self, up: int, down: int) -> list[int]:
        dt, decay = self.dt, math.exp(-self.dt / MOTOR_TAU)
        self.doses = {}
        self.up = decay * self.up + up
        self.down = decay * self.down + down
        self.up_spikes += up
        self.down_spikes += down
        self.ticks += 1
        self.at_top += self.paddle <= PADDLE / 2 + 1e-9
        self.at_bottom += self.paddle >= 1 - PADDLE / 2 - 1e-9
        out: list[int] = []
        while self.feedback and self.feedback[0][0] <= self.t + 1e-9:
            out += self.feedback.pop(0)[1]
        if self.t < self.paused_until:
            self.t += dt
            return out
        # The paddle.
        big = max(self.up, self.down)
        if big > 0 and abs(self.up - self.down) > DEAD_ZONE * big:
            step = PADDLE_SPEED * dt * (1 if self.down > self.up else -1)
            self.paddle = min(max(self.paddle + step, PADDLE / 2), 1 - PADDLE / 2)
        # The ball.
        self.x += self.vx * dt
        self.y += self.vy * dt
        if self.y < 0 or self.y > 1:
            self.y = -self.y if self.y < 0 else 2 - self.y
            self.vy = -self.vy
        if self.x > 1:
            self.x, self.vx = 2 - self.x, -self.vx
        if self.x < 0:
            if abs(self.y - self.paddle) <= PADDLE / 2:
                self.x, self.vx = -self.x, -self.vx
                self.aim()
                self.hits += 1
                if self.condition == "stim":
                    self.schedule(HIT_HZ, HIT_S, self.hit_channels)
                elif self.condition == "reward":
                    self.outcome(1.0, out)
            else:
                self.rallies.append((round(self.start, 3), self.hits))
                if self.condition == "stim":
                    self.schedule(MISS_HZ, MISS_S, lambda: [self.rng.randrange(64)])
                elif self.condition == "reward":
                    self.outcome(0.0, out)
                if self.condition in ("stim", "silent"):
                    self.paused_until = self.t + MISS_S
                self.t += dt
                self.serve()
                self.start = max(self.t, self.paused_until)
                return out
        # The ball's place and distance, as the sensory stims.
        if self.condition != "rest" and self.t >= self.next_sense:
            row = min(int(self.y * 8), 7)
            out.append(self.sensory[row])
            hz = 4 + 36 * (1 - min(max(self.x, 0), 1))
            self.next_sense = self.t + 1 / hz
        self.t += dt
        return out


def summary(rallies: list[tuple[float, int]]) -> dict:
    def of(rs):
        if not rs:
            return {"rallies": 0}
        hits = [h for _, h in rs]
        return {"rallies": len(rs), "hits": sum(hits) / len(hits),
                "aces": sum(h == 0 for h in hits) / len(hits),
                "long": sum(h >= 3 for h in hits) / len(hits)}
    return {"early": of([r for r in rallies if r[0] < EARLY_S]),
            "late": of([r for r in rallies if r[0] >= EARLY_S]),
            "all": of(rallies)}


def play(minutes: float, seed: int, condition: str, flip: bool = False,
         layout: str = "rows", **parameters) -> dict:
    os.environ.setdefault("CL_SDK_ACCELERATED_TIME", "1")
    import cl
    import ent_dish
    game = Pong(condition, seed, flip, layout)
    up_set, down_set = set(game.up_channels), set(game.down_channels)
    dish = dict(parameters)
    if condition == "reward":
        dish = {"modulated": 1, "reward_channel": game.reward, "punish_channel": game.punish, **dish}
    ent_dish.use(seed=seed, **dish)
    with cl.open() as neurons:
        for tick in neurons.loop(TICKS, stop_after_seconds=minutes * 60):
            up = down = 0
            for spike in tick.analysis.spikes:
                if spike.channel in up_set:
                    up += 1
                elif spike.channel in down_set:
                    down += 1
            channels = game.tick(up, down)
            plain = sorted(set(c for c in channels if c not in game.doses))
            if plain:
                neurons.stim(cl.ChannelSet(*plain), 1.0)
            for channel, current in game.doses.items():
                neurons.stim(channel, current)
    return {"seed": seed, "condition": condition, "flip": flip, "minutes": minutes,
            "parameters": parameters, **summary(game.rallies),
            "paddle_top": game.at_top / game.ticks, "paddle_bottom": game.at_bottom / game.ticks,
            "up_hz": game.up_spikes / (minutes * 60), "down_hz": game.down_spikes / (minutes * 60),
            "rallies": game.rallies, "layout": layout}


def main() -> int:
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    condition = sys.argv[3] if len(sys.argv) > 3 else "stim"
    flip = "flip" in sys.argv[4:]
    layout = "columns" if "columns" in sys.argv[4:] else "rows"
    parameters = {k: float(v) for k, v in (a.split("=", 1) for a in sys.argv[4:] if "=" in a)}
    print(json.dumps(play(minutes, seed, condition, flip, layout, **parameters)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
