// RUN: ent-opt %s --ent-lower-views | FileCheck %s

// A load and a store through a view of the world's bytes get the address
// they need: the bytes' own, the column's start on, the element on. The
// view goes where nothing else uses it.
// CHECK-LABEL: func.func @column(
// CHECK-SAME:      %[[WORLD:.*]]: memref<4096xi8>, %[[ROW:.*]]: index) -> f32 {
// CHECK:   %[[AT:.*]] = memref.extract_aligned_pointer_as_index %[[WORLD]]
// CHECK:   %[[BITS:.*]] = arith.index_cast %[[AT]] : index to i64
// CHECK:   %[[BASE:.*]] = llvm.inttoptr %[[BITS]] : i64 to !llvm.ptr
// CHECK:   %[[COLUMN:.*]] = llvm.getelementptr %[[BASE]][%{{.*}}] : (!llvm.ptr, i64) -> !llvm.ptr, i8
// CHECK-NOT: memref.view
// CHECK:   %[[R:.*]] = arith.index_cast %[[ROW]] : index to i64
// CHECK:   %[[PLACE:.*]] = llvm.getelementptr %[[COLUMN]][%[[R]]] : (!llvm.ptr, i64) -> !llvm.ptr, f32
// CHECK:   %[[X:.*]] = llvm.load %[[PLACE]] : !llvm.ptr -> f32
// CHECK:   llvm.store %{{.*}}, %{{.*}} : f32, !llvm.ptr
// CHECK:   return %[[X]]
func.func @column(%world: memref<4096xi8>, %row: index) -> f32 {
  %start = arith.constant 256 : index
  %column = memref.view %world[%start][] : memref<4096xi8> to memref<64xf32>
  %x = memref.load %column[%row] : memref<64xf32>
  %twice = arith.addf %x, %x : f32
  memref.store %twice, %column[%row] : memref<64xf32>
  return %x : f32
}

// A view that something else takes stays, with what reads through it
// changed all the same.
// CHECK-LABEL: func.func @kept(
// CHECK:   %[[VIEW:.*]] = memref.view
// CHECK:   llvm.load
// CHECK:   call @takes(%[[VIEW]])
func.func private @takes(memref<64xi32>)
func.func @kept(%world: memref<4096xi8>, %row: index) -> i32 {
  %start = arith.constant 512 : index
  %column = memref.view %world[%start][] : memref<4096xi8> to memref<64xi32>
  %x = memref.load %column[%row] : memref<64xi32>
  call @takes(%column) : (memref<64xi32>) -> ()
  return %x : i32
}
