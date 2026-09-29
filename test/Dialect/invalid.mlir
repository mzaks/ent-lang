// RUN: ecs-opt %s -split-input-file -verify-diagnostics

// expected-error @+1 {{has duplicate field 'x'}}
ecs.component @P (x: f32, x: f32)

// -----

// expected-error @+1 {{field 'v' has type 'vector<4xf32>'; only integer, float and index fields are supported}}
ecs.component @P (v: vector<4xf32>)

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{lists @P more than once; 'writes' already implies read access}}
ecs.system @s() reads [@P] writes [@P] {
}

// -----

// expected-error @+1 {{declares access to unknown component, resource or archetype @Nope}}
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
ecs.archetype @Empty () capacity 1000

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{lists component @P more than once}}
ecs.archetype @Twice (@P, @P) capacity 1000

// -----

// expected-error @+1 {{contains unknown component @Nope}}
ecs.archetype @Bad (@Nope) capacity 1000

// -----

ecs.component @P (x: f32)
// expected-error @+1 {{attribute 'capacity' failed to satisfy constraint: 64-bit signless integer attribute whose value is positive}}
ecs.archetype @Zero (@P) capacity 0

// -----

ecs.component @P (x: f32)
ecs.system @s() reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{uses component reference #0; references may only be used by 'ecs.get' and 'ecs.set'}}
    %x = builtin.unrealized_conversion_cast %p : !ecs.ref<@P> to f32
  }
}

// -----

ecs.schedule @frame() {
  ecs.stage {
    // expected-error @+1 {{is not allowed in 'ecs.stage'; a stage holds only 'ecs.run'}}
    %c = arith.constant 1.0 : f32
  }
}

// -----

ecs.system @s() {
  // expected-error @+1 {{'ecs.stage' op expects parent op 'ecs.schedule'}}
  ecs.stage {
  }
}

// -----

ecs.resource @Clock (dt: f32)
ecs.component @P (x: f32)
ecs.system @s(%v: f32) reads [@P] writes [@Clock] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{cannot write a resource inside 'ecs.query': every entity would write the same field, which makes the entities depend on each other; write it at system level}}
    ecs.write @Clock "dt", %v : f32
  }
}

// -----

ecs.resource @Clock (dt: f32)
ecs.system @s(%v: f32) reads [@Clock] {
  // expected-error @+1 {{writes @Clock but system @s does not declare it in 'writes'}}
  ecs.write @Clock "dt", %v : f32
}

// -----

ecs.resource @Clock (dt: f32)
ecs.system @s() {
  // expected-error @+1 {{reads @Clock but system @s does not declare it in 'reads' or 'writes'}}
  %dt = ecs.read @Clock "dt" : f32
}

// -----

ecs.resource @Clock (dt: f32)
ecs.system @s() reads [@Clock] {
  // expected-error @+1 {{resource @Clock has no field 'frame'}}
  %f = ecs.read @Clock "frame" : i64
}

// -----

ecs.resource @Clock (dt: f32)
ecs.system @s() reads [@Clock] {
  // expected-error @+1 {{result type 'i32' does not match field 'dt' of type 'f32'}}
  %dt = ecs.read @Clock "dt" : i32
}

// -----

ecs.component @P (x: f32)
ecs.system @s() reads [@P] {
  // expected-error @+1 {{references unknown resource @P}}
  %x = ecs.read @P "x" : f32
}

// -----

ecs.resource @Clock (dt: f32)
func.func @f() -> f32 {
  // expected-error @+1 {{must be inside an 'ecs.system'}}
  %dt = ecs.read @Clock "dt" : f32
  return %dt : f32
}

// -----

ecs.resource @Clock (dt: f32)
// A resource is not an entity: no query can bind it, and no archetype can
// hold it.
// expected-error @+1 {{contains unknown component @Clock}}
ecs.archetype @A (@Clock) capacity 10

// -----

// expected-error @+1 {{has a field without a name}}
ecs.component @P ("": f32)

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32)
// expected-error @+1 {{marks @S optional but does not list it as a component}}
"ecs.archetype"() <{sym_name = "A", components = [@P], optional = [@S], capacity = 10 : i64}> : () -> ()

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.system @s(%v: f32) writes [@S] {
  // expected-error @+1 {{must be inside an 'ecs.query': it changes the entity the query visits}}
  ecs.add @S(%v) : f32
}

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.system @s(%v: f32) reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{changes @S but system @s does not declare it in 'writes'}}
    ecs.add @S(%v) : f32
  }
}

// -----

ecs.component @P (x: f32)
ecs.component @Q (q: f32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.archetype @B (@P, @Q) capacity 10
ecs.system @s(%v: f32) reads [@P] writes [@S] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{adds @S to entities of @B, but no archetype has exactly the resulting components; declare one, or make @S optional in @B}}
    ecs.add @S(%v) : f32
  }
}

// -----

ecs.system @s() {
  // expected-error @+1 {{must be inside an 'ecs.query'}}
  %id = ecs.entity : i64
}

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, @S) capacity 10
ecs.system @s() writes [@S] {
  ecs.query (%s: !ecs.ref<@S>) {
    // expected-error @+1 {{removes @S from entities of @A, but no archetype has exactly the resulting components; declare one, or make @S optional in @A}}
    ecs.remove @S
  }
}

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32, n: i32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.system @s(%v: f32) reads [@P] writes [@S] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{initialises 1 fields, but @S has 2}}
    ecs.add @S(%v) : f32
  }
}

// -----

ecs.component @P (x: f32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.system @s(%v: i32) reads [@P] writes [@S] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{value #0 has type 'i32', but field 't' has type 'f32'}}
    ecs.add @S(%v) : i32
  }
}

// -----

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 10
// expected-error @+1 {{lists archetype @A in 'reads'; archetypes are declared in 'writes', by systems that spawn or despawn their entities}}
ecs.system @s() reads [@A] {
}

// -----

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 10
ecs.system @s(%v: f32) {
  // expected-error @+1 {{spawns into @A but system @s does not declare it in 'writes'}}
  ecs.spawn @A(%v) : f32
}

// -----

ecs.component @P (x: f32, y: i32)
ecs.component @S (t: f32)
ecs.archetype @A (@P, optional @S) capacity 10
ecs.system @s(%v: f32) writes [@A] {
  // Optional components start absent, so only P's fields are given.
  // expected-error @+1 {{value #1 has type 'f32', but field P.y has type 'i32'}}
  ecs.spawn @A(%v, %v) : f32, f32
}

// -----

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 10
ecs.system @s() writes [@A] {
  // expected-error @+1 {{initialises 0 fields, but the non-optional components of @A have 1}}
  ecs.spawn @A()
}

// -----

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 10
ecs.archetype @B (@P) capacity 10
ecs.system @s() reads [@P] writes [@A] {
  ecs.query (%p: !ecs.ref<@P>) {
    // expected-error @+1 {{despawns entities of @B but system @s does not declare it in 'writes'}}
    ecs.despawn
  }
}

// -----

ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 10
ecs.system @s() writes [@A] {
  // expected-error @+1 {{must be inside an 'ecs.query'}}
  ecs.despawn
}
