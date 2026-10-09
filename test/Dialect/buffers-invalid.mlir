// RUN: ent-opt %s -split-input-file -verify-diagnostics

// expected-error @+1 {{field 'name' has type '!ent.text<8>'; a buffer's fields are numbers, bools and enums}}
ent.buffer @B (name: !ent.text<8>) capacity 4

// -----

// expected-error @+1 {{has no fields}}
ent.buffer @B () capacity 4

// -----

ent.buffer @B (x: f32) capacity 4
ent.system @s(%x: f32) {
  // expected-error @+1 {{gives 2 values, but @B has 1 fields}}
  ent.append @B (%x, %x) : f32, f32
}

// -----

ent.buffer @B (x: f32) capacity 4
ent.system @s(%n: i32) {
  // expected-error @+1 {{value #0 has type 'i32', but field 'x' has type 'f32'}}
  ent.append @B (%n) : i32
}

// -----

ent.component @P (x: f32)
ent.system @s(%x: f32) {
  // expected-error @+1 {{refers to unknown buffer @P}}
  ent.append @P (%x) : f32
}

// -----

// At most once for each entity.
ent.buffer @B (x: f32) capacity 4
ent.component @P (x: f32)
ent.system @s(%x: f32) {
  ent.query (%p: !ent.ref<@P>) {
    %c0 = arith.constant 0 : index
    %c4 = arith.constant 4 : index
    %c1 = arith.constant 1 : index
    scf.for %i = %c0 to %c4 step %c1 {
      // expected-error @+1 {{must not be inside a loop ('scf.for') in a query: it may run at most once per entity}}
      ent.append @B (%x) : f32
    }
  }
}

// -----

ent.buffer @B (x: f32) capacity 4
ent.component @P (x: f32)
ent.system @s() {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{must not be inside a query}}
    ent.clear @B
  }
}

// -----

ent.buffer @B (x: f32) capacity 4
ent.system @s(%x: f32) reads [@B] {
  // expected-error @+1 {{appends to @B but system @s does not declare it in 'writes'}}
  ent.append @B (%x) : f32
}

// -----

ent.buffer @B (x: f32) capacity 4
ent.system @s(%n: i32) {
  // expected-error @+1 {{reads 'y', which @B has not}}
  %y = ent.buffer.at @B "y"[%n : i32] : f32
}

// -----

// Its rows are read while the function runs: only a proc is handed one.
ent.buffer @B (x: f32) capacity 4
// expected-error @+1 {{parameter #0 is a buffer, which only a proc is handed}}
ent.function @sum(!ent.buffer<@B>) -> f32

// -----

// A buffer is handed over, and not kept.
ent.buffer @B (x: f32) capacity 4
ent.function proc @draw(!ent.buffer<@B>)
ent.system @s(%c: i1) {
  // expected-error @+1 {{is for an 'ent.invoke' next to it: a buffer is handed to a function, and not kept}}
  %b = ent.buffer.of @B : !ent.buffer<@B>
  scf.if %c {
    ent.invoke proc @draw(%b) : (!ent.buffer<@B>) -> ()
  }
}
