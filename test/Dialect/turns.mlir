// RUN: ent-opt %s | FileCheck %s
// Printing and re-parsing must give the same module.
// RUN: ent-opt %s | ent-opt | FileCheck %s

ent.component @Thing (born: i32, hp: i32) capacity 8
ent.component @Fast (speed: i32)
ent.resource @Stream (value: i32)
ent.buffer @Seen (born: i32) capacity 16
ent.archetype @Things (@Thing) capacity 8

// CHECK-LABEL: ent.system @order()
ent.system @order() {
  // One query, its entities by a field: what it sends it does not read.
  // CHECK: ent.query (%{{.*}}: !ent.ref<@Thing>) by @Thing "born" {
  ent.query (%t: !ent.ref<@Thing>) by @Thing "born" {
    %born = ent.get %t "born" : !ent.ref<@Thing> -> i32
    ent.append @Seen (%born) : i32
    ent.accumulate @Stream "value" add %born : i32
  }
}

// CHECK-LABEL: ent.system @turns()
ent.system @turns() {
  // A query for each: it writes a resource and a buffer's row, reads what
  // it sends to, and sets another entity's field.
  // CHECK: ent.query (%{{.*}}: !ent.ref<@Thing, mut>) with [@Fast] by @Thing "born" turn {
  ent.query (%t: !ent.ref<@Thing, mut>) with [@Fast] by @Thing "born" turn {
    %me = ent.entity
    %born = ent.get %t "born" : !ent.ref<@Thing, mut> -> i32
    %old = ent.read @Stream "value" : i32
    %new = arith.addi %old, %born : i32
    ent.write @Stream "value", %new : i32
    %hp, %found = ent.lookup %me @Thing "hp" : i32
    ent.apply %me @Thing "hp" set %new : i32
    ent.set %t "hp", %hp : !ent.ref<@Thing, mut>, i32
    %zero = arith.constant 0 : i32
    ent.buffer.set @Seen "born"[%zero : i32] = %new : i32
  }
}

// CHECK-LABEL: ent.system @equals(
ent.system @equals(%n: i32, %h: i64, %c: i64) {
  // A `where` that compares a field with a value the same for all: the
  // value (of a text, its hashes too), which a hash index may answer.
  // CHECK: ent.query (%{{.*}}: !ent.ref<@Thing>) equals %{{.*}} : i32 attributes {key_equals = [@Thing, "born"]} {
  ent.query (%t: !ent.ref<@Thing>) equals %n : i32 attributes {key_equals = [@Thing, "born"]} {
  }
  // CHECK: ent.query (%{{.*}}: !ent.ref<@Thing>) equals %{{.*}} : i32 equals_hash %{{.*}} : i64 equals_check %{{.*}} : i64 attributes {key_equals = [@Thing, "hp"]} {
  ent.query (%t: !ent.ref<@Thing>) equals %n : i32 equals_hash %h : i64 equals_check %c : i64 attributes {key_equals = [@Thing, "hp"]} {
  }
}
