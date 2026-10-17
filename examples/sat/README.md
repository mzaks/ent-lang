# entsat: a SAT solver in ent-lang

Solves a problem in conjunctive normal form, read from a DIMACS file, by
conflict-driven clause learning, and prints the answer as SAT
competitions print it. Every model is held against the clauses read
before it is printed, and every unsatisfiable answer comes with a DRAT
proof, which drat-trim checks. These are phases 1 and 2 of the plan in
the design doc ("entsat: SAT and SMT solving in ent-lang"): the SAT
core, and making it fast.

- `dimacs.ent`: the file into a buffer of literals, each clause ended by
  -1. A literal is its variable's number from 0 times two, plus one
  where negated.
- `satprep.ent`: bounded variable elimination with subsumption and
  strengthening before the search, as MiniSat's SimpSolver does them.
  The variables come from a heap, the cheapest to eliminate first (of
  two alike the lower first, which on a circuit is the order of its
  gates), and go back in when their clauses change; one goes where its
  resolvents are no more than the clauses they replace. Each resolvent
  is held against the other clauses, and when the heap is empty the
  clauses on the variables of the resolvents are held against the rest.
  The resolvents go into the proof; the removed clauses are kept, and a
  model is extended to the eliminated variables and held against the
  clauses as read.
- `sat.ent`: the solver. Its state is buffers sized from the problem
  (the arena of clauses, the watch lists, the trail, the variables) and
  one unique, `Search`; a round of the search is a schedule of systems,
  run by `main` until there is an answer:
  - `propagate`: unit propagation over two watched literals per clause,
    each watch with a blocker literal, as MiniSat has them; a clause of
    two is propagated by its watch alone;
  - `learn`: the first-UIP clause of a conflict, minimized as MiniSat
    minimizes it, the jump back, and the clause into the arena and the
    proof;
  - `reduce` and `watch`: every 300 times the square root of the
    reductions so far conflicts, three quarters of the learned clauses
    that may go go (LBD 3 or more, and not those of LBD 6 or less that
    conflicts used since); the arena is moved down and made shorter, and
    the watch lists are laid out again;
  - `decide`: the next variable, with the value it last had.

  The search goes between two modes, as CaDiCaL does: focused, with
  decisions in a VMTF queue (the variables of a conflict moved to its
  front, in the order they had in it) and Glucose's restarts (where the
  recent LBDs are 10% above the long-run ones); and stable, with VSIDS
  scores in a heap and restarts after 1,024 times the Luby sequence. The
  first 1,000 conflicts are focused, and each mode then lasts twice as
  long as the one before. A restart keeps the levels it would decide
  again, and a variable in no clause is not decided.
- `entsat.ent`: the program; `room.ent` gives it room for 4 million
  variables and a file of 256 MB.
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
"proof.drat" there (binary DRAT). `Options` in `sat.ent` and `Prep` in
`satprep.ent` hold the settings: the proof, the restarts and modes, the
reductions, a time limit (`Options.seconds`), the elimination's bounds.
`bench/sat` times it against MiniSat and CaDiCaL on SAT Competition
problems.
