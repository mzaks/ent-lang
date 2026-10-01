// More entities change between two runs of a reactive query than its event
// log holds (`log 2`, four units): the query must notice and scan, so it
// still visits all of them.

ecs.component @Hull (hp: f32)
ecs.component @Seen (count: i32)

ecs.archetype @Unit (@Hull, @Seen) capacity 16

ecs.system @hurt(%all: i1) writes [@Hull] {
  ecs.query (%h: !ecs.ref<@Hull, mut>) {
    scf.if %all {
      %hp = ecs.get %h "hp" : !ecs.ref<@Hull, mut> -> f32
      %one = arith.constant 1.0 : f32
      %less = arith.subf %hp, %one : f32
      ecs.set %h "hp", %less : !ecs.ref<@Hull, mut>, f32
    }
  }
}

ecs.system @count() reads [@Hull] writes [@Seen] {
  ecs.query (%s: !ecs.ref<@Seen, mut>) on [changed @Hull "hp" log 2] {
    %n = ecs.get %s "count" : !ecs.ref<@Seen, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ecs.set %s "count", %next : !ecs.ref<@Seen, mut>, i32
  }
}

ecs.schedule @frame(%all: i1) {
  ecs.run @hurt(%all) : i1
  ecs.run @count()
}
