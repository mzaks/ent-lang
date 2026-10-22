#!/usr/bin/env python3
"""The LJ melt against what is known of it exactly.

  examples/lj/check/melt.py

Runs examples/lj/melt.ent (32,000 atoms, 100 steps) and holds its lines
(`Step Temp E_pair TotEng Press`, and the total energy of the potential
shifted to 0 at the cut) against:

- step 0: the atoms on the fcc lattice, so the energy an atom and the
  pressure are sums over one atom's neighbours, worked out here (the
  energy is the -6.7733681 that LAMMPS says of its own melt);
- every step: the shifted total energy stays, as velocity Verlet keeps
  it, within 1e-4 of where it started (the total energy itself moves by
  U(cut) each time a pair crosses the cut, as the liquid has more pairs
  within it than the lattice has).

Needs the compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for
tools/ent).
"""

import itertools
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROGRAM = os.path.join(ROOT, "examples", "lj", "melt.ent")


def lattice(density=0.8442, cells=20, temperature=1.44, cut=2.5):
    """The energy an atom and the pressure of the fcc lattice."""
    a = 1.0
    for _ in range(60):
        a = a - (a * a * a - 4.0 / density) / (3.0 * a * a)
    atoms = 4 * cells ** 3
    volume = (a * cells) ** 3
    energy = virial = 0.0
    basis = [(0, 0, 0), (0.5, 0.5, 0), (0.5, 0, 0.5), (0, 0.5, 0.5)]
    reach = int(cut / a) + 2
    for i, j, k in itertools.product(range(-reach, reach + 1), repeat=3):
        for b in basis:
            x, y, z = (i + b[0]) * a, (j + b[1]) * a, (k + b[2]) * a
            r2 = x * x + y * y + z * z
            if 0 < r2 <= cut * cut:
                r6i = 1.0 / r2 ** 3
                energy += 2.0 * r6i * (r6i - 1.0)
                virial += 0.5 * 48.0 * r6i * (r6i - 0.5)
    dof = 3 * atoms - 3
    return energy, (dof * temperature + atoms * virial) / (3.0 * volume)


def main():
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "run", "--parallel", PROGRAM],
                         capture_output=True, text=True, check=True).stdout
    rows = [list(map(float, line.split())) for line in out.split("\n") if line.strip()]
    energy, pressure = lattice()
    failed = 0
    step0 = rows[0]
    for name, got, want in (("E_pair", step0[2], energy), ("Press", step0[4], pressure)):
        ok = abs(got - want) <= 1e-6 * abs(want)
        print(f"step 0 {name}: {got} (the lattice's {want:.7f}){'' if ok else '  OFF'}")
        failed += not ok
    start = step0[5]
    for row in rows:
        ok = abs(row[5] - start) <= 1e-4
        print(f"step {int(row[0])}: T {row[1]} TotEng {row[3]} shifted {row[5]}{'' if ok else '  OFF'}")
        failed += not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
