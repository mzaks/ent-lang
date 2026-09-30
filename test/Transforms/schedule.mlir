// RUN: ecs-opt %s --ecs-schedule=explain=1 -verify-diagnostics | FileCheck %s

ecs.component @P (x: f32, y: f32)
ecs.component @V (dx: f32)
ecs.component @Tag ()
ecs.archetype @A (@P, @V) capacity 1000
ecs.archetype @B (@P, @Tag) capacity 1000

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

// Resources: readers of a resource commute; a writer orders against them.
ecs.resource @Clock (dt: f32, frame: i64)
ecs.system @readDt() reads [@Clock] writes [@V] {
  ecs.query (%v: !ecs.ref<@V, mut>) {
    %dt = ecs.read @Clock "dt" : f32
    ecs.set %v "dx", %dt : !ecs.ref<@V, mut>, f32
  }
}
ecs.system @readDtAgain(%c: f32) reads [@Clock] writes [@P] {
  %dt = ecs.read @Clock "dt" : f32
  ecs.query (%p: !ecs.ref<@P, mut>) {
    ecs.set %p "y", %dt : !ecs.ref<@P, mut>, f32
  }
}
ecs.system @advance() writes [@Clock] {
  %f = ecs.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %n = arith.addi %f, %one : i64
  ecs.write @Clock "frame", %n : i64
}
ecs.system @setDt(%dt: f32) writes [@Clock] {
  ecs.write @Clock "dt", %dt : f32
}

// CHECK-LABEL: ecs.schedule @resources
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @readDt
// CHECK-NEXT:   ecs.run @readDtAgain
// CHECK-NEXT:   ecs.run @advance
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @setDt
// CHECK-NEXT: }
ecs.schedule @resources(%c: f32) {
  ecs.run @readDt()
  ecs.run @readDtAgain(%c) : f32
  // Writes Clock.frame, which nobody else touches: same stage.
  ecs.run @advance()
  // expected-remark @+1 {{@setDt waits for @readDt: it writes Clock.dt, which @readDt reads}}
  ecs.run @setDt(%c) : f32
}

// Optional components: gaining or losing one writes the presence, which
// every query binding the component reads.
ecs.component @Q (q: f32)
ecs.component @S (t: f32)
ecs.archetype @C (@Q, optional @S) capacity 100
ecs.system @stun(%t: f32) reads [@Q] writes [@S] {
  ecs.query (%q: !ecs.ref<@Q>) {
    ecs.add @S(%t) : f32
  }
}
ecs.system @countDown(%dt: f32) writes [@S] {
  ecs.query (%s: !ecs.ref<@S, mut>) {
    %t = ecs.get %s "t" : !ecs.ref<@S, mut> -> f32
    %n = arith.subf %t, %dt : f32
    ecs.set %s "t", %n : !ecs.ref<@S, mut>, f32
  }
}
ecs.system @moveQ(%d: f32) writes [@Q] {
  ecs.query (%q: !ecs.ref<@Q, mut>) {
    ecs.set %q "q", %d : !ecs.ref<@Q, mut>, f32
  }
}

// stun waits for countDown because of the presence; moveQ writes C.Q.q,
// which neither reads (binding Q only selects entities), so it joins the
// first stage.
// CHECK-LABEL: ecs.schedule @optional
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @countDown
// CHECK-NEXT:   ecs.run @moveQ
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @stun
// CHECK-NEXT: }
ecs.schedule @optional(%c: f32) {
  ecs.run @countDown(%c) : f32
  // expected-remark @+1 {{@stun waits for @countDown: it writes C.S?, which @countDown reads}}
  ecs.run @stun(%c) : f32
  ecs.run @moveQ(%c) : f32
}

// Structural changes: spawning into A writes A's count and columns, so a
// query over A waits for it; a query over B only does not.
ecs.system @spawnA(%c: f32) writes [@A] {
  ecs.spawn @A(%c, %c, %c) : f32, f32, f32
}

// CHECK-LABEL: ecs.schedule @structural
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @spawnA
// CHECK-NEXT:   ecs.run @xOnB
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @readX
// CHECK-NEXT: }
ecs.schedule @structural(%c: f32) {
  ecs.run @spawnA(%c) : f32
  ecs.run @xOnB(%c) : f32
  // expected-remark @+1 {{@readX waits for @spawnA: it reads A.P.x, which @spawnA writes}}
  ecs.run @readX()
}

// Lookups read the entity table and the looked-up field in every archetype
// holding the component; a despawn writes the table, so it waits.
ecs.component @Ref (entity: !ecs.entity)
ecs.archetype @Seeker (@Ref) capacity 100
ecs.system @seek() reads [@Ref, @P] {
  ecs.query (%r: !ecs.ref<@Ref>) {
    %id = ecs.get %r "entity" : !ecs.ref<@Ref> -> !ecs.entity
    %x, %found = ecs.lookup %id @P "x" : f32
  }
}
ecs.system @cull() reads [@P] writes [@A, @B] {
  ecs.query (%p: !ecs.ref<@P>) {
    ecs.despawn
  }
}

// CHECK-LABEL: ecs.schedule @relations
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @seek
// CHECK-NEXT:   ecs.run @readX
// CHECK-NEXT: }
// CHECK-NEXT: ecs.stage {
// CHECK-NEXT:   ecs.run @cull
// CHECK-NEXT: }
ecs.schedule @relations() {
  ecs.run @seek()
  ecs.run @readX()
  // expected-remark @+1 {{@cull waits for @seek: it writes entities, which @seek reads}}
  ecs.run @cull()
}
