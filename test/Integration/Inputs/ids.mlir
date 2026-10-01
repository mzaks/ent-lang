// Despawns every entity whose flag is set.
ent.component @P (flag: i32)
ent.archetype @A (@P) capacity 8

ent.system @cull() reads [@P] writes [@A] {
  ent.query (%p: !ent.ref<@P>) {
    %flag = ent.get %p "flag" : !ent.ref<@P> -> i32
    %zero = arith.constant 0 : i32
    %set = arith.cmpi ne, %flag, %zero : i32
    scf.if %set {
      ent.despawn
    }
  }
}

ent.schedule @frame() {
  ent.run @cull()
}
