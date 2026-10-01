// Relations with generational ids: cells connected by the host and by a
// query (to their next), weak edges disconnected, a cell despawned in
// frame 1 and its slot reused by a new cell in frame 2, whose stale edges
// must not count.

ent.component @N (v: f32, input: f32, next: !ent.entity) capacity 8
ent.archetype @Cell (@N) capacity 8
ent.relation @Syn (w: f32) capacity 16
ent.resource @Clock (frame: i64)

// Frame 0: every cell connects to its next, with its v as weight.
ent.system @link() {
  ent.query (%n: !ent.ref<@N>) {
    %self = ent.entity
    %next = ent.get %n "next" : !ent.ref<@N> -> !ent.entity
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    ent.connect @Syn %self, %next (%v) : f32
  }
}

// Weak edges are dropped.
ent.system @prune() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn out (%s: !ent.ref<@Syn>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      %one = arith.constant 1.0 : f32
      %weak = arith.cmpf olt, %w, %one : f32
      scf.if %weak {
        ent.disconnect
      }
    }
  }
}

ent.system @push() {
  ent.query (%n: !ent.ref<@N>) {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    ent.edges @Syn out (%s: !ent.ref<@Syn>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      %x = arith.mulf %v, %w : f32
      ent.apply %t @N "input" add %x : f32
    }
  }
}

// Frame 1: the cell with v 3 dies.
ent.system @kill() {
  ent.query (%n: !ent.ref<@N>) {
    %v = ent.get %n "v" : !ent.ref<@N> -> f32
    %three = arith.constant 3.0 : f32
    %dies = arith.cmpf oeq, %v, %three : f32
    scf.if %dies {
      ent.despawn
    }
  }
}

ent.system @tick() {
  %f = ent.read @Clock "frame" : i64
  %one = arith.constant 1 : i64
  %g = arith.addi %f, %one : i64
  ent.write @Clock "frame", %g : i64
}

ent.schedule @frame() {
  ent.run @link() if {
    %f = ent.read @Clock "frame" : i64
    %z = arith.constant 0 : i64
    %c = arith.cmpi eq, %f, %z : i64
    ent.yield %c : i1
  }
  ent.run @prune()
  ent.run @push()
  ent.run @kill() if {
    %f = ent.read @Clock "frame" : i64
    %z = arith.constant 1 : i64
    %c = arith.cmpi eq, %f, %z : i64
    ent.yield %c : i1
  }
  ent.run @tick()
}
