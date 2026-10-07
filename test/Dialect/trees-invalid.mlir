// RUN: ent-opt %s -split-input-file -verify-diagnostics

ent.component @N (v: f32)
ent.relation @R () capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s() {
  // expected-error @+1 {{cascades along @R, which is not declared a 'tree'}}
  ent.query (%n: !ent.ref<@N>) cascade @R {
  }
}

// -----

ent.component @N (v: f32)
ent.component @M (w: f32)
ent.relation @R () capacity 4
ent.system @s() {
  // expected-error @+1 {{binds up @R, which is not declared a 'tree'}}
  ent.query (%n: !ent.ref<@N>, %m: !ent.ref<@M, up @R>) {
  }
}

// -----

ent.component @N (v: f32)
ent.system @s() {
  // expected-error @+1 {{cascades along unknown relation @R}}
  ent.query (%n: !ent.ref<@N>) cascade @R {
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() reads [@N] {
  // expected-error @+1 {{binds up @R but system @s does not declare it in 'reads' or 'writes'}}
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, up @R>) {
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s() {
  // expected-error @+1 {{is 'leaves first' without a 'cascade'}}
  ent.query (%n: !ent.ref<@N>) attributes {leaves_first} {
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  // expected-error @+1 {{binds component @N up @R more than once}}
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, up @R>,
             %q: !ent.ref<@N, up @R>) {
  }
}

// -----

// An ancestor is another entity: only its ancestors being visited first
// makes reading what the query writes independent of the order.
ent.component @N (v: f32, w: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  // expected-error @+1 {{reads @N "v" of an ancestor up @R and changes it; which entities see the old value would depend on iteration order ('cascade @R' visits ancestors first)}}
  ent.query (%n: !ent.ref<@N, mut>, %p: !ent.ref<@N, up @R>) {
    %v = ent.get %p "v" : !ent.ref<@N, up @R> -> f32
    // expected-note @+1 {{changed here}}
    ent.set %n "v", %v : !ent.ref<@N, mut>, f32
  }
}

// -----

// Another field is no trouble, nor is another tree's order any help.
ent.component @N (v: f32, w: f32)
ent.relation @R () tree capacity 4
ent.relation @Q () tree capacity 4
ent.system @fine() {
  ent.query (%n: !ent.ref<@N, mut>, %p: !ent.ref<@N, up @R>) {
    %v = ent.get %p "v" : !ent.ref<@N, up @R> -> f32
    ent.set %n "w", %v : !ent.ref<@N, mut>, f32
  }
}
ent.system @s() {
  // expected-error @+1 {{reads @N "v" of an ancestor up @R and changes it}}
  ent.query (%n: !ent.ref<@N, mut>, %p: !ent.ref<@N, up @R>) cascade @Q {
    %v = ent.get %p "v" : !ent.ref<@N, up @R> -> f32
    // expected-note @+1 {{changed here}}
    ent.set %n "v", %v : !ent.ref<@N, mut>, f32
  }
}

// -----

ent.component @N (v: f32)
ent.component @M (w: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N, optional @M) capacity 4
ent.system @s() {
  // expected-error @+1 {{reads @M of an ancestor up @R and changes it}}
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@M, up @R>) {
    // expected-note @+1 {{changed here}}
    ent.remove @M
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) cascade @R {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{in a cascading query is not supported yet}}
    %e = ent.spawn @A(%v) : f32
  }
}

// -----

// A value sent to an entity by its id lands when the sender's depth is
// through: the query may not read that field, of any entity.
ent.component @N (v: f32, w: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s(%to: !ent.entity) {
  ent.query (%n: !ent.ref<@N>) cascade @R {
    // expected-note @+1 {{read or set here}}
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    // expected-error @+1 {{in a cascading query combines into a field the query also reads or sets}}
    ent.apply %to @N "v" add %v : f32
  }
}

// -----

// Another field is fine, and so are despawning and adding into a
// resource the query does not read.
ent.component @N (v: f32, w: f32)
ent.resource @Total (value: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N) capacity 4
ent.system @fine(%to: !ent.entity) {
  ent.query (%n: !ent.ref<@N>) cascade @R leaves first {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    ent.apply %to @N "w" add %v : f32
    ent.accumulate @Total "value" add %v : f32
    ent.despawn
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.archetype @A (@N) capacity 4
ent.system @s() {
  // expected-error @+1 {{cascades and is reactive, which is not supported yet}}
  ent.query (%n: !ent.ref<@N>) cascade @R on [changed @N "v"] {
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N, mut>, %p: !ent.ref<@N, up @R>) {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{uses component reference #1; a reference 'up' a relation may only be used by 'ent.get' and 'ent.combine'}}
    ent.set %p "v", %v : !ent.ref<@N, up @R>, f32
  }
}

// -----

// An ancestor's field is combined into, not set.
ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, mut, up @R>) cascade @R {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{uses component reference #1; a reference 'up' a relation may only be used by 'ent.get' and 'ent.combine'}}
    ent.set %p "v", %v : !ent.ref<@N, mut, up @R>, f32
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, up @R>) cascade @R {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{requires a mutable reference, got '!ent.ref<@N, up @R>'}}
    ent.combine %p "v" add %v : !ent.ref<@N, up @R>, f32
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N, mut>) {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{uses component reference #0; references may only be used by 'ent.get' and 'ent.set'}}
    ent.combine %n "v" add %v : !ent.ref<@N, mut>, f32
  }
}

// -----

// Without an order the values would wait for the query's end.
ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, mut, up @R>) {
    %v = arith.constant 1.0 : f32
    // expected-error @+1 {{combines into an ancestor up @R in a query that does not cascade along it, which is not supported yet}}
    ent.combine %p "v" add %v : !ent.ref<@N, mut, up @R>, f32
  }
}

// -----

// Entities of one depth would see what others of it sent.
ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, mut, up @R>)
      cascade @R leaves first {
    // expected-note @+1 {{read here}}
    %v = ent.get %p "v" : !ent.ref<@N, mut, up @R> -> f32
    // expected-error @+1 {{combines into @N "v" of an ancestor in a query that reads it of other entities; which values those see would depend on the order}}
    ent.combine %p "v" add %v : !ent.ref<@N, mut, up @R>, f32
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R () tree capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, mut, up @R>)
      cascade @R leaves first {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    // expected-error @+1 {{combines @N "v" with 'add', but the query also combines it with 'max'; the result would depend on the order}}
    ent.combine %p "v" add %v : !ent.ref<@N, mut, up @R>, f32
    // expected-note @+1 {{other rule here}}
    ent.combine %p "v" max %v : !ent.ref<@N, mut, up @R>, f32
  }
}

// -----

ent.component @N (v: f32, on: i1)
ent.relation @R () tree capacity 4
ent.system @s() reads [@N, @R] {
  // expected-error @+1 {{binds @N mutably but system @s does not declare it in 'writes'}}
  ent.query (%n: !ent.ref<@N>, %p: !ent.ref<@N, mut, up @R>)
      cascade @R leaves first {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    ent.combine %p "v" add %v : !ent.ref<@N, mut, up @R>, f32
  }
}

// -----

ent.component @N (v: f32)
// expected-error @+1 {{is 'sorted' but not a 'tree'; only a tree gives its entities an order}}
ent.relation @R () from @N to @N sorted capacity 4

// -----

// Which archetypes are sorted must be known.
ent.component @N (v: f32)
// expected-error @+1 {{is 'sorted' but does not say what its targets have ('to'): which archetypes it sorts must be known}}
ent.relation @R () from @N tree sorted capacity 4
