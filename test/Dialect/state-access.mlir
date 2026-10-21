// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

// The ops of state and access that the lowering stages a loop's applies
// with (ent-lower-to-loops makes and lowers them; none are left after it).
ent.component @Cell (got: i32)

// CHECK-LABEL: func.func @staged(
func.func @staged(%slice: i64, %id: i64, %v: i32, %tick: i64,
                  %world: memref<4096xi8>) {
  // CHECK: ent.sa.combine 3[%{{.*}}] @Cell "got" "add"(%{{.*}}, %{{.*}}) in %{{.*}} : i64, i32, memref<4096xi8>
  ent.sa.combine 3[%slice] @Cell "got" "add" (%id, %v) in %world
    : i64, i32, memref<4096xi8>
  // CHECK: ent.sa.merge 3 @Cell "got" "add" of i32 in %{{.*}} at %{{.*}} {least = 16384 : i64, parallel, resolution = "groups"} : memref<4096xi8>
  ent.sa.merge 3 @Cell "got" "add" of i32 in %world at %tick
    {least = 16384 : i64, parallel, resolution = "groups"} : memref<4096xi8>
  // CHECK: ent.sa.merge 4 @Cell "got" "max" of i32 in %{{.*}} {least = 16384 : i64, order_shows} : memref<4096xi8>
  ent.sa.merge 4 @Cell "got" "max" of i32 in %world
    {least = 16384 : i64, order_shows} : memref<4096xi8>
  return
}
