// RUN: ent-opt %s -split-input-file -verify-diagnostics

// expected-error @+1 {{has duplicate field 'x'}}
ent.component @P (x: f32, x: f32)

// -----

// expected-error @+1 {{field 'v' has type 'vector<4xf32>'; only integer, float, index and entity fields are supported}}
ent.component @P (v: vector<4xf32>)

// -----

ent.component @P (x: f32)
// expected-error @+1 {{lists @P more than once; 'writes' already implies read access}}
ent.system @s() reads [@P] writes [@P] {
}

// -----

// expected-error @+1 {{declares access to unknown component, resource or archetype @Nope}}
ent.system @s() reads [@Nope] {
}

// -----

ent.component @P (x: f32)
// expected-error @+1 {{parameter #0 is a component reference; references can only be bound by 'ent.query'}}
ent.system @s(%p: !ent.ref<@P, mut>) writes [@P] {
}

// -----

ent.component @P (x: f32)
ent.system @s() reads [@P] {
  // expected-error @+1 {{binds @P mutably but system @s does not declare it in 'writes'}}
  ent.query (%p: !ent.ref<@P, mut>) {
  }
}

// -----

ent.component @P (x: f32)
ent.component @Q (y: f32)
ent.system @s() reads [@P] {
  // expected-error @+1 {{binds @Q but system @s does not declare it in 'reads' or 'writes'}}
  ent.query (%p: !ent.ref<@P>, %q: !ent.ref<@Q>) {
  }
}

// -----

ent.component @P (x: f32)
ent.system @s() writes [@P] {
  // expected-error @+1 {{binds component @P more than once}}
  ent.query (%a: !ent.ref<@P, mut>, %b: !ent.ref<@P>) {
  }
}

// -----

ent.system @s() {
  // expected-error @+1 {{argument #0 must be an !ent.ref, got 'f32'}}
  ent.query (%a: f32) {
  }
}

// -----

ent.system @s() {
  // expected-error @+1 {{must bind at least one component}}
  ent.query () {
  }
}

// -----

ent.component @P (x: f32)
ent.system @s() reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{component @P has no field 'y'}}
    %y = ent.get %p "y" : !ent.ref<@P> -> f32
  }
}

// -----

ent.component @P (x: f32)
ent.system @s() reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{result type 'i32' does not match field 'x' of type 'f32'}}
    %x = ent.get %p "x" : !ent.ref<@P> -> i32
  }
}

// -----

ent.component @P (x: f32)
ent.system @s(%v: f32) reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{requires a mutable reference, got '!ent.ref<@P>'}}
    ent.set %p "x", %v : !ent.ref<@P>, f32
  }
}

// -----

ent.component @P (x: f32)
ent.system @s(%v: i32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    // expected-error @+1 {{value type 'i32' does not match field 'x' of type 'f32'}}
    ent.set %p "x", %v : !ent.ref<@P, mut>, i32
  }
}

// -----

ent.component @P (x: f32)
// expected-error @+1 {{'ent.query' op expects parent op 'ent.system'}}
ent.query (%p: !ent.ref<@P>) {
}

// -----

ent.system @s(%dt: f32) {
}
ent.schedule @frame(%dt: i32) {
  // expected-error @+1 {{argument types ('i32') do not match the parameters ('f32') of system @s}}
  ent.run @s(%dt) : i32
}

// -----

ent.schedule @frame() {
  // expected-error @+1 {{references unknown system @missing}}
  ent.run @missing()
}

// -----

// expected-error @+1 {{must contain at least one component}}
ent.archetype @Empty () capacity 1000

// -----

ent.component @P (x: f32)
// expected-error @+1 {{lists component @P more than once}}
ent.archetype @Twice (@P, @P) capacity 1000

// -----

// expected-error @+1 {{contains unknown component @Nope}}
ent.archetype @Bad (@Nope) capacity 1000

// -----

ent.component @P (x: f32)
// expected-error @+1 {{attribute 'capacity' failed to satisfy constraint: 64-bit signless integer attribute whose value is positive}}
ent.archetype @Zero (@P) capacity 0

// -----

ent.component @P (x: f32)
ent.system @s() reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{uses component reference #0; references may only be used by 'ent.get' and 'ent.set'}}
    %x = builtin.unrealized_conversion_cast %p : !ent.ref<@P> to f32
  }
}

// -----

ent.schedule @frame() {
  ent.stage {
    // expected-error @+1 {{is not allowed in 'ent.stage'; a stage holds only 'ent.run'}}
    %c = arith.constant 1.0 : f32
  }
}

