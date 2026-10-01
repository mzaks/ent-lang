// RUN: ent-opt %s --ent-schedule=explain=1 -verify-diagnostics | FileCheck %s

ent.component @P (x: f32, y: f32)
ent.component @V (dx: f32)
ent.component @Tag ()
ent.archetype @A (@P, @V) capacity 1000
ent.archetype @B (@P, @Tag) capacity 1000

func.func private @log()

// Touch P.x or P.y in both archetypes.
ent.system @writeX(%c: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    ent.set %p "x", %c : !ent.ref<@P, mut>, f32
  }
}
ent.system @writeY(%c: f32) writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>) {
    ent.set %p "y", %c : !ent.ref<@P, mut>, f32
  }
}
ent.system @readX() reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    %x = ent.get %p "x" : !ent.ref<@P> -> f32
  }
}
// Touch P.x in one archetype each; V and Tag are bound only to select it.
ent.system @xOnA(%c: f32) reads [@V] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>, %v: !ent.ref<@V>) {
    ent.set %p "x", %c : !ent.ref<@P, mut>, f32
  }
}
ent.system @xOnB(%c: f32) reads [@Tag] writes [@P] {
  ent.query (%p: !ent.ref<@P, mut>, %t: !ent.ref<@Tag>) {
    ent.set %p "x", %c : !ent.ref<@P, mut>, f32
  }
}
// A call has unknown effects, so this system conflicts with everything.
ent.system @logs() {
  func.call @log() : () -> ()
}

// Different fields of the same component do not conflict.
// CHECK-LABEL: ent.schedule @fields
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT:   ent.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: }
ent.schedule @fields(%c: f32) {
  ent.run @writeX(%c) : f32
  ent.run @writeY(%c) : f32
}

// Both declare `writes [@P]`, but they write P.x in different archetypes.
// CHECK-LABEL: ent.schedule @archetypes
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @xOnA
// CHECK-NEXT:   ent.run @xOnB
// CHECK-NEXT: }
// CHECK-NEXT: }
ent.schedule @archetypes(%c: f32) {
  ent.run @xOnA(%c) : f32
  ent.run @xOnB(%c) : f32
}

// CHECK-LABEL: ent.schedule @readAfterWrite
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @readX
// CHECK-NEXT: }
ent.schedule @readAfterWrite(%c: f32) {
  ent.run @writeX(%c) : f32
  // expected-remark @+1 {{@readX waits for @writeX: it reads A.P.x, which @writeX writes}}
  ent.run @readX()
}

// CHECK-LABEL: ent.schedule @writeAfterRead
// CHECK:      ent.run @readX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
ent.schedule @writeAfterRead(%c: f32) {
  ent.run @readX()
  // expected-remark @+1 {{@writeX waits for @readX: it writes A.P.x, which @readX reads}}
  ent.run @writeX(%c) : f32
}

// CHECK-LABEL: ent.schedule @writeAfterWrite
// CHECK:      ent.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @xOnA
ent.schedule @writeAfterWrite(%c: f32) {
  ent.run @writeX(%c) : f32
  // expected-remark @+1 {{@xOnA waits for @writeX: it writes A.P.x, which @writeX also writes}}
  ent.run @xOnA(%c) : f32
}

// A later run moves ahead of a conflicting pair when it commutes with both.
// CHECK-LABEL: ent.schedule @reorder
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT:   ent.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @readX
// CHECK-NEXT: }
ent.schedule @reorder(%c: f32) {
  ent.run @writeX(%c) : f32
  // expected-remark @+1 {{@readX waits for @writeX}}
  ent.run @readX()
  ent.run @writeY(%c) : f32
}

// CHECK-LABEL: ent.schedule @opaque
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeY
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @logs
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT: }
ent.schedule @opaque(%c: f32) {
  ent.run @writeY(%c) : f32
  // expected-remark @+1 {{@logs waits for @writeY: it has effects outside component access ('func.call')}}
  ent.run @logs()
  // expected-remark @+1 {{@writeX waits for @logs: @logs has effects outside component access ('func.call')}}
  ent.run @writeX(%c) : f32
}

