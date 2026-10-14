#!/usr/bin/env python3
"""The model reads a circuit as its file has it (the spec's check 1).

  examples/analog/prepare.py all
  examples/analog/check/parse.py [circuits]

For each prepared circuit (all of build/analog/circuits, or those
named) the model reads circuit.net, and what it has made of it is held
against the file: as many nodes, devices and probes as the file's second
line says; as many resistors, capacitors, voltage sources, current
sources and memristors as it has lines of each; ground and every node a
source to ground sets fixed, and the rest not; two edges for every
device, to its own two nodes.

First, the model reads numbers as Python does: 20,000 numbers written as
prepare.py writes them (as few digits as give the same f64), of every
size from 1e-300 to 1e300, each to be read as that f64 exactly. Says how
many are not, and the first.
"""

import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ANALOG = os.path.join(ROOT, "examples", "analog")
CIRCUITS = os.path.join(ROOT, "build", "analog", "circuits")
RUN = os.path.join(ROOT, "build", "analog", "run")


def build():
    program = os.path.join(RUN, "parse_check")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "-I", ANALOG,
                    os.path.join(HERE, "parse_check.ent"), "-o", program],
                   check=True, env=environment)
    return program


def said(program, folder):
    out = subprocess.run([program], check=True, capture_output=True, text=True, cwd=folder).stdout
    found = {}
    for line in out.split("\n"):
        words = line.split()
        head = words[0] if len(words) % 2 else None
        pairs = words[1:] if head else words
        for key, value in zip(pairs[::2], pairs[1::2]):
            found[(head, key) if head in ("said", "made") else key] = value
    return found


def numbers(program):
    """Numbers of every size, and each as a whole number times a power of
    two, which the model works out exactly and holds against its reading."""
    pick = random.Random(1)
    values = [0.1, 0.2, 0.3, 1e-14, 1e-9, 2.5e-6, 16000.0, 1.0000000000000002e-14,
              9.999999999999999e-06, 0.7000000000000001, 123456789012345680.0,
              5e-324 * 2 ** 60, 1.7976931348623157e308, 2.2250738585072014e-308]
    for _ in range(20000 - len(values)):
        values.append(pick.uniform(1, 10) * 10.0 ** pick.randint(-300, 300) *
                      pick.choice((1, -1)))
    lines = []
    for value in values:
        whole, power = abs(value).as_integer_ratio()
        exponent = 0
        while power > 1:
            power //= 2
            exponent -= 1
        while whole >= 1 << 53:
            if whole & 1:
                raise ValueError(value)
            whole //= 2
            exponent += 1
        high, low = whole >> 26, whole & ((1 << 26) - 1)
        lines.append(f"{value!r} {1 if value < 0 else 0} {high} {low} {exponent}")
    folder = os.path.join(RUN, "numbers")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "numbers.txt"), "w") as file:
        file.write("\n".join(lines) + "\n")
    out = subprocess.run([program], check=True, capture_output=True, text=True,
                         cwd=folder).stdout
    shutil.rmtree(folder)
    line = next(line for line in out.split("\n") if line.startswith("numbers"))
    print(line.replace("numbers", "numbers read:", 1) + " from what Python reads")
    return " differ 0" in line


def counted(path):
    """What the file has, by Python's reading of it."""
    kinds, fixed, header = {}, {0}, None
    with open(path) as file:
        for line in file:
            words = line.split()
            if not words:
                continue
            if words[0] == "nodes":
                header = {"nodes": words[1], "devices": words[3], "probes": words[5]}
            elif words[0] in ("R", "C", "V", "I", "M"):
                kinds[words[0]] = kinds.get(words[0], 0) + 1
                a, b = int(words[1]), int(words[2])
                if words[0] == "V" and (a == 0 or b == 0):
                    fixed.add(a or b)
    return header, kinds, len(fixed)


def main():
    names = sys.argv[1:] or sorted(os.listdir(CIRCUITS))
    os.makedirs(RUN, exist_ok=True)
    program = build()
    failed = 0 if numbers(program) else 1
    for name in names:
        folder = os.path.join(CIRCUITS, name)
        header, kinds, fixed = counted(os.path.join(folder, "circuit.net"))
        got = said(program, folder)
        wrong = []
        if got.get("read") != "true" or got.get("wrong") != "0":
            wrong.append(f"not read (line {got.get('wrong')})")
        for key in ("nodes", "devices", "probes"):
            if got.get(("said", key)) != header[key] or got.get(("made", key)) != header[key]:
                wrong.append(f"{key}: the file {header[key]}, the model made "
                             f"{got.get(('made', key))}")
        for kind in ("R", "C", "V", "I", "M"):
            if got.get(kind) != str(kinds.get(kind, 0)):
                wrong.append(f"{kind}: the file {kinds.get(kind, 0)}, the model {got.get(kind)}")
        if got.get("fixed") != str(fixed):
            wrong.append(f"fixed: {fixed} to be, the model {got.get('fixed')}")
        if got.get("edges") != str(2 * int(header["devices"])) or got.get("astray") != "0":
            wrong.append(f"edges {got.get('edges')}, astray {got.get('astray')}")
        if wrong:
            failed += 1
            print(f"{name}: NOT as the file says: " + "; ".join(wrong))
        else:
            print(f"{name}: {header['nodes']} nodes, {header['devices']} devices, "
                  f"{header['probes']} probes, as the file says")
    print(f"{len(names)} circuits, {failed} not as they should be")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
