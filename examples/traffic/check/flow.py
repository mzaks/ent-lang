#!/usr/bin/env python3
"""The ring road's flows against the exact ones.

  examples/traffic/check/flow.py

Runs examples/traffic/flow.ent (a sweep of densities on roads of 1,000
cells, 1,000 steps measured after 1,000 to settle) and holds its flows
against what is known of the Nagel-Schreckenberg model exactly:

- without braking, every step's flow is min(density * vmax, 1 - density),
  for vmax 1 (Wolfram's rule 184) and 5: no step off, and the mean the
  same to the last digit printed;
- with vmax 1 and a chance p to brake, the flow of the endless road is
  (1 - sqrt(1 - 4 (1 - p) density (1 - density))) / 2 (Schreckenberg,
  Schadschneider, Nagel and Ito, 1995): within 3 percent of it.

With vmax 5 and braking there is no formula; the script prints where the
flow is highest. Needs the compiler built (build/bin) and its LLVM
(LLVM_PREFIX, as for tools/ent).
"""

import csv
import io
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROGRAM = os.path.join(ROOT, "examples", "traffic", "flow.ent")


def main():
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "run", "--parallel", PROGRAM],
                         capture_output=True, text=True, check=True).stdout
    rows = list(csv.DictReader(io.StringIO(out)))
    failed = 0
    for r in rows:
        vmax, brake = int(r["vmax"]), float(r["brake"])
        density, flow = float(r["density"]), float(r["flow"])
        if brake == 0.0:
            want = min(density * vmax, 1.0 - density)
            ok = int(r["off"]) == 0 and abs(flow - want) < 1e-6
        elif vmax == 1:
            want = (1.0 - math.sqrt(1.0 - 4.0 * (1.0 - brake) * density * (1.0 - density))) / 2.0
            ok = abs(flow - want) <= 0.03 * want
        else:
            continue
        if not ok:
            failed += 1
            print(f"off: vmax {vmax} brake {brake} density {density}: flow {flow}, exactly {want:.6f}, "
                  f"{r['off']} steps off")
    held = sum(1 for r in rows if float(r["brake"]) == 0.0 or r["vmax"] == "1")
    best = max((r for r in rows if r["vmax"] == "5" and r["brake"] != "0.0"), key=lambda r: float(r["flow"]))
    print(f"vmax 5, brake {best['brake']}: highest flow {best['flow']} at density {best['density']}")
    print(f"{held - failed} of {held} flows as they are exactly")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