// An op with effects in the schedule is a barrier; pure ops stay in place.
// CHECK-LABEL: ent.schedule @barrier
// CHECK-NEXT: arith.constant
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: call @log()
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeY
// CHECK-NEXT: }
ent.schedule @barrier() {
  %c = arith.constant 1.0 : f32
  ent.run @writeX(%c) : f32
  func.call @log() : () -> ()
  ent.run @writeY(%c) : f32
}

// Schedules that already have stages are left alone.
// CHECK-LABEL: ent.schedule @staged
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @writeY
// CHECK-NEXT: }
ent.schedule @staged(%c: f32) {
  ent.stage {
    ent.run @writeX(%c) : f32
  }
  ent.stage {
    ent.run @writeY(%c) : f32
  }
}

// Resources: readers of a resource commute; a writer orders against them.
ent.resource @Clock (dt: f32, frame: i64)
ent.system @readDt() reads [@Clock] writes [@V] {
  ent.query (%v: !ent.ref<@V, mut>) {
    %dt = ent.read @Clock "dt" : f32
    ent.set %v "dx", %dt : !ent.ref<@V, mut>, f32
  }
}
ent.system @readDtAgain(%c: f32) reads [@Clock] writes [@P] {
  %dt = ent.read @Clock "dt" : f32
  ent.query (%p: !ent.ref<@P, mut>) {
    ent.set %p "y", %dt : !ent.ref<@P, mut>, f32
  }
}
ent.system @advance() writes [@Clock] {
  %f = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %n = arith.addi %f, %one : i64
  ent.write @Clock "frame", %n : i64
}
ent.system @setDt(%dt: f32) writes [@Clock] {
  ent.write @Clock "dt", %dt : f32
}

// CHECK-LABEL: ent.schedule @resources
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @readDt
// CHECK-NEXT:   ent.run @readDtAgain
// CHECK-NEXT:   ent.run @advance
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @setDt
// CHECK-NEXT: }
ent.schedule @resources(%c: f32) {
  ent.run @readDt()
  ent.run @readDtAgain(%c) : f32
  // Writes Clock.frame, which nobody else touches: same stage.
  ent.run @advance()
  // expected-remark @+1 {{@setDt waits for @readDt: it writes Clock.dt, which @readDt reads}}
  ent.run @setDt(%c) : f32
}

// Optional components: gaining or losing one writes the presence, which
// every query binding the component reads.
ent.component @Q (q: f32)
ent.component @S (t: f32)
ent.archetype @C (@Q, optional @S) capacity 100
ent.system @stun(%t: f32) reads [@Q] writes [@S] {
  ent.query (%q: !ent.ref<@Q>) {
    ent.add @S(%t) : f32
  }
}
ent.system @countDown(%dt: f32) writes [@S] {
  ent.query (%s: !ent.ref<@S, mut>) {
    %t = ent.get %s "t" : !ent.ref<@S, mut> -> f32
    %n = arith.subf %t, %dt : f32
    ent.set %s "t", %n : !ent.ref<@S, mut>, f32
  }
}
ent.system @moveQ(%d: f32) writes [@Q] {
  ent.query (%q: !ent.ref<@Q, mut>) {
    ent.set %q "q", %d : !ent.ref<@Q, mut>, f32
  }
}

// stun waits for countDown because of the presence; moveQ writes C.Q.q,
// which neither reads (binding Q only selects entities), so it joins the
// first stage.
// CHECK-LABEL: ent.schedule @optional
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @countDown
// CHECK-NEXT:   ent.run @moveQ
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @stun
// CHECK-NEXT: }
ent.schedule @optional(%c: f32) {
  ent.run @countDown(%c) : f32
  // expected-remark @+1 {{@stun waits for @countDown: it writes C.S?, which @countDown reads}}
  ent.run @stun(%c) : f32
  ent.run @moveQ(%c) : f32
}

// Structural changes: spawning into A writes A's count and columns, so a
// query over A waits for it; a query over B only does not.
ent.system @spawnA(%c: f32) writes [@A] {
  ent.spawn @A(%c, %c, %c) : f32, f32, f32
}

