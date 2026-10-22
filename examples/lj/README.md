# Atoms by the Lennard-Jones potential in ent-lang

LAMMPS's "LJ melt" benchmark (its `bench/in.lj`): 32,000 atoms on an fcc
lattice at density 0.8442, started at temperature 1.44 so that the
lattice melts, the potential cut at 2.5, 100 steps of 0.005 by velocity
Verlet, in a box that wraps on every side. As in LAMMPS, every atom has
a list of the atoms within the cut and a skin of 0.3, made every 20
steps: the grid index `Cells` finds them (`docs/syntax.md`, Grids), `for
o, b: Atom near (a.x, a.y, a.z) within cut + skin in Cells`, each atom
seen as its nearest image. Every step each atom goes through its list.

- `lj.ent`: the atoms, the lattice, the velocities (no momentum, the
  temperature asked for), the lists, the forces and the steps, and the
  sums LAMMPS reports. With `Melt.half` a list has only the atoms whose
  number is above the atom's own, each pair once (a half list), and the
  other atom is given the opposite force (`Pull(o).fx += ..`, combined
  in the order of the atoms that give it); else each atom has all of
  its own (a full list) and gives nothing.
- `melt.ent`: the benchmark, of half lists (`melt_full.ent`: of full
  lists); every 50 steps `Step Temp E_pair TotEng Press` as LAMMPS writes
  them, and the total energy of the potential shifted to 0 at the cut. The velocities are drawn here, not as LAMMPS
  draws them, so after step 0 the numbers are others (at step 100 near
  LAMMPS's: a temperature of 0.76, E_pair -5.76).
- `check/melt.py`: at step 0 the energy an atom and the pressure are
  those of the lattice, worked out over one atom's neighbours (the
  energy is the -6.7733681 LAMMPS reports); and the shifted total energy
  stays within 1e-4 (the total energy moves by U(2.5) each time a pair
  crosses the cut: the liquid has more pairs within it than the lattice).
- `check/melt.c`: the same in plain C, the lists made in the index's
  order, which `test/Integration/lj_melt.test` compares to the bit, of
  half lists and of full ones.

```sh
tools/ent run --parallel examples/lj/melt.ent
examples/lj/check/melt.py
```

`reference/fetch.sh` fetches LAMMPS (stable_30Sep2026) into `build/lj`
and builds it native, with its OPENMP package (`-sf omp`). Its melt
says, at step 0, what `melt.ent` says to every digit (`-6.7733681
-4.6134356 -5.0197073`), and at step 100, of its own velocities,
`0.7574531 -5.7585055 -4.6223613 0.20726105`.

| 32,000 atoms, 100 steps (native, 2026-10-22) | one core | 12 threads |
|---|---|---|
| `melt.ent`, half lists | 1.36 s | 0.35 s |
| `melt_full.ent`, full lists | 2.16 s | 0.34 s |
| `check/melt.c`, half lists, full lists | 1.09 s, 1.69 s | |
| LAMMPS (half lists; `-sf omp` with threads) | 0.74 s | 0.12 s |
| before the lists: every atom of the 27 cells around, every step | 4.76 s | 0.76 s |

- Half lists are the faster on one core: half the pairs. On many threads
  the other atom's force is sent, and put on lists to be combined in the
  order of the atoms that sent it, so that the sums are the same on any
  number of threads: the three fields together, one entry of four words
  (`docs/syntax.md`), written where the list is without a call. So half
  lists are about as fast as full ones there (0.77 s before the entries
  were one and written without a call). LAMMPS's threads each add into a
  copy of the forces of their own, which differ in the last bits with
  the number of threads.
- Against its own C (1.19 s on the same day), `melt.ent` is 1.14 times
  slower on one core. Of its 13.6 ms a step, the forces are 8.4 and the
  lists 4.5 (the `near` scan twice, counting then filling, 1.2 and 3.3,
  and the index 0.4); `near` leaves out the rows of cells out of reach,
  and goes along the rest as runs of rows. The count's scan is
  vectorized, the fill's is not, as it writes. LAMMPS's lists take 1.2 ms
  a step, its pairs 5.9. Of the force loop's difference: the nearest
  image of every pair, which LAMMPS has no need of (its ghost atoms); the
  C without it takes 5.7 ms a step, and ent's `image` is made into
  selects where the C's stays branches (8.2 ms as selects in the C).
