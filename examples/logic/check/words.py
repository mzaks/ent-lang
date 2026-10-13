#!/usr/bin/env python3
"""A circuit written out word by word gives what its gates give.

  examples/logic/fetch.sh
  examples/logic/prepare.py all
  examples/logic/check/words.py [cycles] [circuits] [each=N]

For each prepared circuit (all of build/logic/circuits, or those named)
examples/logic/words.py writes the circuit's Verilog out word by word,
examples/logic/coded.ent runs that for so many clock cycles (1,000),
and Icarus Verilog runs the circuit's gates with the same inputs
(check/equal.py makes its testbench). Every output of every cycle is to
be the same. `ops` (examples/logic/ops.v) is the circuit made for this:
every operation of Verilog, words of many widths, flip-flops and a
memory. The processor is held against its gates in check/cpu.py. With
`each=N` words.py puts so many operations in a system, so that a small
circuit is shared out among several too.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
LOGIC = os.path.join(ROOT, "examples", "logic")
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import equal


def model(name, cycles, each=()):
    said = subprocess.run([sys.executable, os.path.join(LOGIC, "words.py"), name, *each],
                          capture_output=True, text=True)
    if said.returncode:
        return None, (said.stderr or said.stdout).strip().split("\n")[-1]
    folder = os.path.join(equal.RUN, "words_" + name)
    os.makedirs(folder, exist_ok=True)
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "-I", LOGIC,
                    "-I", os.path.join(ROOT, "build", "logic", "words", name),
                    os.path.join(LOGIC, "coded.ent"), "-o", os.path.join(folder, "coded")],
                   check=True, env=environment)
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{cycles} {equal.SEED}\n")
    subprocess.run([os.path.join(folder, "coded")], check=True, capture_output=True, cwd=folder)
    with open(os.path.join(folder, "outputs.txt")) as file:
        lines = [line for line in file.read().split("\n") if line]
    shutil.rmtree(folder)
    return lines, said.stdout.strip()


def main():
    arguments = sys.argv[1:]
    cycles = next((int(a) for a in arguments if a.isdigit()), 1000)
    each = [a[5:] for a in arguments if a.startswith("each=")]
    arguments = [a for a in arguments if not a.startswith("each=")]
    names = [a for a in arguments if not a.isdigit()] or sorted(
        name[:-4] for name in os.listdir(equal.CIRCUITS) if name.endswith(".aag"))
    os.makedirs(equal.RUN, exist_ok=True)
    failed = skipped = 0
    for name in names:
        got, said = model(name, cycles, each)
        if got is None:
            skipped += 1
            print(f"{name}: not written out: {said}")
            continue
        wanted = equal.reference(name, cycles)
        differ = next((cycle for cycle, (a, b) in enumerate(zip(wanted, got)) if a != b), None)
        if differ is None and len(wanted) == len(got) == cycles:
            print(f"{name}: the same {cycles} cycles, {len(wanted[0]) if wanted else 0} outputs "
                  f"({said.split(': ', 1)[-1]})")
            continue
        failed += 1
        if differ is None:
            print(f"{name}: NOT the same: {len(wanted)} cycles from Icarus, {len(got)} in words")
        else:
            at = next(i for i, (a, b) in enumerate(zip(wanted[differ], got[differ])) if a != b)
            print(f"{name}: NOT the same from cycle {differ}, output {at}: "
                  f"Icarus {wanted[differ][at]}, the words {got[differ][at]}")
    print(f"{len(names)} circuits, {failed} not the same, {skipped} not written out")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