// CHECK-LABEL: ent.schedule @structural
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @spawnA
// CHECK-NEXT:   ent.run @xOnB
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @readX
// CHECK-NEXT: }
ent.schedule @structural(%c: f32) {
  ent.run @spawnA(%c) : f32
  ent.run @xOnB(%c) : f32
  // expected-remark @+1 {{@readX waits for @spawnA: it reads A.P.x, which @spawnA writes}}
  ent.run @readX()
}

// Lookups read the entity table and the looked-up field in every archetype
// holding the component; a despawn writes the table, so it waits.
ent.component @Ref (entity: !ent.entity)
ent.archetype @Seeker (@Ref) capacity 100
ent.system @seek() reads [@Ref, @P] {
  ent.query (%r: !ent.ref<@Ref>) {
    %id = ent.get %r "entity" : !ent.ref<@Ref> -> !ent.entity
    %x, %found = ent.lookup %id @P "x" : f32
  }
}
ent.system @cull() reads [@P] writes [@A, @B] {
  ent.query (%p: !ent.ref<@P>) {
    ent.despawn
  }
}

// CHECK-LABEL: ent.schedule @relations
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @seek
// CHECK-NEXT:   ent.run @readX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @cull
// CHECK-NEXT: }
ent.schedule @relations() {
  ent.run @seek()
  ent.run @readX()
  // expected-remark @+1 {{@cull waits for @seek: it writes entities, which @seek reads}}
  ent.run @cull()
}

// An apply writes the field in every archetype holding the component, so
// a system reading that field waits; one reading another field does not.
ent.system @push(%d: f32) reads [@Ref] writes [@P] {
  ent.query (%r: !ent.ref<@Ref>) {
    %id = ent.get %r "entity" : !ent.ref<@Ref> -> !ent.entity
    ent.apply %id @P "y" add %d : f32
  }
}
ent.system @readY() reads [@P] {
  ent.query (%p: !ent.ref<@P>) {
    %y = ent.get %p "y" : !ent.ref<@P> -> f32
  }
}

// CHECK-LABEL: ent.schedule @applies
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @seek
// CHECK-NEXT:   ent.run @push
// CHECK-NEXT:   ent.run @readX
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @readY
// CHECK-NEXT: }
ent.schedule @applies(%d: f32) {
  ent.run @seek()
  ent.run @push(%d) : f32
  ent.run @readX()
  // expected-remark @+1 {{@readY waits for @push: it reads A.P.y, which @push writes}}
  ent.run @readY()
}

// A reactive query waits for the systems writing the stamps it observes;
// every system causing an observed event reads the tick counter that
// reactive queries advance, so they never share a stage either.
ent.component @Hp (hp: f32)
ent.component @Mood (m: f32)
ent.archetype @Unit (@Hp, @Mood) capacity 10
ent.system @hurt() writes [@Hp] {
  ent.query (%h: !ent.ref<@Hp, mut>) {
    %x = ent.get %h "hp" : !ent.ref<@Hp, mut> -> f32
    ent.set %h "hp", %x : !ent.ref<@Hp, mut>, f32
  }
}
ent.system @onHurt() reads [@Hp] writes [@Mood] {
  ent.query (%m: !ent.ref<@Mood, mut>) on [changed @Hp "hp"] {
    %c = arith.constant 1.0 : f32
    ent.set %m "m", %c : !ent.ref<@Mood, mut>, f32
  }
}
ent.system @onMood() reads [@Hp, @Mood] {
  ent.query (%h: !ent.ref<@Hp>) on [changed @Mood] {
  }
}

// CHECK-LABEL: ent.schedule @reactive
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @hurt
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @onHurt
// CHECK-NEXT: }
// CHECK-NEXT: ent.stage {
// CHECK-NEXT:   ent.run @onMood
// CHECK-NEXT: }
ent.schedule @reactive() {
  ent.run @hurt()
  // expected-remark @+1 {{@onHurt waits for @hurt: it writes ticks, which @hurt reads}}
  ent.run @onHurt()
  // expected-remark @+1 {{@onMood waits for @onHurt: it writes ticks, which @onHurt also writes}}
  ent.run @onMood()
}
