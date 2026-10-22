// A function marked ent.no_gather (one with a `near` by blocks of rows)
// has the machine's features, as tools/ent gives them, and prefers no
// gathers; the others keep what the command line gives. Without the
// features (not on x86) none is changed.
// RUN: ent-opt %s --ent-emit-llvm="output=%t.bc features=+avx2,+fma" -o /dev/null
// RUN: llvm-dis %t.bc -o - | FileCheck %s
// RUN: ent-opt %s --ent-emit-llvm="output=%t.none.bc" -o /dev/null
// RUN: llvm-dis %t.none.bc -o - | FileCheck %s --check-prefix=NONE

// CHECK: define void @fill() #[[FILL:[0-9]+]]
// CHECK: define void @count() {
// CHECK: attributes #[[FILL]] = { "target-features"="+avx2,+fma,+prefer-no-gather" }

// NONE-NOT: target-features

llvm.func @fill() attributes {ent.no_gather} {
  llvm.return
}

llvm.func @count() {
  llvm.return
}
