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

`reference/fetch.sh` fetches LAMMPS (stable_30Sep2026) into `build/lj`
and builds it native, with its OPENMP package (`-sf omp`). Its melt
says, at step 0, what `melt.ent` says to every digit (`-6.7733681
-4.6134356 -5.0197073`), and at step 100, of its own velocities,
`0.7574531 -5.7585055 -4.6223613 0.20726105`.

| 32,000 atoms, 100 steps (native, 2026-10-22) | one core | 4 threads | 12 threads |
|---|---|---|---|
| `melt.ent` | 4.76 s | | 0.76 s |
| `check/melt.c` (the same work) | 4.25 s | | |
| LAMMPS | 0.74 s (0.65 with `-sf omp`) | 0.18 s | 0.12 s |

LAMMPS does less work, not the same work faster: a list for each atom
of the atoms within the cut and a skin (2.8), made every 20 steps, of
each pair once (half the list, the force given to both); `melt.ent`
looks at every atom in the 27 cells around each atom (some 390), every
step, and works out each pair twice. Its 81% of the time in the pair
forces is the same share as LAMMPS's.

Against its own C, `melt.ent` is 12% slower on one core (23.9 billion
cycles to 21.2): the image of an atom is chosen without a branch, where
the C's branches are well foreseen. (17% before the rows of `near` were
read by row numbers, which are not asked whether they are rows.)
