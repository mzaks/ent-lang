// A query visits the entities that exist when it starts (docs/sync-points.md,
// Q1). It matches @A and @B, and every entity it visits for the first time
// spawns a @B: the one spawned while visiting @A must not be visited by the
// loop over @B that follows in the same query.
ecs.component @T (visits: i32)
ecs.component @X ()
ecs.archetype @A (@T, @X) capacity 8
ecs.archetype @B (@T) capacity 8
ecs.system @s() writes [@T, @B] {
  ecs.query (%t: !ecs.ref<@T, mut>) {
    %v = ecs.get %t "visits" : !ecs.ref<@T, mut> -> i32
    %one = arith.constant 1 : i32
    %n = arith.addi %v, %one : i32
    ecs.set %t "visits", %n : !ecs.ref<@T, mut>, i32
    %zero = arith.constant 0 : i32
    %first = arith.cmpi eq, %v, %zero : i32
    scf.if %first {
      %id = ecs.spawn @B(%zero) : i32
    }
  }
}
ecs.schedule @frame() {
  ecs.run @s()
}
