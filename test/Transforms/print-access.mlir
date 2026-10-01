// RUN: ent-opt %s --ent-print-access -verify-diagnostics -o /dev/null

ent.component @P (x: f32, y: f32)
ent.component @V (dx: f32)
ent.archetype @A (@P, @V) capacity 1000
ent.archetype @B (@P) capacity 1000

func.func private @log()

// A query contributes the columns it touches in every matching archetype,
// and reads the entity count of each.
// expected-remark @+1 {{reads A.P.x, B.P.x, A.count, B.count, A.V.dx; writes A.P.x, B.P.x}}
ent.system @s(%c: f32) reads [@V] writes [@P] {
  %true = arith.constant true
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    ent.set %p "x", %x : !ent.ref<@P, mut>, f32
  }
  // Binding P without accessing it does not count; V selects only A.
  ent.query (%p: !ent.ref<@P>, %v: !ent.ref<@V>) {
    %dx = ent.get %v "dx" : !ent.ref<@V> -> f32
    scf.if %true {
      %again = ent.get %v "dx" : !ent.ref<@V> -> f32
    }
  }
}

// expected-remark @+1 {{reads nothing; writes nothing}}
ent.system @opaque() {
  // expected-note @+1 {{has effects outside component access, so the system conflicts with every other system}}
  func.call @log() : () -> ()
}

// Resource fields are columns without an archetype. Reading one inside a
// query counts once, however many archetypes the query matches.
ent.resource @Clock (dt: f32, frame: i64)
// expected-remark @+1 {{reads Clock.frame, A.P.x, B.P.x, Clock.dt, A.count, B.count; writes Clock.frame, A.P.x, B.P.x}}
ent.system @tick() writes [@P, @Clock] {
  %f = ent.read @Clock "frame" : i64
  ent.write @Clock "frame", %f : i64
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    %dt = ent.read @Clock "dt" : f32
    %n = arith.addf %x, %dt : f32
    ent.set %p "x", %n : !ent.ref<@P, mut>, f32
  }
}

// Optional components: binding one reads its presence where it is
// optional; ent.remove writes the presence, ent.add also the fields.
ent.component @Q (q: f32)
ent.component @S (t: f32, u: f32)
ent.archetype @C (@Q, optional @S) capacity 1000
// expected-remark @+1 {{reads C.S.t, C.count, C.S?; writes C.S?}}
ent.system @expire() writes [@S] {
  ent.query (%s: !ent.ref<@S, mut>) {
    %t = ent.get %s "t" : !ent.ref<@S, mut> -> f32
    ent.remove @S
  }
}
// expected-remark @+1 {{reads C.count; writes C.S?, C.S.t, C.S.u}}
ent.system @stun(%t: f32) reads [@Q] writes [@S] {
  ent.query (%q: !ent.ref<@Q>) {
    ent.add @S(%t, %t) : f32, f32
  }
}

// Spawning and despawning change which entities an archetype holds: they
// write its count, its id column, every other column, and the entity table
// (`entities`), which maps ids to archetypes and rows.
ent.component @R (r: f32)
ent.archetype @D (@R) capacity 10
// expected-remark @+1 {{reads D.count; writes entities, D.count, D.id, D.R.r}}
ent.system @recycle(%v: f32) reads [@R] writes [@D] {
  ent.spawn @D(%v) : f32
  ent.query (%r: !ent.ref<@R>) {
    ent.despawn
  }
}

// Adding a component that one matched archetype stores and the other does
// not: overwrite in E2, move from E1 to E2 (both archetypes' structure).
// ent.entity reads the id column.
ent.component @M (m: f32)
ent.component @N (n: f32)
ent.archetype @E1 (@M) capacity 10
ent.archetype @E2 (@M, @N) capacity 10
// expected-remark @+1 {{reads E1.id, E2.id, E1.count, E2.count; writes entities, E1.count, E1.id, E1.M.m, E2.count, E2.id, E2.M.m, E2.N.n}}
ent.system @promote(%v: f32) reads [@M] writes [@N] {
  ent.query (%m: !ent.ref<@M>) {
    %id = ent.entity
    ent.add @N(%v) : f32
  }
}

