#!/usr/bin/env python3
"""The waves are Xyce's (the spec's check 5).

  examples/analog/crossbar.py 256 1 yakopcic 0.1 1 write   (and others)
  examples/analog/prepare.py all
  examples/analog/check/xyce.py [circuits] [timeout=S]

For each prepared circuit that Xyce can run (prepare.py wrote it a
circuit.xyce: of our memristor models Xyce has Yakopcic's built in, not
Biolek's nor VTEAM's; and a power grid's operating point is for check 7),
by default the crossbars, Xyce runs it with tight tolerances (reltol
1e-6, abstol 1e-12, and Newton's 1e-6 and 1e-15; its steps no longer than
the circuit's step, as check 3 runs ngspice) by Gear's rule (BDF), single-threaded, for up to `timeout` seconds (by
default 1,800: where it takes longer the circuit is said and left out),
and way 0 runs it at reltol 1e-5. As in check 3: Xyce's values are taken
at the model's time points, in a straight line between its own; every
probe at every time point is to be within 1e-3 of the probe's range in
Xyce's waves. Says how long each took.

(By Xyce's default, the trapezoidal rule, it runs the 4 by 4 crossbar
with 10 ohm wires, written, in a tenth of a second at its own
tolerances, 1.0e-3 of the range off ngspice's reference of check 3; at
reltol 1e-4 or tighter its steps shrink to 3e-16 s where the devices
switch, at 13.5 ms, and it is not done in hours. By Gear's rule at reltol
1e-6 it takes 1,171 time points, 2.4e-4 off ngspice's.)
"""

import bisect
import contextlib
import fcntl
import os
import re
import shutil
import subprocess
import sys
import resource
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

SHARE = 1e-3
# (Xyce on a large crossbar takes gigabytes: one at a time on the machine,
# across processes, and each held to MEMORY bytes of address space, so
# that one past it fails alone instead of the kernel's OOM killer taking
# the terminal it runs in.)
MEMORY = int(float(os.environ.get("ANALOG_XYCE_GB", "12")) * 2**30)
LOCK = os.path.join(common.ROOT, "build", "analog", "xyce.lock")


@contextlib.contextmanager
def alone():
    """Held while Xyce runs: no other Xyce of ours runs meanwhile."""
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    with open(LOCK, "w") as file:
        fcntl.flock(file, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(file, fcntl.LOCK_UN)


def limited():
    """For Popen's preexec_fn: the process's memory held to MEMORY."""
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY, MEMORY))


def xyce(name, net, timeout):
    """Xyce's waves (a list of lines: the time, or nothing at an operating
    point, then the probes), what it said where it gave none, and how long
    it took."""
    folder = os.path.join(common.RUN, f"{name}_xyce_{os.getpid()}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    with open(os.path.join(common.CIRCUITS, name, "circuit.xyce")) as file:
        deck = file.read()
    options = (".options timeint method=gear reltol=1e-6 abstol=1e-12\n"
               ".options nonlin reltol=1e-6 abstol=1e-15\n")
    if net.tran:
        step, stop = net.tran[0], net.tran[1]
        most = min(net.tran[2], step) if len(net.tran) > 2 else step
        deck = re.sub(r"^\.tran .*$", f".tran {step!r} {stop!r} 0 {most!r}", deck, flags=re.M)
    deck = deck.replace("\n.print", "\n" + options + ".print", 1)
    with open(os.path.join(folder, "circuit.xyce"), "w") as file:
        file.write(deck)
    with alone():
        began = time.time()
        try:
            said = subprocess.run(["Xyce", "-quiet", "circuit.xyce"], capture_output=True,
                                  text=True, cwd=folder, timeout=timeout, preexec_fn=limited,
                                  env=dict(os.environ, OMP_NUM_THREADS="1"))
        except subprocess.TimeoutExpired:
            shutil.rmtree(folder, ignore_errors=True)
            return None, f"not done in {timeout} s", timeout
        took = time.time() - began
    lines = []
    path = os.path.join(folder, "xyce.txt")
    if os.path.exists(path):
        with open(path) as file:
            for line in file:
                try:
                    lines.append([float(w) for w in line.split()])
                except ValueError:
                    continue
    shutil.rmtree(folder, ignore_errors=True)
    if not lines:
        why = (said.stdout[-500:] + said.stderr[-500:]).strip()
        if said.returncode < 0:
            why = f"stopped by signal {-said.returncode}, likely past its " \
                  f"{MEMORY / 2**30:g} GB (ANALOG_XYCE_GB); {why[-200:]}"
        return None, why, took
    if not net.tran:
        lines = [[0.0] + line for line in lines]
    return lines, "", took


def main():
    arguments = sys.argv[1:]
    timeout = next((float(a[8:]) for a in arguments if a.startswith("timeout=")), 1800.0)
    names = [a for a in arguments if "=" not in a] or \
        [n for n in sorted(os.listdir(common.CIRCUITS)) if n.startswith("crossbar_")]
    names = [n for n in names if os.path.exists(os.path.join(common.CIRCUITS, n, "circuit.xyce"))]
    program = common.build()
    failed = checked = 0
    for name in names:
        net = common.Netlist(name)
        theirs, why, took = xyce(name, net, timeout)
        if theirs is None:
            print(f"{name}: Xyce gave nothing ({why}); left out")
            continue
        checked += 1
        times = [line[0] for line in theirs]
        columns = len(theirs[0]) - 1

        def at(t, k):
            """Xyce's probe k at time t, in a straight line between."""
            if not net.tran:
                return theirs[0][1 + k]
            j = min(max(bisect.bisect_left(times, t), 1), len(times) - 1)
            t0, t1 = times[j - 1], times[j]
            v0, v1 = theirs[j - 1][1 + k], theirs[j][1 + k]
            return v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)

        ranges = [max(1e-12, max(line[1 + k] for line in theirs) -
                      min(line[1 + k] for line in theirs),
                      abs(theirs[0][1 + k]) if not net.tran else 0.0)
                  for k in range(columns)]
        began = time.time()
        waves, said = common.run(program, name, 0, reltol=1e-5)
        ours = time.time() - began
        if not waves or "settled true" not in said:
            failed += 1
            print(f"{name}: way 0 did not settle ({said})")
            continue
        worst, where = 0.0, None
        for line in waves:
            for k in range(columns):
                share = abs(line[1 + k] - at(line[0], k)) / ranges[k]
                if share > worst:
                    worst, where = share, (line[0], k, line[1 + k], at(line[0], k))
        head = (f"{name}: Xyce {len(theirs)} time points in {took:.1f} s, way 0 {len(waves)} "
                f"in {ours:.1f} s")
        if worst > SHARE:
            failed += 1
            t, k, mine, theirs_ = where
            print(f"{head}; NOT Xyce's: probe {k} at {t} s is {mine!r}, Xyce {theirs_!r}")
        else:
            print(f"{head}; within {worst:.1e} of the range")
    print(f"{checked} circuits held against Xyce, {failed} not as it")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
