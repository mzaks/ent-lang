#!/usr/bin/env python3
"""Every way of running a circuit gives the same outputs (the spec's
fifth check).

  examples/logic/prepare.py all
  examples/logic/check/agree.py [cycles] [circuits]

For each prepared circuit (all, or those named) the outputs of so many
cycles (1,000) are written four times: by the model with its gates as
entities, all of them gone through or only those whose inputs changed,
with them as rows of buffers, and by the circuit written out as code and
compiled (compile.py, coded.ent). The four files are to be
the same byte for byte, and so is the number each makes of all outputs
of all 64 runs. check/equal.py holds the first against Icarus Verilog.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
LOGIC = os.path.join(ROOT, "examples", "logic")
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
RUN = os.path.join(ROOT, "build", "logic", "run")


def build(source, built, more=()):
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "-I", LOGIC,
                    *more, os.path.join(LOGIC, source), "-o", built],
                   check=True, env=environment)
    return built


def main():
    arguments = sys.argv[1:]
    cycles = next((int(a) for a in arguments if a.isdigit()), 1000)
    names = [a for a in arguments if not a.isdigit()] or sorted(
        name[:-4] for name in os.listdir(CIRCUITS) if name.endswith(".aag"))
    os.makedirs(RUN, exist_ok=True)
    simulate = build("simulate.ent", os.path.join(RUN, "simulate"))
    failed = 0
    for name in names:
        folder = os.path.join(RUN, "agree_" + name)
        os.makedirs(folder, exist_ok=True)
        shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(folder, "circuit.aag"))
        subprocess.run([sys.executable, os.path.join(LOGIC, "compile.py"), name],
                       check=True, stdout=subprocess.DEVNULL)
        coded = build("coded.ent", os.path.join(folder, "coded"),
                      ["-I", os.path.join(ROOT, "build", "logic", "code", name)])
        seen = []
        for program, way in ((simulate, 0), (simulate, 1), (simulate, 2), (coded, 0)):
            with open(os.path.join(folder, "run.txt"), "w") as file:
                file.write(f"{cycles} 1 1 {way}\n")
            said = subprocess.run([program], check=True, capture_output=True, text=True,
                                  cwd=folder).stdout.strip().split("\n")[-1]
            with open(os.path.join(folder, "outputs.txt")) as file:
                seen.append((said, file.read()))
        shutil.rmtree(folder)
        same = seen[0] == seen[1] == seen[2] == seen[3]
        failed += 0 if same else 1
        print(f"{name}: {'the same four ways' if same else 'NOT the same'} ({seen[0][0]})",
              flush=True)
    print(f"{len(names)} circuits, {failed} not the same")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
