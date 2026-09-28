// RUN: ecs-opt %s --ecs-lower-to-loops | FileCheck %s --check-prefix=SEQ

ecs.component @P (x: f32, y: f32)
ecs.archetype @A (@P)

ecs.system @writeX(%c: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    ecs.set %p "x", %c : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @writeY(%c: f32) writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>) {
    ecs.set %p "y", %c : !ecs.ref<@P, mut>, f32
  }
}

// Sequential: stages dissolve into calls in program order.
// SEQ-LABEL: func.func @frame(
// SEQ-NEXT:    call @writeX
// SEQ-NEXT:    call @writeY
// SEQ-NEXT:    call @writeX
// SEQ-NEXT:    return

ecs.schedule @frame(%c: f32) {
  ecs.stage {
    ecs.run @writeX(%c) : f32
    ecs.run @writeY(%c) : f32
  }
  ecs.stage {
    ecs.run @writeX(%c) : f32
  }
}
