// RUN: ent-opt %s -split-input-file -verify-diagnostics

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  // expected-error @+1 {{'ent.edges' op must be inside an 'ent.query'}}
  ent.edges @R out (%e: !ent.ref<@R>, %t: !ent.entity) {
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    // expected-error @+1 {{direction must be 'out' or 'in', got 'up'}}
    ent.edges @R up (%e: !ent.ref<@R>, %t: !ent.entity) {
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.relation @Q (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    // expected-error @+1 {{ref names @Q, but the loop visits @R}}
    ent.edges @R out (%e: !ent.ref<@Q>, %t: !ent.entity) {
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @R out (%e: !ent.ref<@R>, %t: !ent.entity) {
      // expected-error @+1 {{cannot be nested in another 'ent.edges'}}
      ent.edges @R out (%f: !ent.ref<@R>, %u: !ent.entity) {
      }
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() reads [@N, @R] {
  ent.query (%n: !ent.ref<@N>) {
    // expected-error @+1 {{writes the edges of @R but system @s does not declare it in 'writes'}}
    ent.edges @R out (%e: !ent.ref<@R, mut>, %t: !ent.entity) {
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    // expected-error @+1 {{visits @R both ways in one query and one loop writes the edges}}
    ent.edges @R out (%e: !ent.ref<@R, mut>, %t: !ent.entity) {
    }
    // expected-note @+1 {{other loop here}}
    ent.edges @R in (%f: !ent.ref<@R>, %u: !ent.entity) {
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @R out (%e: !ent.ref<@R>, %t: !ent.entity) {
      %w = ent.get %e "w" : !ent.ref<@R> -> f32
      // expected-error @+1 {{requires a mutable reference}}
      ent.set %e "w", %w : !ent.ref<@R>, f32
    }
  }
}

// -----

ent.component @N (v: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @R out (%e: !ent.ref<@R>, %t: !ent.entity) {
      // expected-error @+1 {{relation @R has no field 'x'}}
      %x = ent.get %e "x" : !ent.ref<@R> -> f32
    }
  }
}

// -----

ent.component @N (v: f32)
ent.resource @Sum (s: f32)
ent.relation @R (w: f32) capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @R out (%e: !ent.ref<@R>, %t: !ent.entity) {
      %w = ent.get %e "w" : !ent.ref<@R> -> f32
      // expected-error @+1 {{inside 'ent.edges' is not supported yet}}
      ent.accumulate @Sum "s" add %w : f32
    }
  }
}

// -----

ent.relation @R (w: f32) capacity 4
ent.system @s(%a: !ent.entity) {
  // expected-error @+1 {{initialises 0 fields, but @R has 1}}
  ent.connect @R %a, %a ()
}

// -----

ent.component @N (v: f32)
ent.relation @R () capacity 4
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    %self = ent.entity
    %c = arith.constant 0 : index
    %one = arith.constant 1 : index
    scf.for %i = %c to %one step %one {
      // expected-error @+1 {{must not be inside a loop ('scf.for') in a query}}
      ent.connect @R %self, %self ()
    }
  }
}

// -----

ent.relation @R () capacity 4
ent.system @s(%a: !ent.entity) reads [@R] {
  // expected-error @+1 {{connects @R but system @s does not declare it in 'writes'}}
  ent.connect @R %a, %a ()
}

// -----

ent.component @N (v: f32)
ent.system @s() {
  ent.query (%n: !ent.ref<@N>) {
    // expected-error @+1 {{must be inside an 'ent.edges'}}
    ent.disconnect
  }
}
