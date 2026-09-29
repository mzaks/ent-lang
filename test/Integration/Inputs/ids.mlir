// Despawns every entity whose flag is set.
ecs.component @P (flag: i32)
ecs.archetype @A (@P) capacity 8

ecs.system @cull() reads [@P] writes [@A] {
  ecs.query (%p: !ecs.ref<@P>) {
    %flag = ecs.get %p "flag" : !ecs.ref<@P> -> i32
    %zero = arith.constant 0 : i32
    %set = arith.cmpi ne, %flag, %zero : i32
    scf.if %set {
      ecs.despawn
    }
  }
}

ecs.schedule @frame() {
  ecs.run @cull()
}