// -----

ent.system @s() {
  // expected-error @+1 {{'ent.stage' op expects parent op 'ent.schedule'}}
  ent.stage {
  }
}

// -----

ent.resource @Clock (dt: f32)
ent.component @P (x: f32)
ent.system @s(%v: f32) reads [@P] writes [@Clock] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{cannot write a resource inside 'ent.query': every entity would write the same field, which makes the entities depend on each other; write it at system level, or combine values with 'ent.accumulate'}}
    ent.write @Clock "dt", %v : f32
  }
}

// -----

ent.resource @Clock (dt: f32)
ent.system @s(%v: f32) reads [@Clock] {
  // expected-error @+1 {{writes @Clock but system @s does not declare it in 'writes'}}
  ent.write @Clock "dt", %v : f32
}

// -----

ent.resource @Clock (dt: f32)
// A declared contract, even an empty one, is checked.
ent.system @s() reads [] {
  // expected-error @+1 {{reads @Clock but system @s does not declare it in 'reads' or 'writes'}}
  %dt = ent.read @Clock "dt" : f32
}

// -----

ent.resource @Clock (dt: f32)
ent.system @s() reads [@Clock] {
  // expected-error @+1 {{resource @Clock has no field 'frame'}}
  %f = ent.read @Clock "frame" : i64
}

// -----

ent.resource @Clock (dt: f32)
ent.system @s() reads [@Clock] {
  // expected-error @+1 {{result type 'i32' does not match field 'dt' of type 'f32'}}
  %dt = ent.read @Clock "dt" : i32
}

// -----

ent.component @P (x: f32)
ent.system @s() reads [@P] {
  // expected-error @+1 {{references unknown resource @P}}
  %x = ent.read @P "x" : f32
}

// -----

ent.resource @Clock (dt: f32)
func.func @f() -> f32 {
  // expected-error @+1 {{must be inside an 'ent.system'}}
  %dt = ent.read @Clock "dt" : f32
  return %dt : f32
}

// -----

ent.resource @Clock (dt: f32)
// A resource is not an entity: no query can bind it, and no archetype can
// hold it.
// expected-error @+1 {{contains unknown component @Clock}}
ent.archetype @A (@Clock) capacity 10

// -----

// expected-error @+1 {{has a field without a name}}
ent.component @P ("": f32)

// -----

ent.component @P (x: f32)
ent.component @S (t: f32)
// expected-error @+1 {{marks @S optional but does not list it as a component}}
"ent.archetype"() <{sym_name = "A", components = [@P], optional = [@S], capacity = 10 : i64}> : () -> ()

// -----

ent.component @P (x: f32)
ent.component @S (t: f32)
ent.archetype @A (@P, optional @S) capacity 10
ent.system @s(%v: f32) writes [@S] {
  // expected-error @+1 {{must be inside an 'ent.query': it changes the entity the query visits}}
  ent.add @S(%v) : f32
}

// -----

ent.component @P (x: f32)
ent.component @S (t: f32)
ent.archetype @A (@P, optional @S) capacity 10
ent.system @s(%v: f32) reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{changes @S but system @s does not declare it in 'writes'}}
    ent.add @S(%v) : f32
  }
}

// -----

ent.component @P (x: f32)
ent.component @Q (q: f32)
ent.component @S (t: f32)
ent.archetype @A (@P, optional @S) capacity 10
ent.archetype @B (@P, @Q) capacity 10
ent.system @s(%v: f32) reads [@P] writes [@S] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{adds @S to entities of @B, but no archetype has exactly the resulting components; declare one, or make @S optional in @B}}
    ent.add @S(%v) : f32
  }
}

// -----

ent.system @s() {
  // expected-error @+1 {{must be inside an 'ent.query'}}
  %id = ent.entity
}

// -----

ent.component @P (x: f32)
ent.component @S (t: f32)
ent.archetype @A (@P, @S) capacity 10
ent.system @s() writes [@S] {
  ent.query (%s: !ent.ref<@S>) {
    // expected-error @+1 {{removes @S from entities of @A, but no archetype has exactly the resulting components; declare one, or make @S optional in @A}}
    ent.remove @S
  }
}

// -----

ent.component @P (x: f32)
ent.component @S (t: f32, n: i32)
ent.archetype @A (@P, optional @S) capacity 10
ent.system @s(%v: f32) reads [@P] writes [@S] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{initialises 1 fields, but @S has 2}}
    ent.add @S(%v) : f32
  }
}

