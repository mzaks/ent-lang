// RUN: ent-opt %s -split-input-file -verify-diagnostics | FileCheck %s
// `ent.index.get`: the entity of a key, of a hash index (its slots, rows
// and tags, as `index ... hash` declares them).

ent.component @Ship (id: i64) capacity 8
ent.archetype @Ships (@Ship) capacity 8
ent.buffer @ById (key: i64, head: i32, last: i32, count: i32, one: !ent.entity) capacity 16 {ent.index = {hash}}
ent.buffer @ById_list (who: !ent.entity, key: i64, place: i64, next: i32, prev: i32) capacity 8
ent.buffer @ById_tags (tag: ui8) capacity 32

// CHECK-LABEL: ent.system @look(
ent.system @look(%k: i64, %h: i64, %c: i64) {
  // CHECK: ent.index.get @ById[%{{.*}} : i64] : !ent.entity
  %e = ent.index.get @ById[%k : i64] : !ent.entity
  // (Of a text: its two hashes.)
  // CHECK: ent.index.get @ById[%{{.*}}, %{{.*}} : i64] : !ent.entity
  %f = ent.index.get @ById[%h, %c : i64] : !ent.entity
}

// -----

ent.component @Ship (id: i64) capacity 8
ent.archetype @Ships (@Ship) capacity 8
ent.buffer @Seen (key: i64) capacity 16
ent.system @look(%k: i64) {
  // expected-error @+1 {{asks @Seen, not a hash index}}
  %e = ent.index.get @Seen[%k : i64] : !ent.entity
}

// -----

ent.component @Ship (id: i64) capacity 8
ent.archetype @Ships (@Ship) capacity 8
ent.buffer @ById (key: i64, head: i32, last: i32, count: i32, one: !ent.entity) capacity 16 {ent.index = {hash}}
ent.buffer @ById_list (who: !ent.entity, key: i64, place: i64, next: i32, prev: i32) capacity 8
ent.buffer @ById_tags (tag: ui8) capacity 32
ent.system @look(%k: i64) gpu {
  // expected-error @+1 {{is in a 'gpu' system: a hash index is asked on the CPU}}
  %e = ent.index.get @ById[%k : i64] : !ent.entity
}
