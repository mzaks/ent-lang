// RUN: ecs-opt %s --ecs-schedule=explain=1 -verify-diagnostics | FileCheck %s

ecs.component @P (x: f32, y: f32)
ecs.component @V (dx: f32)
ecs.component @Tag ()
ecs.archetype @A (@P, @V)
ecs.archetype @B (@P, @Tag)

func.func private @log()

// Touch P.x or P.y in both archetypes.
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
ecs.system @readX() reads [@P] {
  ecs.query (%p: !ecs.ref<@P>) {
    %x = ecs.get %p "x" : !ecs.ref<@P> -> f32
  }
}
// Touch P.x in one archetype each; V and Tag are bound only to select it.
ecs.system @xOnA(%c: f32) reads [@V] writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>, %v: !ecs.ref<@V>) {
    ecs.set %p "x", %c : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @xOnB(%c: f32) reads [@Tag] writes [@P] {
  ecs.query (%p: !ecs.ref<@P, mut>, %t: !ecs.ref<@Tag>) {
    ecs.set %p "x", %c : !ecs.ref<@P, mut>, f32
  }
}
// A call has unknown effects, so this system conflicts with everything.
ecs.system @logs() {
  func.call @log() : () -> ()
}

// Different fields of the same component do not conflict.
// CHECK-LABEL: ecs.schedule @fields
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT:   ecs.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: }
ecs.schedule @fields(%c: f32) {
  ecs.run @writeX(%c) : f32
  ecs.run @writeY(%c) : f32
}

// Both declare `writes [@P]`, but they write P.x in different archetypes.
// CHECK-LABEL: ecs.schedule @archetypes
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @xOnA
// CHECK-NEXT:   ecs.run @xOnB
// CHECK-NEXT: }
// CHECK-NEXT: }
ecs.schedule @archetypes(%c: f32) {
  ecs.run @xOnA(%c) : f32
  ecs.run @xOnB(%c) : f32
}

// CHECK-LABEL: ecs.schedule @readAfterWrite
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @readX
// CHECK-NEXT: }
ecs.schedule @readAfterWrite(%c: f32) {
  ecs.run @writeX(%c) : f32
  // expected-remark @+1 {{@readX waits for @writeX: it reads A.P.x, which @writeX writes}}
  ecs.run @readX()
}

// CHECK-LABEL: ecs.schedule @writeAfterRead
// CHECK:      ecs.run @readX
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
ecs.schedule @writeAfterRead(%c: f32) {
  ecs.run @readX()
  // expected-remark @+1 {{@writeX waits for @readX: it writes A.P.x, which @readX reads}}
  ecs.run @writeX(%c) : f32
}

// CHECK-LABEL: ecs.schedule @writeAfterWrite
// CHECK:      ecs.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @xOnA
ecs.schedule @writeAfterWrite(%c: f32) {
  ecs.run @writeX(%c) : f32
  // expected-remark @+1 {{@xOnA waits for @writeX: it writes A.P.x, which @writeX also writes}}
  ecs.run @xOnA(%c) : f32
}

// A later run moves ahead of a conflicting pair when it commutes with both.
// CHECK-LABEL: ecs.schedule @reorder
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT:   ecs.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @readX
// CHECK-NEXT: }
ecs.schedule @reorder(%c: f32) {
  ecs.run @writeX(%c) : f32
  // expected-remark @+1 {{@readX waits for @writeX}}
  ecs.run @readX()
  ecs.run @writeY(%c) : f32
}

// CHECK-LABEL: ecs.schedule @opaque
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @logs
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT: }
ecs.schedule @opaque(%c: f32) {
  ecs.run @writeY(%c) : f32
  // expected-remark @+1 {{@logs waits for @writeY: it has effects outside component access ('func.call')}}
  ecs.run @logs()
  // expected-remark @+1 {{@writeX waits for @logs: @logs has effects outside component access ('func.call')}}
  ecs.run @writeX(%c) : f32
}

// An op with effects in the schedule is a barrier; pure ops stay in place.
// CHECK-LABEL: ecs.schedule @barrier
// CHECK-NEXT: arith.constant
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: call @log()
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeY
// CHECK-NEXT: }
ecs.schedule @barrier() {
  %c = arith.constant 1.0 : f32
  ecs.run @writeX(%c) : f32
  func.call @log() : () -> ()
  ecs.run @writeY(%c) : f32
}

// Schedules that already have stages are left alone.
// CHECK-LABEL: ecs.schedule @staged
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @writeY
// CHECK-NEXT: }
ecs.schedule @staged(%c: f32) {
  ecs.stage {
    ecs.run @writeX(%c) : f32
  }
  ecs.stage {
    ecs.run @writeY(%c) : f32
  }
}
