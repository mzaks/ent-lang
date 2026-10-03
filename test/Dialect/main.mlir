// Extern systems and the entry point print and parse again.
// RUN: ent-opt %s | FileCheck %s
// RUN: ent-opt %s | ent-opt | FileCheck %s

ent.component @Position (x: f32)
ent.resource @Clock (frame: i32, dt: f32)
ent.resource @Exit (value: i1)
ent.archetype @Rock (@Position) capacity 8

// CHECK: ent.extern @draw(f32, i1) reads [@Position] writes [@Clock]
ent.extern @draw(f32, i1) reads [@Position] writes [@Clock]
// CHECK: ent.extern @load() writes [@Rock]
ent.extern @load() writes [@Rock]
// CHECK: ent.extern @beep()
ent.extern @beep()

ent.schedule @setup() {
  ent.run @load()
}
// CHECK-LABEL: ent.schedule @frame(%{{.*}}: f32) {
// CHECK:   ent.run @draw(%{{.*}}, %{{.*}}) : f32, i1
// CHECK:   ent.run @beep()
ent.schedule @frame(%dt: f32) {
  %on = arith.constant true
  ent.run @draw(%dt, %on) : f32, i1
  ent.run @beep()
}

// CHECK-LABEL: ent.main {
// CHECK-NEXT:   ent.call @setup()
// CHECK-NEXT:   ent.loop {
// CHECK-NEXT:     %[[DT:.*]] = ent.read @Clock "dt" : f32
// CHECK-NEXT:     ent.call @frame(%[[DT]]) : f32
// CHECK-NEXT:     ent.loop {
// CHECK-NEXT:       ent.call @setup()
// CHECK-NEXT:     }
// CHECK-NEXT:     %[[DONE:.*]] = ent.read @Exit "value" : i1
// CHECK-NEXT:     ent.yield %[[DONE]] : i1
// CHECK-NEXT:   }
// CHECK-NEXT: }
ent.main {
  ent.call @setup()
  ent.loop {
    %dt = ent.read @Clock "dt" : f32
    ent.call @frame(%dt) : f32
    ent.loop {
      ent.call @setup()
    }
    %done = ent.read @Exit "value" : i1
    ent.yield %done : i1
  }
}
