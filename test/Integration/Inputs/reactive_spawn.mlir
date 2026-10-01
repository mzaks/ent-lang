// Entities spawned between runs of a reactive query, by the host through
// the header or by the program, are added events: the query finds them
// in its event log.

ent.component @Hull (hp: f32)
ent.component @Seen (count: i32)

ent.archetype @Unit (@Hull, @Seen) capacity 16

ent.system @breed(%spawn: i1) writes [@Unit] {
  scf.if %spawn {
    %hp = arith.constant 10.0 : f32
    %zero = arith.constant 0 : i32
    %id = ent.spawn @Unit(%hp, %zero) : f32, i32
  }
}

ent.system @greet() reads [@Hull] writes [@Seen] {
  ent.query (%s: !ent.ref<@Seen, mut>) on [added @Hull] {
    %n = ent.get %s "count" : !ent.ref<@Seen, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ent.set %s "count", %next : !ent.ref<@Seen, mut>, i32
  }
}

ent.schedule @frame(%spawn: i1) {
  ent.run @breed(%spawn) : i1
  ent.run @greet()
}
