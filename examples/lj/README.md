# Atoms by the Lennard-Jones potential in ent-lang

LAMMPS's "LJ melt" benchmark (its `bench/in.lj`): 32,000 atoms on an fcc
lattice at density 0.8442, started at temperature 1.44 so that the
lattice melts, the potential cut at 2.5, 100 steps of 0.005 by velocity
Verlet, in a box that wraps on every side. As in LAMMPS, every atom has
a list of the atoms within the cut and a skin of 0.3, made every 20
steps: the grid index `Cells` finds them (`docs/syntax.md`, Grids), `for
o, b: Atom near (a.x, a.y, a.z) within cut + skin in Cells`, each atom
seen as its nearest image. Every step each atom goes through its list.
And as in LAMMPS, the atoms are put back in the box only where the lists
are made: until then a pair apart within the box stays so and needs no
image; those seen across the side of the box are a list of their own
(`Crossing`), whose other atom is seen as its nearest image.

- `lj.ent`: the atoms, the lattice, the velocities (no momentum, the
  temperature asked for), the lists, the forces and the steps, and the
  sums LAMMPS reports. With `Melt.half` a list has only the atoms after
  the atom in the index (`after me`), each pair once (a half list), and the
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

| 32,000 atoms, 100 steps (native, 2026-10-22) | one core | all threads |
|---|---|---|
| `melt.ent`, half lists | 0.90 s | 0.167 s (23) |
| `melt_full.ent`, full lists | 1.61 s | 0.180 s (23) |
| `check/melt.c`, half lists, full lists | 1.09 s, 1.65 s | |
| LAMMPS (half lists; `-sf omp` with threads) | 0.74 s | 0.12 s (12) |
| before the lists: every atom of the 27 cells around, every step | 4.76 s | 0.76 s (12) |

- Half lists are the faster on one core: half the pairs. On many threads
  the other atom's force is sent, and put on lists to be combined in the
  order of the atoms that sent it, so that the sums are the same on any
  number of threads: the three fields together, one entry of four words
  (`docs/syntax.md`), written where the list is without a call. So half
  lists are a little slower than full ones there (0.77 s before the
  entries were one and written without a call). LAMMPS's threads each add into a
  copy of the forces of their own, which differ in the last bits with
  the number of threads.
- On one core `melt.ent` is faster than its own C, which goes over
  all the atoms around and drops those before it. Of its 9.0 ms a step,
  the forces are 6.4 and the lists 2.1 (the `near` scan twice, counting
  then filling, 0.7 and 1.4, and the index 0.05); `near` leaves out the
  rows of cells out of reach and, with `after me`, the rows before the
  atom's own, and goes along the rest as runs of rows. The count's scan
  is done many rows at once; the fill's, as it writes, by blocks of 64
  rows, which are within reach one bit each, many at once, then the
  atoms of those bits (1.9 ms a step by a branch a row). With
  `ENT_CFLAGS='-march=native -mno-gather'` the fill takes 1.0 and the
  melt 0.84 s (LLVM loads x, y and z of a row with one gather). LAMMPS's
  lists take 1.2 ms a step, its pairs 5.9.
- On 12 threads, of 2.0 ms a step: the force loop 1.0, combining what
  it sent to `Pull` 0.4 (64 groups of the atoms at once, each in the
  order of the lists, counted and emptied on the threads too), the lists
  0.4, the index 0.06 (put in order on many threads, by cell and row
  before); on one thread 0.2 in all.
- Tried (2026-10-22), not kept: no sends from the force loop, each
  pair's force written at its row (`Given`) and each atom adding up
  those of its pairs through edges into it (a relation `Push`, made with
  the lists; edges go by source, the order the sends land in, so the
  same bits). On 12 threads 0.51 s where 0.196: the sum reads `Given`
  (30 MB) row by row as the edges point, 1.7 ms a step, where combining
  the sends reads the lists in order and adds into `Pull` (768 KB, in
  the cache), 0.4; the windows that write `Given` take their rows each
  step (the force loop 1.26 ms where 0.95); and making 1.25 million
  edges every 20 steps, 24 ms each.
- Before the atoms stayed out of the box between the lists, every pair
  was seen as the nearest image of its other atom: the forces took 8.5
  ms a step, and ent's `image` is selects where the C's is branches
  (8.2 ms as selects in the C, 7.3 as branches).
