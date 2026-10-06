// RUN: ent-opt %s -split-input-file --ent-lower-to-loops -verify-diagnostics -o /dev/null
// What a sorted tree needs of the archetypes, which are known once those
// that spawns ask for are there.

// A tree across archetypes is sorted in each; one in two sorted trees is
// not.
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
