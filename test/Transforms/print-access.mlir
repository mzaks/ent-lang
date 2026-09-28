// RUN: ecs-opt %s --ecs-print-access -verify-diagnostics -o /dev/null

ecs.component @P (x: f32, y: f32)
ecs.component @V (dx: f32)
ecs.archetype @A (@P, @V)
ecs.archetype @B (@P)

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
