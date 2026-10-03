// A relation that names its ends' components: edges go from entities with
// @N to entities with @Hub, which is optional in @A. Nothing despawns or
// removes @Hub, so reading it through an edge needs no checks.

ent.component @N (v: f32) capacity 8
ent.component @Hub (h: f32) capacity 8
ent.archetype @A (@N, optional @Hub) capacity 8
ent.relation @Feeds (w: f32) from @N to @Hub capacity 8

ent.system @pull() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Feeds out (%s: !ent.ref<@Feeds>, %t: !ent.entity) {
      %h, %found = ent.lookup %t @Hub "h" : f32
      %w = ent.get %s "w" : !ent.ref<@Feeds> -> f32
      %x = arith.mulf %h, %w : f32
      %v = ent.get %n "v" : !ent.ref<@N, mut> -> f32
      %y = arith.addf %v, %x : f32
      ent.set %n "v", %y : !ent.ref<@N, mut>, f32
    }
  }
}

ent.schedule @frame() {
  ent.run @pull()
}
