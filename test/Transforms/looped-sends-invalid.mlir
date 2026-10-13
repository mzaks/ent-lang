// RUN: ent-opt %s -split-input-file --ent-lower-to-loops -verify-diagnostics -o /dev/null

// What is sent from a loop of a query's body lands in the order it is
// sent in, which on one thread is where it is sent: so the query reads
// the field nowhere.
ent.component @H (hp: f32)
ent.archetype @A (@H) capacity 10
ent.system @s(%d: f32) {
  ent.query (%h: !ent.ref<@H>) {
    %id = ent.entity
    %hp = ent.get %h "hp" : <@H> -> f32
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c3 = arith.constant 3 : index
    scf.for %i = %c0 to %c3 step %c1 {
      // expected-error @+1 {{is in a loop of a 'for's body, so nothing else in the 'for' may read, set or send to the field it sends to}}
      ent.apply %id @H "hp" add %hp : f32
    }
  }
}

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
ent.resource @Score (points: i64)
ent.system @s(%v: i64) {
  ent.query (%p: !ent.ref<@P>) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %seen = ent.read @Score "points" : i64
    scf.for %i = %c0 to %c1 step %c1 {
      // expected-error @+1 {{is in a loop of a 'for's body, so nothing else in the 'for' may read, set or send to the field it sends to}}
      ent.accumulate @Score "points" add %seen : i64
    }
  }
}
