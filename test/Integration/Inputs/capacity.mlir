// One spawn per frame into an archetype of capacity 2.
ent.component @P (x: f32)
ent.archetype @A (@P) capacity 2
ent.system @grow(%x: f32) writes [@A] {
  ent.spawn @A(%x) : f32
}
ent.schedule @frame(%x: f32) {
  ent.run @grow(%x) : f32
}
