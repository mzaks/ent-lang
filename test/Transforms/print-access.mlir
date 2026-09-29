// RUN: ecs-opt %s --ecs-print-access -verify-diagnostics -o /dev/null

ecs.component @P (x: f32, y: f32)
ecs.component @V (dx: f32)
ecs.archetype @A (@P, @V) capacity 1000
ecs.archetype @B (@P) capacity 1000

func.func private @log()

// A query contributes the columns it touches in every matching archetype.
// expected-remark @+1 {{reads A.P.x, B.P.x, A.V.dx; writes A.P.x, B.P.x}}
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
// expected-remark @+1 {{reads Clock.frame, A.P.x, B.P.x, Clock.dt; writes Clock.frame, A.P.x, B.P.x}}
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
// expected-remark @+1 {{reads C.S.t, C.S?; writes C.S?}}
ecs.system @expire() writes [@S] {
  ecs.query (%s: !ecs.ref<@S, mut>) {
    %t = ecs.get %s "t" : !ecs.ref<@S, mut> -> f32
    ecs.remove @S
  }
}
// expected-remark @+1 {{reads nothing; writes C.S?, C.S.t, C.S.u}}
ecs.system @stun(%t: f32) reads [@Q] writes [@S] {
  ecs.query (%q: !ecs.ref<@Q>) {
    ecs.add @S(%t, %t) : f32, f32
  }
}
