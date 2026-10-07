// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// CHECK: ent.relation @Under () from @Node to @Node tree capacity 16
// CHECK: ent.relation @Owns (share: f32) tree capacity 4
ent.component @Node (local: i32, total: i32) capacity 8
ent.component @Mark (color: i32)
ent.relation @Under () from @Node to @Node tree capacity 16
ent.relation @Owns (share: f32) tree capacity 4
ent.archetype @Plain (@Node) capacity 8
ent.archetype @Marked (@Node, @Mark) capacity 8

// A ref up a tree and the order that lets the query write what it reads
// there.
// CHECK-LABEL: ent.system @sum() {
// CHECK:   ent.query (%[[N:.*]]: !ent.ref<@Node, mut>, %[[P:.*]]: !ent.ref<@Node, up @Under>) cascade @Under {
// CHECK:     ent.get %[[P]] "total" : <@Node, up @Under> -> i32
ent.system @sum() {
  ent.query (%n: !ent.ref<@Node, mut>, %p: !ent.ref<@Node, up @Under>)
      cascade @Under {
    %above = ent.get %p "total" : !ent.ref<@Node, up @Under> -> i32
    %own = ent.get %n "local" : !ent.ref<@Node, mut> -> i32
    %total = arith.addi %above, %own : i32
    ent.set %n "total", %total : !ent.ref<@Node, mut>, i32
  }
}

// An ancestor's component may be one the entity itself must not have, and
// a system that declares its access names the tree.
// CHECK-LABEL: ent.system @inherit() reads [@Node, @Mark, @Under] {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node>, %{{.*}}: !ent.ref<@Mark, up @Under>) without [@Mark] {
ent.system @inherit() reads [@Node, @Mark, @Under] {
  ent.query (%n: !ent.ref<@Node>, %m: !ent.ref<@Mark, up @Under>)
      without [@Mark] {
    %color = ent.get %m "color" : !ent.ref<@Mark, up @Under> -> i32
  }
}

// Ordered only, after the filters.
// CHECK-LABEL: ent.system @visit() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node>) with [@Mark] cascade @Under {
ent.system @visit() {
  ent.query (%n: !ent.ref<@Node>) with [@Mark] cascade @Under {
  }
}

// Children before their parents, each combining into its parent.
// CHECK-LABEL: ent.system @gather() {
// CHECK:   ent.query (%[[N:.*]]: !ent.ref<@Node>, %[[D:.*]]: !ent.ref<@Node, mut, up @Under>) cascade @Under leaves first {
// CHECK:     ent.combine %[[D]] "total" add %{{.*}} : <@Node, mut, up @Under>, i32
ent.system @gather() {
  ent.query (%n: !ent.ref<@Node>, %d: !ent.ref<@Node, mut, up @Under>)
      cascade @Under leaves first {
    %own = ent.get %n "total" : !ent.ref<@Node> -> i32
    ent.combine %d "total" add %own : !ent.ref<@Node, mut, up @Under>, i32
  }
}

// Reactive: where the entity's own events fired, or the ancestor's.
// CHECK-LABEL: ent.system @place() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node, mut>, %{{.*}}: !ent.ref<@Node, up @Under>, %{{.*}}: !ent.ref<@Mark, up @Under>) cascade @Under on [changed @Node "local", changed @Node "total" up @Under, changed @Mark up @Under log 4] {
ent.system @place() {
  ent.query (%n: !ent.ref<@Node, mut>, %p: !ent.ref<@Node, up @Under>,
             %m: !ent.ref<@Mark, up @Under>)
      cascade @Under
      on [changed @Node "local", changed @Node "total" up @Under,
          changed @Mark up @Under log 4] {
  }
}

// A tree whose children are in an order, the sibling before, and refs
// that may lead nowhere.
// CHECK: ent.relation @Row () from @Node to @Node tree ordered by @Mark "color" capacity 8
// CHECK-LABEL: ent.system @line() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node, mut>, %[[B:.*]]: !ent.ref<@Node, before @Row, optional>, %{{.*}}: !ent.ref<@Node, up @Row, optional>, %{{.*}}: !ent.ref<@Mark, before @Row>) cascade @Row {
// CHECK:     ent.bound %[[B]] : <@Node, before @Row, optional>
ent.relation @Row () from @Node to @Node tree ordered by @Mark "color" capacity 8
ent.system @line() {
  ent.query (%n: !ent.ref<@Node, mut>,
             %b: !ent.ref<@Node, before @Row, optional>,
             %p: !ent.ref<@Node, up @Row, optional>,
             %m: !ent.ref<@Mark, before @Row>) cascade @Row {
    %there = ent.bound %b : !ent.ref<@Node, before @Row, optional>
    %total = ent.get %b "total" : !ent.ref<@Node, before @Row, optional> -> i32
    ent.set %n "total", %total : !ent.ref<@Node, mut>, i32
  }
}

// Reactive to a child's events, from the leaves; and to the sibling
// before's.
// CHECK-LABEL: ent.system @sum_down() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node, mut>) cascade @Under leaves first on [changed @Node "total" down @Under] {
// CHECK-LABEL: ent.system @row() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node, mut>, %{{.*}}: !ent.ref<@Node, before @Row, optional>) cascade @Row on [changed @Node "total" before @Row] {
ent.system @sum_down() {
  ent.query (%n: !ent.ref<@Node, mut>) cascade @Under leaves first
      on [changed @Node "total" down @Under] {
  }
}
ent.system @row() {
  ent.query (%n: !ent.ref<@Node, mut>,
             %b: !ent.ref<@Node, before @Row, optional>) cascade @Row
      on [changed @Node "total" before @Row] {
  }
}

// Two steps up, the sibling after, and the entity a ref leads to.
// CHECK-LABEL: ent.system @reach() {
// CHECK:   ent.query (%{{.*}}: !ent.ref<@Node>, %[[G:.*]]: !ent.ref<@Node, parent @Row, hops 2>, %{{.*}}: !ent.ref<@Node, after @Row, optional>) {
// CHECK:     ent.other %[[G]] : <@Node, parent @Row, hops 2>
ent.system @reach() {
  ent.query (%n: !ent.ref<@Node>,
             %g: !ent.ref<@Node, parent @Row, hops 2>,
             %a: !ent.ref<@Node, after @Row, optional>) {
    %far = ent.other %g : !ent.ref<@Node, parent @Row, hops 2>
  }
}

// A tree whose entities are stored in its order.
// CHECK: ent.relation @In (w: f32) from @Cell to @Cell tree sorted capacity 8
ent.component @Cell (v: f32)
ent.relation @In (w: f32) from @Cell to @Cell tree sorted capacity 8
ent.archetype @Cells (@Cell) capacity 8
