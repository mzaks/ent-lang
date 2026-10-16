# entsat: a SAT solver in ent-lang

Solves a problem in conjunctive normal form, read from a DIMACS file, by
conflict-driven clause learning, and prints the answer as SAT
competitions print it. Every model is held against the clauses read
before it is printed, and every unsatisfiable answer comes with a DRAT
proof, which drat-trim checks. This is phase 1 of the plan in the design
doc ("entsat: SAT and SMT solving in ent-lang"): the SAT core alone.

- `dimacs.ent`: the file into a buffer of literals, each clause ended by
  -1. A literal is its variable's number from 0 times two, plus one
  where negated.
- `sat.ent`: the solver. Its state is buffers sized from the problem
  (the arena of clauses, the watch lists, the trail, the variables) and
  one unique, `Search`; a round of the search is three systems, run by
  `main` until there is an answer:
  - `propagate`: unit propagation over two watched literals per clause,
    each watch with a blocker literal, as MiniSat has them;
  - `learn`: the first-UIP clause of a conflict, minimized as MiniSat
    minimizes it, its variables moved to the front of the decision
    queue (VMTF, in the order they had in it), the jump back, and the
    clause into the arena and the proof;
  - `decide`: the latest unassigned variable of the queue, with the
    value it last had.
  
  Restarts follow the Luby sequence (100 conflicts a unit). Every 2,000
  conflicts and 300 more each time, `reduce` deletes half of the learned
  clauses, those of the highest LBD, and moves the rest down the arena;
  `watch` then lays the watch lists out again.
- `entsat.ent`: the program, with room for 4 million variables.
- `fetch.sh`: SATLIB's random 3-SAT sets (uf/uuf 50 to 250 variables,
  4,999 problems) and drat-trim, into `build/sat`.
- `check/satlib.py`: runs entsat on those sets, checks each model again
  and each proof with drat-trim.

```sh
examples/sat/fetch.sh
ENT_CFLAGS=-march=native tools/ent build examples/sat/entsat.ent -o build/sat/entsat
examples/sat/check/satlib.py              # or some sets: uf250-1065 uuf250-1065
cd somewhere && cp problem.cnf . && .../build/sat/entsat
```

The program reads "problem.cnf" where it is started and writes
"proof.drat" there (binary DRAT). `Options` in `sat.ent` turns the proof
off and sets the restart unit.