// A lookup reads the entity table and the field in every archetype that
// holds the component: the entity may live in any of them.
ent.component @Ref (entity: !ent.entity)
ent.archetype @G (@Ref) capacity 10
// expected-remark @+1 {{reads G.Ref.entity, entities, D.R.r, G.count; writes nothing}}
ent.system @chase() reads [@Ref, @R] {
  ent.query (%t: !ent.ref<@Ref>) {
    %id = ent.get %t "entity" : !ent.ref<@Ref> -> !ent.entity
    %r, %found = ent.lookup %id @R "r" : f32
  }
}

// An apply reads the entity table and writes the field in every archetype
// that holds the component (reading the presence where it is optional).
// expected-remark @+1 {{reads G.Ref.entity, entities, C.S?, G.count; writes C.S.t}}
ent.system @hit(%d: f32) reads [@Ref] writes [@S] {
  ent.query (%t: !ent.ref<@Ref>) {
    %id = ent.get %t "entity" : !ent.ref<@Ref> -> !ent.entity
    ent.apply %id @S "t" add %d : f32
  }
}

// Reactive queries read the stamps of their triggers and advance the tick
// counter; ops that cause observed events write the stamps (in every
// archetype that carries them) and read the counter. Writing an unobserved
// field (u) stamps nothing.
ent.component @Hp (hp: f32, u: f32)
ent.component @Guard ()
ent.archetype @Unit (@Hp, optional @Guard) capacity 10
// expected-remark @+1 {{reads Unit.count, Unit.Hp.hp@, Unit.Guard+, Unit.Guard-; writes ticks}}
ent.system @watch() reads [@Hp, @Guard] {
  ent.query (%h: !ent.ref<@Hp>)
      on [changed @Hp "hp", added @Guard, removed @Guard] {
  }
}
// expected-remark @+1 {{reads Unit.Hp.hp, ticks, Unit.Hp.u, Unit.count; writes Unit.Hp.hp@, Unit.Hp.hp, Unit.Hp.u}}
ent.system @hurt() writes [@Hp] {
  ent.query (%h: !ent.ref<@Hp, mut>) {
    %x = ent.get %h "hp" : !ent.ref<@Hp, mut> -> f32
    ent.set %h "hp", %x : !ent.ref<@Hp, mut>, f32
    %y = ent.get %h "u" : !ent.ref<@Hp, mut> -> f32
    ent.set %h "u", %y : !ent.ref<@Hp, mut>, f32
  }
}
// expected-remark @+1 {{reads ticks, Unit.count; writes Unit.Guard?, Unit.Guard+, Unit.Guard-}}
ent.system @toggle() reads [@Hp] writes [@Guard] {
  ent.query (%h: !ent.ref<@Hp>) {
    ent.add @Guard()
    ent.remove @Guard
  }
}

// An accumulate writes the resource field (and reads it: a write conflicts
// like a read too).
ent.resource @Score (points: i64)
// expected-remark @+1 {{reads A.count, B.count; writes Score.points}}
ent.system @scoreAll(%v: i64) reads [@P] writes [@Score] {
  ent.query (%p: !ent.ref<@P>) {
    ent.accumulate @Score "points" add %v : i64
  }
}

// Without declarations the analysis is the whole story: a system's access
// is what its body does.
// expected-remark @+1 {{reads A.P.x, B.P.x, Score.points, A.count, B.count; writes A.P.y, B.P.y}}
ent.system @undeclared() {
  ent.query (%p: !ent.ref<@P, mut>) {
    %x = ent.get %p "x" : !ent.ref<@P, mut> -> f32
    ent.set %p "y", %x : !ent.ref<@P, mut>, f32
    %s = ent.read @Score "points" : i64
  }
}
