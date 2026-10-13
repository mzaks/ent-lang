#!/usr/bin/env python3
"""One core and all of them give the same outputs (the spec's sixth
check).

  examples/logic/prepare.py c6288 s38584 mem_ctrl
  examples/logic/check/same.py [cycles] [circuits]

Builds the simulator for one core and for all of them (--parallel), and
runs each circuit (c6288, s38584 and mem_ctrl, or those named) for so
many cycles (300) every way the model has: the gates as entities, all
of them or only those whose inputs changed, as rows, and written out as
code. The build for all the cores runs on 1, 4, 16 and all threads; its
outputs and the number it makes of all 64 runs are to be those of the
build for one core, each way.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
LOGIC = os.path.join(ROOT, "examples", "logic")
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
RUN = os.path.join(ROOT, "build", "logic", "same")


def build(source, built, flags, more=()):
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", *flags,
                    "-I", LOGIC, *more, os.path.join(LOGIC, source), "-o", built],
                   check=True, env=environment)
    return built


def run(program, folder, cycles, way, threads):
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{cycles} 1 1 {way}\n")
    environment = dict(os.environ)
    if threads:
        environment["OMP_NUM_THREADS"] = str(threads)
    said = subprocess.run([program], check=True, capture_output=True, text=True, cwd=folder,
                          env=environment).stdout.strip().split("\n")[-1]
    with open(os.path.join(folder, "outputs.txt")) as file:
        return said, file.read()


def main():
    arguments = sys.argv[1:]
    cycles = next((int(a) for a in arguments if a.isdigit()), 300)
    names = [a for a in arguments if not a.isdigit()] or ["c6288", "s38584", "mem_ctrl"]
    os.makedirs(RUN, exist_ok=True)
    one = build("simulate.ent", os.path.join(RUN, "one"), [])
    many = build("simulate.ent", os.path.join(RUN, "all"), ["--parallel"])
    failed = 0
    for name in names:
        folder = os.path.join(RUN, name)
        os.makedirs(folder, exist_ok=True)
        shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(folder, "circuit.aag"))
        subprocess.run([sys.executable, os.path.join(LOGIC, "compile.py"), name],
                       check=True, stdout=subprocess.DEVNULL)
        code = ["-I", os.path.join(ROOT, "build", "logic", "code", name)]
        ways = [("as entities", one, many, 0), ("as rows", one, many, 1),
                ("by what changed", one, many, 2),
                ("as code", build("coded.ent", os.path.join(folder, "coded_one"), [], code),
                 build("coded.ent", os.path.join(folder, "coded_all"), ["--parallel"], code), 0)]
        for way, single, parallel, number in ways:
            wanted = run(single, folder, cycles, number, None)
            apart = [str(threads or "all") for threads in (1, 4, 16, None)
                     if run(parallel, folder, cycles, number, threads) != wanted]
            failed += 1 if apart else 0
            print(f"{name}, {way}: " + ("the same on 1, 4, 16 and all threads" if not apart
                                        else "NOT the same on " + ", ".join(apart) + " threads"),
                  flush=True)
        shutil.rmtree(folder)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
