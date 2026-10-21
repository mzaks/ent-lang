// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// The ops of state and access that the lowering says what an apply or an
// accumulate sends, and what a connect or an append adds, with (ent-lower-to-loops makes and lowers them; none are left after it).
ent.component @Cell (got: i32)
ent.archetype @A (@Cell) capacity 8
ent.relation @ToldBy (day: i32) capacity 64
ent.buffer @Lines (n: i32) capacity 8
ent.resource @Score (points: i32)

// CHECK-LABEL: func.func @staged(
func.func @staged(%e: !ent.entity, %v: i32, %row: index, %mask: i1,
                  %tick: i64, %count: index, %world: memref<4096xi8>) {
  // CHECK: ent.sa.combine 2[1] @Cell "got" "add"(%{{.*}}, %{{.*}}) at %{{.*}} if %{{.*}} in %{{.*}} {resolution = "rows", unobserved} : !ent.entity, i32, memref<4096xi8>
  ent.sa.combine 2[1] @Cell "got" "add" (%e, %v) at %row if %mask
    in %world {resolution = "rows", unobserved}
    : !ent.entity, i32, memref<4096xi8>
  // CHECK: ent.sa.combine 3[1] @Cell "got" "max"(%{{.*}}, %{{.*}}) tick %{{.*}} in %{{.*}} {looped, resolution = "list", sliced} : !ent.entity, i32, memref<4096xi8>
  ent.sa.combine 3[1] @Cell "got" "max" (%e, %v) tick %tick in %world
    {looped, resolution = "list", sliced} : !ent.entity, i32, memref<4096xi8>
  // CHECK: ent.sa.merge 2[1] @Cell "got" "add" of i32 in %{{.*}} count %{{.*}} {least = 0 : i64, resolution = "rows", staged = "rows"} : memref<4096xi8>
  ent.sa.merge 2[1] @Cell "got" "add" of i32 in %world count %count
    {least = 0 : i64, resolution = "rows", staged = "rows"} : memref<4096xi8>
  // CHECK: ent.sa.merge 3[1] @Cell "got" "max" of i32 in %{{.*}} tick %{{.*}} {least = 16384 : i64, order_shows, parallel, resolution = "in-order", staged = "list"} : memref<4096xi8>
  ent.sa.merge 3[1] @Cell "got" "max" of i32 in %world tick %tick
    {least = 16384 : i64, order_shows, parallel, resolution = "in-order",
     staged = "list"} : memref<4096xi8>
  // CHECK: ent.sa.append "connect" 4[1] @ToldBy(%{{.*}}, %{{.*}}, %{{.*}}) in %{{.*}} {looped, resolution = "list", sliced} : (!ent.entity, !ent.entity, i32), memref<4096xi8>
  ent.sa.append "connect" 4[1] @ToldBy (%e, %e, %v) in %world
    {looped, resolution = "list", sliced}
    : (!ent.entity, !ent.entity, i32), memref<4096xi8>
  // CHECK: ent.sa.append "buffer" 0[1] @Lines(%{{.*}}) at %{{.*}} if %{{.*}} in %{{.*}} {resolution = "rows"} : (i32), memref<4096xi8>
  ent.sa.append "buffer" 0[1] @Lines (%v) at %row if %mask in %world
    {resolution = "rows"} : (i32), memref<4096xi8>
  // CHECK: ent.sa.place "connect" 4[1] @ToldBy in %{{.*}} {least = 1024 : i64, parallel, resolution = "reserved", staged = "list"} : memref<4096xi8>
  ent.sa.place "connect" 4[1] @ToldBy in %world
    {least = 1024 : i64, parallel, resolution = "reserved", staged = "list"}
    : memref<4096xi8>
  // CHECK: ent.sa.place "buffer" 0[1] @Lines in %{{.*}} count %{{.*}} {least = 0 : i64, resolution = "rows", staged = "rows"} : memref<4096xi8>
  ent.sa.place "buffer" 0[1] @Lines in %world count %count
    {least = 0 : i64, resolution = "rows", staged = "rows"} : memref<4096xi8>
  // CHECK: ent.sa.place "buffer" 0[1] @Lines in %{{.*}} count %{{.*}} from %{{.*}} {least = 0 : i64, ranks, resolution = "shared", staged = "rows"} : memref<4096xi8>
  ent.sa.place "buffer" 0[1] @Lines in %world count %count from %row
    {least = 0 : i64, ranks, resolution = "shared", staged = "rows"}
    : memref<4096xi8>
  // CHECK: ent.sa.accumulate 5[1] @Score "points" "add"(%{{.*}}) at %{{.*}} in %{{.*}} {reduced, resolution = "reduce", unobserved} : i32, memref<4096xi8>
  ent.sa.accumulate 5[1] @Score "points" "add" (%v) at %row in %world
    {reduced, resolution = "reduce", unobserved} : i32, memref<4096xi8>
  // CHECK: ent.sa.fold 5[1] @Score "points" "add" of i32 in %{{.*}} count %{{.*}} {resolution = "rows", staged = "rows"} : memref<4096xi8>
  ent.sa.fold 5[1] @Score "points" "add" of i32 in %world count %count
    {resolution = "rows", staged = "rows"} : memref<4096xi8>
  return
}
