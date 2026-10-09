// RUN: ent-opt %s -split-input-file -verify-diagnostics

// expected-error @+1 {{declares access to unknown component, resource, relation or archetype @Nope}}
ent.extern @e() reads [@Nope]

// -----

ent.component @P (x: f32)
// expected-error @+1 {{lists @P more than once; 'writes' already implies read access}}
ent.extern @e() reads [@P] writes [@P]

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 1
// expected-error @+1 {{lists archetype @A in 'reads'; archetypes are declared in 'writes', by systems that spawn or despawn their entities}}
ent.extern @e() reads [@A]

// -----

ent.component @P (x: f32)
// expected-error @+1 {{parameter #1 is a component reference; references can only be bound by 'ent.query'}}
ent.extern @e(f32, !ent.ref<@P>)

// -----

ent.extern @e(f32)
ent.schedule @f() {
  // expected-error @+1 {{argument types () do not match the parameters ('f32') of system @e}}
  ent.run @e()
}

// -----

ent.schedule @f() {
  // expected-error @+1 {{references unknown system @f}}
  ent.run @f()
}

// -----

ent.schedule @f(%dt: f32) {
}
ent.main {
  // expected-error @+1 {{argument types () do not match the parameters ('f32') of schedule @f}}
  ent.call @f()
}

// -----

ent.system @s() {
}
ent.main {
  // expected-error @+1 {{references unknown schedule @s}}
  ent.call @s()
}

// -----

// expected-note @+1 {{the other is here}}
ent.main {
}
// expected-error @+1 {{is the second entry point; a program has one}}
ent.main {
}

// -----

ent.resource @R (v: i32)
ent.main {
  // expected-error @+1 {{body must end in 'ent.yield' of nothing (repeat forever) or of an i1 (stop if true)}}
  ent.loop {
    %v = ent.read @R "v" : i32
    ent.yield %v : i32
  }
}

// -----

ent.resource @R (v: i32)
ent.main {
  %v = ent.read @R "v" : i32
  // expected-error @+1 {{'ent.write' op is not allowed in 'ent.main'; the entry point calls schedules, loops, reads resources and computes with ops free of side effects}}
  ent.write @R "v", %v : i32
}

// -----

ent.main {
  %c = arith.constant true
  // expected-error @+1 {{'scf.if' op is not allowed in 'ent.main'; the entry point calls schedules, loops, reads resources and computes with ops free of side effects}}
  scf.if %c {
  }
}

// -----

ent.schedule @f() {
  // expected-error @+1 {{'ent.call' op expects parent op to be one of 'ent.main, ent.loop'}}
  ent.call @f()
}

