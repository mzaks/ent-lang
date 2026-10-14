"""What the checks of examples/analog share: the model built and run on a
prepared circuit, its waves read, and the circuit read here too."""

import atexit
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
