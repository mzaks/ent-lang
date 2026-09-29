// One spawn per frame into an archetype of capacity 2.
ecs.component @P (x: f32)
ecs.archetype @A (@P) capacity 2
ecs.system @grow(%x: f32) writes [@A] {
  ecs.spawn @A(%x) : f32
}
ecs.schedule @frame(%x: f32) {
  ecs.run @grow(%x) : f32
}
