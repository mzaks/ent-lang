// Relations with row ids: neurons spawned and connected by a system in
// frame 0; push (apply per edge), pull (lookup per edge) and learning (mut
// edge weights).

ent.component @N (v: f32, input: f32, gathered: f32) capacity 8
ent.relation @Syn (w: f32) capacity 16
ent.resource @Clock (frame: i64)

ent.system @setup() {
  %one = arith.constant 1.0 : f32
  %two = arith.constant 2.0 : f32
  %three = arith.constant 3.0 : f32
  %z = arith.constant 0.0 : f32
  %a = ent.spawn (@N)(%one, %z, %z) : f32, f32, f32
  %b = ent.spawn (@N)(%two, %z, %z) : f32, f32, f32
  %c = ent.spawn (@N)(%three, %z, %z) : f32, f32, f32
  %w1 = arith.constant 0.5 : f32
  %w2 = arith.constant 0.25 : f32
  %w3 = arith.constant 4.0 : f32
  ent.connect @Syn %a, %b (%w1) : f32
  ent.connect @Syn %a, %c (%w2) : f32
  ent.connect @Syn %b, %c (%w3) : f32
  ent.connect @Syn %c, %a (%w1) : f32
}

// Push: every neuron sends v * w along its outgoing edges.
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

// Pull: every neuron sums v * w over its incoming edges.
ent.system @pull() {
  ent.query (%n: !ent.ref<@N, mut>) {
    ent.edges @Syn in (%s: !ent.ref<@Syn>, %p: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn> -> f32
      %pv, %found = ent.lookup %p @N "v" : f32
      %x = arith.mulf %pv, %w : f32
      %g = ent.get %n "gathered" : !ent.ref<@N, mut> -> f32
      %y = arith.addf %g, %x : f32
      ent.set %n "gathered", %y : !ent.ref<@N, mut>, f32
    }
  }
}

// Learning: weights grow by 1 each frame.
ent.system @learn() {
  ent.query (%n: !ent.ref<@N>) {
    ent.edges @Syn out (%s: !ent.ref<@Syn, mut>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Syn, mut> -> f32
      %one = arith.constant 1.0 : f32
      %w2 = arith.addf %w, %one : f32
      ent.set %s "w", %w2 : !ent.ref<@Syn, mut>, f32
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
  ent.run @setup() if {
    %f = ent.read @Clock "frame" : i64
    %z = arith.constant 0 : i64
    %first = arith.cmpi eq, %f, %z : i64
    ent.yield %first : i1
  }
  ent.run @push()
  ent.run @pull()
  ent.run @learn()
  ent.run @tick()
}