// -----

ent.component @P (x: f32)
ent.component @S (t: f32)
ent.archetype @A (@P, optional @S) capacity 10
ent.system @s(%v: i32) reads [@P] writes [@S] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{value #0 has type 'i32', but field 't' has type 'f32'}}
    ent.add @S(%v) : i32
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
// expected-error @+1 {{lists archetype @A in 'reads'; archetypes are declared in 'writes', by systems that spawn or despawn their entities}}
ent.system @s() reads [@A] {
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.system @s(%v: f32) reads [] {
  // expected-error @+1 {{spawns into @A but system @s does not declare it in 'writes'}}
  ent.spawn @A(%v) : f32
}

// -----

ent.component @P (x: f32, y: i32)
ent.component @S (t: f32)
ent.archetype @A (@P, optional @S) capacity 10
ent.system @s(%v: f32) writes [@A] {
  // Optional components start absent, so only P's fields are given.
  // expected-error @+1 {{value #1 has type 'f32', but field P.y has type 'i32'}}
  ent.spawn @A(%v, %v) : f32, f32
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.system @s() writes [@A] {
  // expected-error @+1 {{initialises 0 fields, but the non-optional components of @A have 1}}
  ent.spawn @A()
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.archetype @B (@P) capacity 10
ent.system @s() reads [@P] writes [@A] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{despawns entities of @B but system @s does not declare it in 'writes'}}
    ent.despawn
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.system @s() writes [@A] {
  // expected-error @+1 {{must be inside an 'ent.query'}}
  ent.despawn
}

// -----

ent.component @P (x: f32)
ent.system @s(%id: !ent.entity) reads [] {
  // expected-error @+1 {{looks up @P but system @s does not declare it in 'reads' or 'writes'}}
  %x, %found = ent.lookup %id @P "x" : f32
}

// -----

ent.component @P (x: f32)
ent.system @s(%id: !ent.entity) reads [@P] {
  // expected-error @+1 {{result type 'i32' does not match field 'x' of type 'f32'}}
  %x, %found = ent.lookup %id @P "x" : i32
}

// -----

ent.component @P (x: f32)
ent.component @T (entity: !ent.entity)
ent.archetype @A (@P, @T) capacity 10
ent.system @s() reads [@T] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>, %t: !ent.ref<@T>) {
    %target = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    // expected-error @+1 {{looks up @P "x" of other entities in a query that changes it; which entities see the old value would depend on iteration order}}
    %x, %found = ent.lookup %target @P "x" : f32
    // expected-note @+1 {{changed here}}
    ent.set %p "x", %x : !ent.ref<@P, mut>, f32
  }
}

// -----

ent.component @P (x: f32)
func.func @f(%id: !ent.entity) {
  // expected-error @+1 {{must be inside an 'ent.system'}}
  %x, %found = ent.lookup %id @P "x" : f32
  return
}

// -----

ent.component @H (hp: f32)
ent.system @s(%id: !ent.entity, %d: f32) writes [@H] {
  // expected-error @+1 {{must be inside an 'ent.query': its values are combined when the query ends}}
  ent.apply %id @H "hp" add %d : f32
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: f32) reads [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{applies to @H but system @s does not declare it in 'writes'}}
    ent.apply %id @H "hp" add %d : f32
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{has unknown rule 'mul'; expected 'add', 'min' or 'max'}}
    ent.apply %id @H "hp" mul %d : f32
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: i32) writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{value type 'i32' does not match field 'hp' of type 'f32'}}
    ent.apply %id @H "hp" add %d : i32
  }
}

// -----

ent.component @H (alive: i1, next: !ent.entity)
ent.archetype @A (@H) capacity 10
ent.system @s(%b: i1) writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{cannot combine field 'alive' of type i1; only integers and floats can}}
    ent.apply %id @H "alive" max %b : i1
  }
}

// -----

