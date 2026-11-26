"""What the checks of examples/analog share: the model built and run on a
prepared circuit, its waves read, and the circuit read here too."""

import atexit
import bisect
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ANALOG = os.path.join(ROOT, "examples", "analog")
CIRCUITS = os.path.join(ROOT, "build", "analog", "circuits")
RUN = os.path.join(ROOT, "build", "analog", "run")


def build(more=()):
    """The model (simulate.ent), built where the checks run it."""
    os.makedirs(RUN, exist_ok=True)
    # (A program of its own for each run of a check: another check may be
    # running the one built before.)
    program = os.path.join(RUN, "simulate" + ("_parallel" if "--parallel" in more else "") +
                           f"_{os.getpid()}")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", *more,
                    "-I", ANALOG, os.path.join(ANALOG, "simulate.ent"), "-o", program],
                   check=True, env=environment)
    atexit.register(lambda: os.path.exists(program) and os.remove(program))
    return program


def run(program, name, way, at=0.0, most=100000, environment=None, step=0.0, folder=None,
        operating=False, reltol=0.001, method=0):
    """The model's waves for a circuit, the way given, and what it said:
    a list of lines, each a list of floats (the time, then the probes).
    `step`: a fixed time step (0: steps chosen by their error), `method`
    its rule (0 backward Euler, 1 trapezoidal). `operating`: the circuit
    without its `tran`, so only its operating point at `at`."""
    folder = folder or os.path.join(RUN, f"{name}_way{way}_{os.getpid()}")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(CIRCUITS, name, "circuit.net")) as file:
        text = file.read()
    if operating:
        text = "".join(line for line in text.splitlines(True) if not line.startswith("tran"))
    with open(os.path.join(folder, "circuit.net"), "w") as file:
        file.write(text)
    if os.path.exists(os.path.join(folder, "waves.txt")):
        os.remove(os.path.join(folder, "waves.txt"))
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{way} 1 1 {reltol!r} 0.000001 1e-12 {most} {at!r} {step!r} {method}\n")
    said = subprocess.run([program], capture_output=True, text=True, cwd=folder,
                          env=environment).stdout.strip()
    waves = []
    if os.path.exists(os.path.join(folder, "waves.txt")):
        with open(os.path.join(folder, "waves.txt")) as file:
            waves = [[float(word) for word in line.split()] for line in file if line.strip()]
    return waves, said


# A jump: where a probe of the waves held to moves at a pace that takes it
# across its whole range in 1/STEEP of the run's length or less, by at
# least JUMP of its range (a switching device's runaway: the RRAM cell's
# set takes its current up tenfold in nanoseconds; a spike: a Morris-Lecar
# membrane falls 100 mV in 50 us, where the solvers differ in when by 0.07
# us after 8 ms of spikes, ngspice and the model, and a stiff solver at
# 1e-12 by 0.3 us from both). The jump can come a bit early or late at any tolerance
# (the cell's set at reltol 1e-4, 1e-5, 1e-7 comes 1.6e-4, 7e-5, 2.8e-5 s
# before a stiff solver's at 1e-12, of a 4 s sweep), and the climb up to
# it with it, so there a point off in its value says little. A point off
# from BEFORE of the run's length before a jump to ON after it is held to
# when the jump comes instead: the waves are to cross the jump's middle
# within ON of the run's length of where those held to do, and the point
# to be within its tolerance of what those held to go through over as
# long again as the jump is early or late (and half as long more).
STEEP = 1e2
JUMP = 0.05
ON = 1e-4
BEFORE = 1e-3


def jumps(waves, k, size):
    """The jumps of probe k of the waves (its range `size`): (start, end,
    middle value, when the middle is crossed)."""
    span = waves[-1][0] - waves[0][0] if len(waves) > 1 else 0.0
    found = []

    def steep(j):
        dt = waves[j][0] - waves[j - 1][0]
        dv = abs(waves[j][1 + k] - waves[j - 1][1 + k]) / size
        return dv > 0 and (dt <= 0 or dv * span / dt > STEEP)

    j = 1
    while j < len(waves) and span > 0:
        if not steep(j):
            j += 1
            continue
        # (One way: a spike's up and down are two jumps, else the two
        # together may come back to near where they started.)
        start = j - 1
        rising = waves[j][1 + k] > waves[j - 1][1 + k]
        while j < len(waves) and steep(j) and (waves[j][1 + k] > waves[j - 1][1 + k]) == rising:
            j += 1
        a, b = waves[start], waves[j - 1]
        if abs(b[1 + k] - a[1 + k]) > JUMP * size:
            middle = (a[1 + k] + b[1 + k]) / 2
            found.append((a[0], b[0], middle, crossing(waves, k, middle, a[0], b[0])))
    return found


