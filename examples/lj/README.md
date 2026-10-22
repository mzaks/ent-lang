# Atoms by the Lennard-Jones potential in ent-lang

LAMMPS's "LJ melt" benchmark (its `bench/in.lj`): 32,000 atoms on an fcc
lattice at density 0.8442, started at temperature 1.44 so that the
lattice melts, the potential cut at 2.5, 100 steps of 0.005 by velocity
Verlet, in a box that wraps on every side. Each atom's force is of the
atoms within the cut of it, which the grid index `Cells` finds
(`docs/syntax.md`, Grids): `for o, b: Atom near (a.x, a.y, a.z) within
cut in Cells`, each atom seen as its nearest image.

- `lj.ent`: the atoms, the lattice, the velocities (no momentum, the
  temperature asked for), the forces and the steps, and the sums LAMMPS
  reports.
- `melt.ent`: the benchmark; every 50 steps `Step Temp E_pair TotEng
  Press` as LAMMPS writes them, and the total energy of the potential
  shifted to 0 at the cut. The velocities are drawn here, not as LAMMPS
  draws them, so after step 0 the numbers are others (at step 100 near
  LAMMPS's: a temperature of 0.76, E_pair -5.76).
- `check/melt.py`: at step 0 the energy an atom and the pressure are
  those of the lattice, worked out over one atom's neighbours (the
  energy is the -6.7733681 LAMMPS reports); and the shifted total energy
  stays within 1e-4 (the total energy moves by U(2.5) each time a pair
  crosses the cut: the liquid has more pairs within it than the lattice).
- `check/melt.c`: the same in plain C, the atoms found in the index's
  order, which `test/Integration/lj_melt.test` compares to the bit.

```sh
tools/ent run --parallel examples/lj/melt.ent
examples/lj/check/melt.py
```

| 32,000 atoms, 100 steps (native) | |
|---|---|
| `check/melt.c`, one core | 4.25 s |
| `melt.ent`, one core | 4.93 s |
| `melt.ent`, 12 threads | 0.76 s |
