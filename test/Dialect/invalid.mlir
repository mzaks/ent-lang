// RUN: ecs-opt %s -split-input-file -verify-diagnostics

// expected-error @+1 {{has duplicate field 'x'}}
ecs.component @P (x: f32, x: f32)

// -----

// expected-error @+1 {{field 'v' has type 'vector<4xf32>'; only integer, float and index fields are supported}}
ecs.component @P (v: vector<4xf32>)

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{lists component @P more than once; 'writes' already implies read access}}
ecs.system @s() reads [@P] writes [@P] {
}

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{parameter #0 is a component reference; references can only be bound by 'ecs.query'}}
ecs.system @s(%p: !ecs.ref<@P, mut>) writes [@P] {
}

// -----

ecs.component @P (x: f32)
ecs.system @s() writes [@P] {
  // expected-error @+1 {{binds component @P more than once}}
  ecs.query (%a: !ecs.ref<@P, mut>, %b: !ecs.ref<@P>) {
  }
}

// -----

ecs.system @s() {
  // expected-error @+1 {{argument #0 must be an !ecs.ref, got 'f32'}}
  ecs.query (%a: f32) {
  }
}

// -----

ecs.system @s() {
  // expected-error @+1 {{must bind at least one component}}
  ecs.query () {
  }
}

// -----

ecs.component @P (x: f32)
ecs.system @s(%v: f32) reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{requires a mutable reference, got '!ecs.ref<@P>'}}
    ecs.set %p "x", %v : !ecs.ref<@P>, f32
  }
}

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{'ecs.query' op expects parent op 'ecs.system'}}
ecs.query (%p: !ecs.ref<@P>) {
}
