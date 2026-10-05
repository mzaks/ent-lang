// RUN: ent-opt %s -split-input-file -verify-diagnostics

ent.component @P (x: f32)
// expected-error @+1 {{parameter #0 has type '!ent.ref<@P>', which cannot be passed to C}}
ent.function @f(!ent.ref<@P>) -> f32

// -----

// expected-error @+1 {{gives a text, which C cannot give back yet}}
ent.function proc @name() -> !ent.text<8>

// -----

ent.system @s() {
  // expected-error @+1 {{references unknown function @nope}}
  ent.invoke @nope() : () -> ()
}

// -----

ent.function @twice(i32) -> i32
ent.system @s() {
  %x = arith.constant 1.0 : f32
  // expected-error @+1 {{argument types ('f32') do not match the parameters ('i32') of function @twice}}
  %y = ent.invoke @twice(%x) : (f32) -> i32
}

// -----

ent.function @twice(i32) -> i32
ent.system @s() {
  %x = arith.constant 1 : i32
  // expected-error @+1 {{result does not match what function @twice gives}}
  ent.invoke @twice(%x) : (i32) -> ()
}

// -----

ent.function proc @beep()
ent.system @s() {
  // expected-error @+1 {{calls @beep, a proc, without 'proc'}}
  ent.invoke @beep() : () -> ()
}

// -----

ent.function @one() -> i32
ent.system @s() {
  // expected-error @+1 {{is marked 'proc', but @one is not one}}
  %x = ent.invoke proc @one() : () -> i32
}

// -----

// A fn computes, but its call is still a system's to make.
ent.function @done() -> i1
ent.schedule @frame() {
}
ent.main {
  ent.loop {
    ent.call @frame()
    // expected-error @+1 {{must be inside 'ent.system'; conditions and the entry point only read uniques and compute}}
    %d = ent.invoke @done() : () -> i1
    ent.yield %d : i1
  }
}
