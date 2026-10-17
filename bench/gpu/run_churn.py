#!/usr/bin/env python3
"""What it costs when the CPU touches what a `gpu` system goes through.

  python3 bench/gpu/run_churn.py

churn/churn.ent.in has a `gpu` system move a million entities every
frame; the CPU spawns SPAWN of them a frame into its archetype, and with
READ adds up a column it writes. Each variant is built by tools/ent,
native, without and with --gpu, and with --gpu run once more with
ENT_GPU_ADVISE=device (devices/gpu.c: the pages kept on the GPU, the CPU
reaching them there). Prints ms per frame; the sums must agree.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ENT = os.path.join(ROOT, "tools", "ent")
VARIANTS = [("0", "false"), ("1000", "false"), ("0", "true"),
            ("1000", "true")]


def main():
    with open(os.path.join(HERE, "churn", "churn.ent.in")) as source:
        template = source.read()
    env = dict(os.environ, ENT_CFLAGS="-march=native")
    print("| spawn a frame | CPU reads a column | cpu | gpu | gpu, advised |")
    print("|---|---|---|---|---|")
    bad = False
    with tempfile.TemporaryDirectory() as directory:
        for spawn, read in VARIANTS:
            program = os.path.join(directory, f"churn_{spawn}_{read}.ent")
            with open(program, "w") as out:
                out.write(template.replace("@SPAWN@", spawn)
                          .replace("@READ@", read))
            cells, sums = [], set()
            for flags, extra in (([], {}), (["--gpu"], {}),
                                 (["--gpu"], {"ENT_GPU_ADVISE": "device"})):
                exe = os.path.join(directory, "churn")
                subprocess.run([sys.executable, ENT, "build", program,
                                *flags, "-o", exe], check=True, env=env)
                out = subprocess.run([exe], check=True, capture_output=True,
                                     text=True, env=dict(env, **extra)).stdout
                m = re.search(r"ms_per_frame=([\d.]+) sum=(\S+)", out)
                cells.append(f"{float(m.group(1)):.3f}")
                sums.add(m.group(2))
            bad |= len(sums) != 1
            print(f"| {spawn} | {read} | " + " | ".join(cells) + " |")
    if bad:
        print("sums differ", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
