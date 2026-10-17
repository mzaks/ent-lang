// RUN: ent-opt %s --ent-lower-to-loops -verify-diagnostics -o /dev/null

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
