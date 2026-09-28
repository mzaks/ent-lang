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

// expected-error @+1 {{declares access to unknown component @Nope}}
ecs.system @s() reads [@Nope] {
}

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{parameter #0 is a component reference; references can only be bound by 'ecs.query'}}
ecs.system @s(%p: !ecs.ref<@P, mut>) writes [@P] {
}

// -----

ecs.component @P (x: f32)
ecs.system @s() reads [@P] {
  // expected-error @+1 {{binds @P mutably but system @s does not declare it in 'writes'}}
  ecs.query (%p: !ecs.ref<@P, mut>) {
  }
}

// -----

ecs.component @P (x: f32)
ecs.component @Q (y: f32)
ecs.system @s() reads [@P] {
  // expected-error @+1 {{binds @Q but system @s does not declare it in 'reads' or 'writes'}}
  ecs.query (%p: !ecs.ref<@P>, %q: !ecs.ref<@Q>) {
  }
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
ecs.system @s() reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{component @P has no field 'y'}}
    %y = ecs.get %p "y" : !ecs.ref<@P> -> f32
  }
}

// -----

ecs.component @P (x: f32)
ecs.system @s() reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{result type 'i32' does not match field 'x' of type 'f32'}}
    %x = ecs.get %p "x" : !ecs.ref<@P> -> i32
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
ecs.system @s(%v: i32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    // expected-error @+1 {{value type 'i32' does not match field 'x' of type 'f32'}}
    ecs.set %p "x", %v : !ecs.ref<@P, mut>, i32
  }
}

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{'ecs.query' op expects parent op 'ecs.system'}}
ecs.query (%p: !ecs.ref<@P>) {
}

// -----

ecs.system @s(%dt: f32) {
}
ecs.schedule @frame(%dt: i32) {
  // expected-error @+1 {{argument types ('i32') do not match the parameters ('f32') of system @s}}
  ecs.run @s(%dt) : i32
}

// -----

ecs.schedule @frame() {
  // expected-error @+1 {{references unknown system @missing}}
  ecs.run @missing()
}

// -----

// expected-error @+1 {{must contain at least one component}}
ecs.archetype @Empty ()

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{lists component @P more than once}}
ecs.archetype @Twice (@P, @P)

// -----

// expected-error @+1 {{contains unknown component @Nope}}
ecs.archetype @Bad (@Nope)

// -----

ecs.component @P (x: f32)
ecs.system @s() reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{uses component reference #0; references may only be used by 'ecs.get' and 'ecs.set'}}
    %x = builtin.unrealized_conversion_cast %p : !ecs.ref<@P> to f32
  }
}
