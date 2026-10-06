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
    // expected-error @+1 {{must be inside 'ent.system' or a function's body; conditions and the entry point only read uniques and compute}}
    %d = ent.invoke @done() : () -> i1
    ent.yield %d : i1
  }
}

// -----

// expected-error @+1 {{is a proc with a body, which is not supported yet}}
ent.function proc @beep(%x: i32) -> i32 {
  ent.yield %x : i32
}

// -----

// expected-error @+1 {{has a body, so it gives a value ('-> type')}}
ent.function @nothing(%x: i32) {
  ent.yield
}

// -----

// expected-error @+1 {{names its parameters, so it needs a body}}
ent.function @twice(%x: i32) -> i32

// -----

// expected-error @+1 {{names all of its parameters or none}}
ent.function @sum(%x: i32, i32) -> i32 {
  ent.yield %x : i32
}

// -----

ent.component @P (x: f32)
// expected-error @+1 {{parameter #0 is a component reference; references can only be bound by 'ent.query'}}
ent.function @x(%p: !ent.ref<@P>) -> f32 {
  %x = arith.constant 1.0 : f32
  ent.yield %x : f32
}

// -----

// expected-error @+1 {{body must end with 'ent.yield' of one 'f32', the function's value}}
ent.function @half(%x: i32) -> f32 {
  ent.yield %x : i32
}

// -----

// expected-error @+1 {{body must end with 'ent.yield' of one 'i32', the function's value}}
ent.function @one() -> i32 {
  ent.yield
}

// -----

// A function never sees the world: what reads it belongs in a system.
ent.resource @Scale (value: i32)
ent.function @scaled(%x: i32) -> i32 {
  // expected-error @+1 {{'ent.read' op must be inside an 'ent.system', a condition or 'ent.main'}}
  %s = ent.read @Scale "value" : i32
  %y = arith.muli %x, %s : i32
  ent.yield %y : i32
}

// -----

ent.function proc @seconds() -> f64
ent.function @now() -> f64 {
  // expected-error @+1 {{'ent.invoke' op calls a proc in a function's body; a function only computes}}
  %t = ent.invoke proc @seconds() : () -> f64
  ent.yield %t : f64
}

// -----

// expected-error @+1 {{gives several values, which C cannot give back}}
ent.function @pair(i32) -> (i32, i32)

// -----

// expected-error @+1 {{body must end with 'ent.yield' of ('i32', 'f32'), the function's values}}
ent.function @pair(%x: i32) -> (i32, f32) {
  ent.yield %x, %x : i32, i32
}

// -----

ent.function @pair(%x: i32) -> (i32, i32) {
  ent.yield %x, %x : i32, i32
}
ent.system @s() {
  %x = arith.constant 1 : i32
  // expected-error @+1 {{result does not match what function @pair gives}}
  %y = ent.invoke @pair(%x) : (i32) -> i32
}

// -----

// expected-error @+1 {{has no cases}}
ent.enum @Nothing []

// -----

// expected-error @+1 {{has duplicate case 'Left'}}
ent.enum @Way ["Left", "Right", "Left"]
