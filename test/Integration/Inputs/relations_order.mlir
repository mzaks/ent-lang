// The order edge loops visit edges in: by source, then in the order they
// were connected, however the relation is stored. @Only is visited only by
// incoming edges (stored sorted by target), @Both both ways (sorted by
// source, incoming edges through the index). Edges into the first entity
// are connected out of source order; reading them as digits (x * 10 + w)
// shows the order.

ent.component @N (only: i32, both: i32, out: i32) capacity 8
ent.relation @Only (w: i32) capacity 16
ent.relation @Both (w: i32) capacity 16

ent.system @setup() {
  %z = arith.constant 0 : i32
  %a = ent.spawn (@N)(%z, %z, %z) : i32, i32, i32
  %b = ent.spawn (@N)(%z, %z, %z) : i32, i32, i32
  %c = ent.spawn (@N)(%z, %z, %z) : i32, i32, i32
  %one = arith.constant 1 : i32
  %two = arith.constant 2 : i32
  %three = arith.constant 3 : i32
  %four = arith.constant 4 : i32
  %five = arith.constant 5 : i32
  // Into a: from c (3), then a (1), then b (2), then a again (4); b -> c (5).
  ent.connect @Only %c, %a (%three) : i32
  ent.connect @Only %a, %a (%one) : i32
  ent.connect @Only %b, %a (%two) : i32
  ent.connect @Only %a, %a (%four) : i32
  ent.connect @Only %b, %c (%five) : i32
  ent.connect @Both %c, %a (%three) : i32
  ent.connect @Both %a, %a (%one) : i32
  ent.connect @Both %b, %a (%two) : i32
  ent.connect @Both %a, %a (%four) : i32
  ent.connect @Both %b, %c (%five) : i32
}

// x * 10 + w over the incoming edges, in the order visited.
ent.system @read() {
  ent.query (%n: !ent.ref<@N, mut>) {
    %ten = arith.constant 10 : i32
    ent.edges @Only in (%s: !ent.ref<@Only>, %p: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Only> -> i32
      %x = ent.get %n "only" : !ent.ref<@N, mut> -> i32
      %y = arith.muli %x, %ten : i32
      %z = arith.addi %y, %w : i32
      ent.set %n "only", %z : !ent.ref<@N, mut>, i32
    }
    ent.edges @Both in (%s: !ent.ref<@Both>, %p: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Both> -> i32
      %x = ent.get %n "both" : !ent.ref<@N, mut> -> i32
      %y = arith.muli %x, %ten : i32
      %z = arith.addi %y, %w : i32
      ent.set %n "both", %z : !ent.ref<@N, mut>, i32
    }
  }
}

// @Both is also visited by outgoing edges.
ent.system @fanout() {
  ent.query (%n: !ent.ref<@N, mut>) {
    %ten = arith.constant 10 : i32
    ent.edges @Both out (%s: !ent.ref<@Both>, %t: !ent.entity) {
      %w = ent.get %s "w" : !ent.ref<@Both> -> i32
      %x = ent.get %n "out" : !ent.ref<@N, mut> -> i32
      %y = arith.muli %x, %ten : i32
      %z = arith.addi %y, %w : i32
      ent.set %n "out", %z : !ent.ref<@N, mut>, i32
    }
  }
}

ent.schedule @frame() {
  ent.run @setup()
  ent.run @read()
  ent.run @fanout()
}
