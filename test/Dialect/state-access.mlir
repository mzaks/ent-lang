// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// The ops of state and access that the lowering says what an apply sends
// with (ent-lower-to-loops makes and lowers them; none are left after it).
ent.component @Cell (got: i32)

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
  return
}