ent.component @H (next: !ent.entity)
ent.archetype @A (@H) capacity 10
ent.system @s() writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{cannot combine field 'next' of type '!ent.entity'; only integers and floats can}}
    ent.apply %id @H "next" max %id : !ent.entity
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c3 = arith.constant 3 : index
    scf.for %i = %c0 to %c3 step %c1 {
      // expected-error @+1 {{must not be inside a loop ('scf.for'): it may run at most once per entity}}
      ent.apply %id @H "hp" add %d : f32
    }
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    // expected-error @+1 {{combines @H "hp" with 'add', but the query also combines it with 'min'; the result would depend on the order}}
    ent.apply %id @H "hp" add %d : f32
    // expected-note @+1 {{other rule here}}
    ent.apply %id @H "hp" min %d : f32
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s() reads [@H] {
  // expected-error @+1 {{unknown trigger 'touched'; expected 'added', 'removed' or 'changed'}}
  ent.query (%h: !ent.ref<@H>) on [touched @H] {
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s() reads [@H] {
  // expected-error @+1 {{component @H has no field 'shield'}}
  ent.query (%h: !ent.ref<@H>) on [changed @H "shield"] {
  }
}

// -----

ent.component @H (hp: f32)
ent.component @T ()
ent.archetype @A (@H, @T) capacity 10
ent.system @s() reads [@T] {
  // expected-error @+1 {{reacts to @H but system @s does not declare it in 'reads' or 'writes'}}
  ent.query (%t: !ent.ref<@T>) on [changed @H] {
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (optional @H) capacity 10
ent.system @s() reads [@H] {
  // expected-error @+1 {{reacts to removed @H but binds it; an entity that lost it never matches}}
  ent.query (%h: !ent.ref<@H>) on [removed @H] {
  }
}

// -----

ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s() reads [@H] {
  // expected-error @+1 {{gives the event log of @H a negative capacity}}
  ent.query (%h: !ent.ref<@H>) on [changed @H log -1] {
  }
}

// -----

ent.resource @Score (points: i64, alive: i1)
ent.system @s(%v: i64) writes [@Score] {
  // expected-error @+1 {{must be inside an 'ent.query': its values are combined when the query ends}}
  ent.accumulate @Score "points" add %v : i64
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64)
ent.system @s(%v: i64) reads [@P, @Score] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{writes @Score but system @s does not declare it in 'writes'}}
    ent.accumulate @Score "points" add %v : i64
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64, alive: i1)
ent.system @s(%b: i1) reads [@P] writes [@Score] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{cannot combine field 'alive' of type 'i1'; only integers (not i1) and floats can}}
    ent.accumulate @Score "alive" max %b : i1
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64)
ent.system @s(%v: i32) reads [@P] writes [@Score] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{value type 'i32' does not match field 'points' of type 'i64'}}
    ent.accumulate @Score "points" add %v : i32
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64)
ent.system @s(%v: i64) reads [@P] writes [@Score] {
  ent.query (%p: !ent.ref<@P>) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    scf.for %i = %c0 to %c1 step %c1 {
      // expected-error @+1 {{must not be inside a loop ('scf.for'): it may run at most once per entity}}
      ent.accumulate @Score "points" add %v : i64
    }
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64)
ent.system @s(%v: i64) reads [@P] writes [@Score] {
  ent.query (%p: !ent.ref<@P>) {
    // expected-error @+1 {{combines @Score "points" with 'add', but the query also combines it with 'max'; the result would depend on the order}}
    ent.accumulate @Score "points" add %v : i64
    // expected-note @+1 {{other rule here}}
    ent.accumulate @Score "points" max %v : i64
  }
}

// -----

ent.component @P (x: f32) capacity 8
ent.system @s(%x: f32) {
  // expected-error @+1 {{lists component @P more than once}}
  ent.spawn (@P, @P)(%x, %x) : f32, f32
}

// -----

ent.component @P (x: f32) capacity 8
ent.system @s(%x: f32) {
  // expected-error @+1 {{spawns unknown component @Q}}
  ent.spawn (@P, @Q)(%x) : f32
}

// -----

ent.component @P (x: f32) capacity 8
ent.component @V (dx: f32) capacity 8
ent.system @s(%x: f32) {
  // expected-error @+1 {{initialises 1 fields, but its components have 2}}
  ent.spawn (@P, @V)(%x) : f32
}

// -----

ent.component @P (x: f32) capacity 8
ent.system @s(%x: f32) reads [@P] {
  // expected-error @+1 {{spawns @P but system @s does not declare it in 'writes'}}
  ent.spawn (@P)(%x) : f32
}

// -----

ent.component @P (x: f32) capacity 8
ent.component @V (dx: f32) capacity 8
ent.archetype @PV (@P, @V) capacity 8
ent.system @s(%x: f32) {
  // expected-error @+1 {{spawns into @PV without its required component @V}}
  ent.spawn (@P) into @PV (%x) : f32
}

// -----

// expected-error @+1 {{'ent.component' op attribute 'capacity' failed to satisfy constraint}}
ent.component @P (x: f32) capacity 0
