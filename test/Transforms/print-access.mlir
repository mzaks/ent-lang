// RUN: ecs-opt %s --ecs-print-access -verify-diagnostics -o /dev/null

ecs.component @P (x: f32, y: f32)
ecs.component @V (dx: f32)
ecs.archetype @A (@P, @V) capacity 1000
ecs.archetype @B (@P) capacity 1000

func.func private @log()

// A query contributes the columns it touches in every matching archetype,
// and reads the entity count of each.
// expected-remark @+1 {{reads A.P.x, B.P.x, A.count, B.count, A.V.dx; writes A.P.x, B.P.x}}
ecs.system @s(%c: f32) reads [@V] writes [@P] {
  %true = arith.constant true
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    ecs.set %p "x", %x : !ecs.ref<@P, mut>, f32
  }
  // Binding P without accessing it does not count; V selects only A.
  ecs.query (%p: !ecs.ref<@P>, %v: !ecs.ref<@V>) {
    %dx = ecs.get %v "dx" : !ecs.ref<@V> -> f32
    scf.if %true {
      %again = ecs.get %v "dx" : !ecs.ref<@V> -> f32
    }
  }
}

// expected-remark @+1 {{reads nothing; writes nothing}}
ecs.system @opaque() {
  // expected-note @+1 {{has effects outside component access, so the system conflicts with every other system}}
  func.call @log() : () -> ()
}

// Resource fields are columns without an archetype. Reading one inside a
// query counts once, however many archetypes the query matches.
ecs.resource @Clock (dt: f32, frame: i64)
// expected-remark @+1 {{reads Clock.frame, A.P.x, B.P.x, Clock.dt, A.count, B.count; writes Clock.frame, A.P.x, B.P.x}}
ecs.system @tick() writes [@P, @Clock] {
  %f = ecs.read @Clock "frame" : i64
  ecs.write @Clock "frame", %f : i64
  ecs.query (%p: !ecs.ref<@P, mut>) {
    %x = ecs.get %p "x" : !ecs.ref<@P, mut> -> f32
    %dt = ecs.read @Clock "dt" : f32
    %n = arith.addf %x, %dt : f32
    ecs.set %p "x", %n : !ecs.ref<@P, mut>, f32
  }
}

// Optional components: binding one reads its presence where it is
// optional; ecs.remove writes the presence, ecs.add also the fields.
ecs.component @Q (q: f32)
ecs.component @S (t: f32, u: f32)
ecs.archetype @C (@Q, optional @S) capacity 1000
// expected-remark @+1 {{reads C.S.t, C.count, C.S?; writes C.S?}}
ecs.system @expire() writes [@S] {
  ecs.query (%s: !ecs.ref<@S, mut>) {
    %t = ecs.get %s "t" : !ecs.ref<@S, mut> -> f32
    ecs.remove @S
  }
}
// expected-remark @+1 {{reads C.count; writes C.S?, C.S.t, C.S.u}}
ecs.system @stun(%t: f32) reads [@Q] writes [@S] {
  ecs.query (%q: !ecs.ref<@Q>) {
    ecs.add @S(%t, %t) : f32, f32
  }
}

// Spawning and despawning change which entities an archetype holds: they
// write its count, its id column, every other column, and the entity table
// (`entities`), which maps ids to archetypes and rows.
ecs.component @R (r: f32)
ecs.archetype @D (@R) capacity 10
// expected-remark @+1 {{reads D.count; writes entities, D.count, D.id, D.R.r}}
ecs.system @recycle(%v: f32) reads [@R] writes [@D] {
  ecs.spawn @D(%v) : f32
  ecs.query (%r: !ecs.ref<@R>) {
    ecs.despawn
  }
}

// Adding a component that one matched archetype stores and the other does
// not: overwrite in E2, move from E1 to E2 (both archetypes' structure).
// ecs.entity reads the id column.
ecs.component @M (m: f32)
ecs.component @N (n: f32)
ecs.archetype @E1 (@M) capacity 10
ecs.archetype @E2 (@M, @N) capacity 10
// expected-remark @+1 {{reads E1.id, E2.id, E1.count, E2.count; writes entities, E1.count, E1.id, E1.M.m, E2.count, E2.id, E2.M.m, E2.N.n}}
ecs.system @promote(%v: f32) reads [@M] writes [@N] {
  ecs.query (%m: !ecs.ref<@M>) {
    %id = ecs.entity
    ecs.add @N(%v) : f32
  }
}

// A lookup reads the entity table and the field in every archetype that
// holds the component: the entity may live in any of them.
ecs.component @Ref (entity: !ecs.entity)
ecs.archetype @G (@Ref) capacity 10
// expected-remark @+1 {{reads G.Ref.entity, entities, D.R.r, G.count; writes nothing}}
ecs.system @chase() reads [@Ref, @R] {
  ecs.query (%t: !ecs.ref<@Ref>) {
    %id = ecs.get %t "entity" : !ecs.ref<@Ref> -> !ecs.entity
    %r, %found = ecs.lookup %id @R "r" : f32
  }
}

// An apply reads the entity table and writes the field in every archetype
// that holds the component (reading the presence where it is optional).
// expected-remark @+1 {{reads G.Ref.entity, entities, C.S?, G.count; writes C.S.t}}
ecs.system @hit(%d: f32) reads [@Ref] writes [@S] {
  ecs.query (%t: !ecs.ref<@Ref>) {
    %id = ecs.get %t "entity" : !ecs.ref<@Ref> -> !ecs.entity
    ecs.apply %id @S "t" add %d : f32
  }
}

// Reactive queries read the stamps of their triggers and advance the tick
// counter; ops that cause observed events write the stamps (in every
// archetype that carries them) and read the counter. Writing an unobserved
// field (u) stamps nothing.
ecs.component @Hp (hp: f32, u: f32)
ecs.component @Guard ()
ecs.archetype @Unit (@Hp, optional @Guard) capacity 10
// expected-remark @+1 {{reads Unit.count, Unit.Hp.hp@, Unit.Guard+, Unit.Guard-; writes ticks}}
ecs.system @watch() reads [@Hp, @Guard] {
  ecs.query (%h: !ecs.ref<@Hp>)
      on [changed @Hp "hp", added @Guard, removed @Guard] {
  }
}
// expected-remark @+1 {{reads Unit.Hp.hp, ticks, Unit.Hp.u, Unit.count; writes Unit.Hp.hp@, Unit.Hp.hp, Unit.Hp.u}}
ecs.system @hurt() writes [@Hp] {
  ecs.query (%h: !ecs.ref<@Hp, mut>) {
    %x = ecs.get %h "hp" : !ecs.ref<@Hp, mut> -> f32
    ecs.set %h "hp", %x : !ecs.ref<@Hp, mut>, f32
    %y = ecs.get %h "u" : !ecs.ref<@Hp, mut> -> f32
    ecs.set %h "u", %y : !ecs.ref<@Hp, mut>, f32
  }
}
// expected-remark @+1 {{reads ticks, Unit.count; writes Unit.Guard?, Unit.Guard+, Unit.Guard-}}
ecs.system @toggle() reads [@Hp] writes [@Guard] {
  ecs.query (%h: !ecs.ref<@Hp>) {
    ecs.add @Guard()
    ecs.remove @Guard
  }
}
