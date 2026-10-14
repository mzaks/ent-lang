// RUN: ent-opt %s "--ent-lower-to-loops=explain=1" -verify-diagnostics -o /dev/null

// Whole numbers come to the same in whatever order they are combined:
// a reactive query that sends only such, to a field it reads nothing of
// (looking whether the entity has the component reads nothing), or
// counts into a resource, walks its triggers' logs. No remark.
ent.component @H (hp: f32)
ent.component @T (entity: !ent.entity)
ent.component @N (n: i32)
ent.archetype @B (@H, @T, @N) capacity 8
ent.resource @Count (value: i32)

ent.system @hurt(%d: f32) writes [@H] {
  ent.query (%h: !ent.ref<@H, mut>) {
    ent.set %h "hp", %d : !ent.ref<@H, mut>, f32
  }
}

ent.system @told() reads [@T, @H] writes [@N, @Count] {
  ent.query (%t: !ent.ref<@T>) on [changed @H "hp"] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    %value, %found = ent.lookup %id @N "n" : i32
    %seven = arith.constant 7 : i32
    scf.if %found {
      ent.apply %id @N "n" max %seven : i32
    }
    %one = arith.constant 1 : i32
    ent.accumulate @Count "value" add %one : i32
  }
}

// One that reads what it sends to waits for its end, in the order of rows.
ent.system @read() reads [@T, @H] writes [@N] {
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, connects edges or appends rows, which are combined in row order}}
  ent.query (%t: !ent.ref<@T>, %n: !ent.ref<@N>) on [changed @H "hp"] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    %mine = ent.get %n "n" : !ent.ref<@N> -> i32
    ent.apply %id @N "n" add %mine : i32
  }
}

// And a float, whose sum depends on the order: the rows from the logs
// are put in order and visited then (kRowOrderedAttr), one op sending.
ent.system @pass(%d: f32) reads [@T, @N] writes [@H] {
  ent.query (%t: !ent.ref<@T>) on [changed @N "n"] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
  }
}

// Two ops sending floats to one field would be combined op after op.
ent.system @twice(%d: f32) reads [@T, @N] writes [@H] {
  // expected-remark @+1 {{scans every entity on each run: it applies or accumulates values, connects edges or appends rows, which are combined in row order}}
  ent.query (%t: !ent.ref<@T>) on [changed @N "n"] {
    %id = ent.get %t "entity" : !ent.ref<@T> -> !ent.entity
    ent.apply %id @H "hp" add %d : f32
    ent.apply %id @H "hp" add %d : f32
  }
}
