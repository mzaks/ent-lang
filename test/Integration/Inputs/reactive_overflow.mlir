// More entities change between two runs of a reactive query than its event
// log holds (`log 2`, four units): the query must notice and scan, so it
// still visits all of them.

ent.component @Hull (hp: f32)
ent.component @Seen (count: i32)

ent.archetype @Unit (@Hull, @Seen) capacity 16

ent.system @hurt(%all: i1) writes [@Hull] {
  ent.query (%h: !ent.ref<@Hull, mut>) {
    scf.if %all {
      %hp = ent.get %h "hp" : !ent.ref<@Hull, mut> -> f32
      %one = arith.constant 1.0 : f32
      %less = arith.subf %hp, %one : f32
      ent.set %h "hp", %less : !ent.ref<@Hull, mut>, f32
    }
  }
}

ent.system @count() reads [@Hull] writes [@Seen] {
  ent.query (%s: !ent.ref<@Seen, mut>) on [changed @Hull "hp" log 2] {
    %n = ent.get %s "count" : !ent.ref<@Seen, mut> -> i32
    %one = arith.constant 1 : i32
    %next = arith.addi %n, %one : i32
    ent.set %s "count", %next : !ent.ref<@Seen, mut>, i32
  }
}

ent.schedule @frame(%all: i1) {
  ent.run @hurt(%all) : i1
  ent.run @count()
}
