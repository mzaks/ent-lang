# Doom in ent-lang

A port of Doom (the shareware episode, v1.9) that is to play the original
demos as the original does, with its things as entities and what moves
them as systems. It is being built; what is here so far:

- `fixed.ent`, `tables.ent`, `angles.ent`, `random.ent`: fixed point, the
  angle tables and what computes with them, and the random numbers.
- `check/`: programs that print what the port computes, to hold against
  the reference.
- `reference/`: what the port is compared against. `fetch.sh` fetches
  Chocolate Doom and the shareware WAD into `build/doom` and builds the
  first with `trace.patch`, which makes it write a line for each tic of a
  level; `compare.py` finds the first tic at which two such traces
  differ. `make_tables.py` wrote `tables.ent` and `random.ent` from the
  reference's C.

```sh
examples/doom/reference/fetch.sh demo1 demo2 demo3   # the traces to match
examples/doom/check/maths.sh                         # "the same: 24 lines"
```

The port follows the source of Doom as id Software released it and as
Chocolate Doom keeps it, so the files in this directory are under the GNU
General Public License, version 2 or later, as theirs are.