def crossing(waves, k, middle, start, end):
    """When probe k of the waves first crosses `middle` between start and
    end (in a straight line between its time points), or None."""
    for j in range(1, len(waves)):
        t0, t1 = waves[j - 1][0], waves[j][0]
        if t1 < start or t0 > end:
            continue
        a, b = waves[j - 1][1 + k] - middle, waves[j][1 + k] - middle
        if a == 0:
            return t0
        if a * b < 0:
            return t0 + (t1 - t0) * a / (a - b)
    return None


def on_a_jump(t, k, value, tolerance, theirs, theirs_jumps, mine):
    """Whether a point of `mine` at t, probe k, `value`, is about one of the
    jumps of the waves held to (`theirs`), and comes with it: `mine`
    crosses its middle when they do, and the value is within `tolerance`
    of what they go through over as long as the jump is early or late."""
    span = theirs[-1][0] - theirs[0][0] if len(theirs) > 1 else 0.0
    near = ON * span
    # (Any of the jumps near it that so explains it: one jump can come in
    # two parts where it slows on the way, as a growing memristor's
    # current does, and the point is the second's.)
    for start, end, middle, when in theirs_jumps:
        if not (start - BEFORE * span <= t <= end + near) or when is None:
            continue
        mine_when = crossing(mine, k, middle, start - near, end + near)
        if mine_when is None or abs(mine_when - when) > near:
            continue
        shift = 1.5 * abs(mine_when - when)
        seen = [line[1 + k] for line in theirs if t - shift <= line[0] <= t + shift]
        seen += [level_at(theirs, k, t - shift), level_at(theirs, k, t + shift)]
        if min(seen) - tolerance <= value <= max(seen) + tolerance:
            return True
    return False


def level_at(waves, k, t):
    """Probe k of the waves at t, in a straight line between its points."""
    j = min(max(bisect.bisect_left([line[0] for line in waves], t), 1), len(waves) - 1)
    t0, t1 = waves[j - 1][0], waves[j][0]
    a, b = waves[j - 1][1 + k], waves[j][1 + k]
    return a if t1 == t0 else a + (b - a) * (t - t0) / (t1 - t0)


class Netlist:
    """circuit.net as Python reads it."""

    def __init__(self, name):
        self.nodes, self.devices, self.probes, self.points = 0, [], [], []
        self.op = self.tran = None
        with open(os.path.join(CIRCUITS, name, "circuit.net")) as file:
            for line in file:
                words = line.split()
                if not words:
                    continue
                if words[0] == "nodes":
                    self.nodes = int(words[1])
                elif words[0] in ("R", "C", "V", "I", "M"):
                    kind, a, b = words[0], int(words[1]), int(words[2])
                    rest = words[3:]
                    if kind in "VIM":
                        self.devices.append((kind, a, b, rest[0], [float(w) for w in rest[1:]]))
                    else:
                        self.devices.append((kind, a, b, None, [float(w) for w in rest]))
                elif words[0] == "probe":
                    self.probes.append((words[1], int(words[2])))
                elif words[0] == "tran":
                    self.tran = [float(w) for w in words[1:]]
                elif words[0] == "op":
                    self.op = True


def level(wave, numbers, t):
    """A source's value at time t, as sources.ent has it (floats)."""
    import math
    if wave == "dc":
        return numbers[0]
    if wave == "sin":
        offset, amplitude, hertz, delay, damping, phase = numbers
        turn = phase / 360
        if t < delay:
            return offset + amplitude * math.sin(2 * math.pi * turn)
        return offset + amplitude * math.exp(-(t - delay) * damping) * \
            math.sin(2 * math.pi * (hertz * (t - delay) + turn))
    if wave == "pulse":
        low, high, delay, rise, fall, width, period = numbers
        if t < delay:
            return low
        at = math.fmod(t - delay, period) if period > 0 else t - delay
        if at < rise:
            return low + (high - low) * at / rise
        if at < rise + width:
            return high
        if at < rise + width + fall:
            return high + (low - high) * (at - rise - width) / fall
        return low
    points = list(zip(numbers[1::2], numbers[2::2]))
    if t <= points[0][0]:
        return points[0][1]
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if t0 <= t < t1:
            return v0 + (v1 - v0) * (t - t0) / (t1 - t0)
    return points[-1][1]
