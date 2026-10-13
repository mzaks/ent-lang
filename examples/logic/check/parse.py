#!/usr/bin/env python3
"""The circuits are read as their files have them (the spec's first
check).

  examples/logic/fetch.sh
  examples/logic/prepare.py all
  examples/logic/check/parse.py [circuits]

Builds check/parse_check.ent and runs it for each prepared circuit (all
of build/logic/circuits, or those named): the model must have as many
inputs, flip-flops, outputs and ANDs as the file's first line says, every
AND after both its inputs, and every input of an AND and of a flip-flop
an entity. Says each circuit's numbers and how many levels it has.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
RUN = os.path.join(ROOT, "build", "logic", "run")


def main():
    names = sys.argv[1:] or sorted(name[:-4] for name in os.listdir(CIRCUITS)
                                   if name.endswith(".aag"))
    os.makedirs(RUN, exist_ok=True)
    program = os.path.join(RUN, "parse_check")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
                    "-I", os.path.join(HERE, ".."), os.path.join(HERE, "parse_check.ent"),
                    "-o", program], check=True, env=environment)
    failed = 0
    for name in names:
        shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(RUN, "circuit.aag"))
        with open(os.path.join(RUN, "circuit.aag")) as file:
            _, most, inputs, flops, outputs, gates = file.readline().split()
        said = subprocess.run([program], capture_output=True, text=True, cwd=RUN).stdout
        words = said.split()
        seen = dict(zip(words[0::2], words[1::2]))
        wanted = {"read": "true", "ordered": "true", "inputs": inputs, "flops": flops,
                  "outputs": outputs, "gates": gates, "early": "0", "unset": "0"}
        wrong = [f"{key} {seen.get(key)} (file: {value})" for key, value in wanted.items()
                 if seen.get(key) != value]
        failed += 1 if wrong else 0
        print(f"{name}: {inputs} inputs, {flops} flip-flops, {outputs} outputs, "
              f"{gates} ANDs, {seen.get('levels')} levels" +
              ("" if not wrong else "  WRONG: " + ", ".join(wrong)))
    print(f"{len(names)} circuits, {failed} not as their files have them")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
