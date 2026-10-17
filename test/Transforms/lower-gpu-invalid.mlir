// RUN: ent-opt %s -split-input-file --ent-lower-to-loops -verify-diagnostics -o /dev/null

// Adding a component that the visited archetype does not hold moves the
// entity, which is done on the CPU: only known once the archetypes are
// there, so the lowering says so, with or without `gpu`.

ent.component @P (x: f32)
ent.component @Hit ()
ent.archetype @A (@P) capacity 16
ent.archetype @AHit (@P, @Hit) capacity 16

ent.system @mark() gpu {
  // expected-error @+1 {{cannot run on the GPU, as its system is 'gpu': it moves entities of @A to another archetype}}
  ent.query (%p: !ent.ref<@P>) {
    ent.add @Hit()
  }
}
ent.schedule @frame() {
  ent.run @mark()
}

// -----

// A field a reactive query reacts to has its events kept on the CPU.

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 16

ent.system @move() gpu {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    // expected-error @+1 {{cannot run on the GPU, as its system is 'gpu': a reactive query reacts to @P}}
    ent.set %p "x", %x : !ent.ref<@P, mut>, f32
  }
}
ent.system @watch() {
  ent.query (%p: !ent.ref<@P>) on [changed @P "x"] {
  }
}
ent.schedule @frame() {
  ent.run @move()
  ent.run @watch()
}

// -----

// What is sent lands when the query ends: on the GPU, as it is sent, so
// only where nothing else in the query touches the field.

ent.component @Hp (hp: i32, foe: !ent.entity)
ent.archetype @A (@Hp) capacity 16

ent.system @hurt() gpu {
  ent.query (%h: !ent.ref<@Hp>) {
    %foe = ent.get %h "foe" : !ent.ref<@Hp> -> !ent.entity
    %hp = ent.get %h "hp" : !ent.ref<@Hp> -> i32
    // expected-error @+1 {{it sends to a field its query also reads or writes}}
    ent.apply %foe @Hp "hp" add %hp : i32
  }
}
ent.schedule @frame() {
  ent.run @hurt()
}

// -----

// A send to a field a reactive query reacts to: its events are the CPU's.

ent.component @Hp (hp: i32, foe: !ent.entity)
ent.archetype @A (@Hp) capacity 16

ent.system @hurt() gpu {
  ent.query (%h: !ent.ref<@Hp>) {
    %foe = ent.get %h "foe" : !ent.ref<@Hp> -> !ent.entity
    %one = arith.constant 1 : i32
    // expected-error @+1 {{a reactive query reacts to @Hp}}
    ent.apply %foe @Hp "hp" add %one : i32
  }
}
ent.system @watch() {
  ent.query (%h: !ent.ref<@Hp>) on [changed @Hp "hp"] {
  }
}
ent.schedule @frame() {
  ent.run @hurt()
  ent.run @watch()
}
