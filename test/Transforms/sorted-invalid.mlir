// RUN: ent-opt %s -split-input-file --ent-lower-to-loops -verify-diagnostics -o /dev/null
// What a sorted tree needs of the archetypes, which are known once those
// that spawns ask for are there.

module attributes {ent.default_capacity = 4 : i64} {
ent.component @N (v: f32)
ent.component @M (w: f32)
// expected-error @+1 {{is 'sorted', but its entities can be in 2 archetypes (@A, @N_M); a tree across archetypes cannot be sorted yet}}
ent.relation @R () from @N to @N tree sorted capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s() {
  %v = arith.constant 0.0 : f32
  %e = ent.spawn (@N, @M)(%v, %v) : f32, f32
}
}

// -----

ent.component @N (v: f32)
ent.component @M (w: f32)
// expected-error @+1 {{is 'sorted', but @A, which holds its entities, does not always hold @M, which its targets have}}
ent.relation @R () from @N to @M tree sorted capacity 4
ent.archetype @A (@N, optional @M) capacity 4

// -----

// An archetype's rows have one order.
ent.component @N (v: f32)
// expected-error @+1 {{is 'sorted', and so is @Q; both have their entities in @A, whose rows have one order}}
ent.relation @R () from @N to @N tree sorted capacity 4
// expected-note @+1 {{the other tree}}
ent.relation @Q () from @N to @N tree sorted capacity 4
ent.archetype @A (@N) capacity 4

// -----

// Any number of other relations are no trouble: edges hold ids, not rows.
ent.component @N (v: f32)
ent.relation @R () from @N to @N tree sorted capacity 4
ent.relation @Q () from @N to @N tree capacity 4
ent.relation @P (w: f32) capacity 4
ent.archetype @A (@N) capacity 4
