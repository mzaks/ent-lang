// Entities spawned between runs of a reactive query, by the host through
// the header or by the program, are added events: the query finds them
// in its event log.

ecs.component @Hull (hp: f32)
ecs.component @Seen (count: i32)

ecs.archetype @Unit (@Hull, @Seen) capacity 16

ecs.system @breed(%spawn: i1) writes [@Unit] {
  scf.if %spawn {
    %hp = arith.constant 10.0 : f32
    %zero = arith.constant 0 : i32
    %id = ecs.spawn @Unit(%hp, %zero) : f32, i32
  }
}

ecs.system @greet() reads [@Hull] writes [@Seen] {
  ecs.query (%s: !ecs.ref<@Seen, mut>) on [added @Hull] {
    %n = ecs.get %s "count" : !ecs.ref<@Seen, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ecs.set %s "count", %next : !ecs.ref<@Seen, mut>, i32
  }
}

ecs.schedule @frame(%spawn: i1) {
  ecs.run @breed(%spawn) : i1
  ecs.run @greet()
}
