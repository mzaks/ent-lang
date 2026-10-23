// RUN: ent-opt %s -split-input-file -verify-diagnostics

ent.component @Thing (born: i32, hp: i32)
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  // expected-error @+1 {{is 'turn' without a 'by': its entities take their turns in the order of a field}}
  ent.query (%t: !ent.ref<@Thing>) attributes {turn} {
  }
}

// -----

ent.component @Thing (born: i32, speed: i1)
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  // expected-error @+1 {{goes by 'speed' of type 'i1'; the order is that of a whole number or a fraction}}
  ent.query (%t: !ent.ref<@Thing>) by @Thing "speed" {
  }
}

// -----

ent.component @Thing (born: i32)
ent.component @Other (n: i32)
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  // expected-error @+1 {{goes by a field of @Other, which its entities need not have: bind it or name it in 'with'}}
  ent.query (%t: !ent.ref<@Thing>) by @Other "n" {
  }
}

// -----

// One query still: it does not read what it sends, which lands as it
// goes.
ent.component @Thing (born: i32)
ent.resource @Stream (value: i32)
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  ent.query (%t: !ent.ref<@Thing>) by @Thing "born" {
    %born = ent.get %t "born" : !ent.ref<@Thing> -> i32
    %old = ent.read @Stream "value" : i32
    // expected-note @-1 {{read or set here}}
    // expected-error @+1 {{in a query that goes by a field combines into what the query also reads or sets; it is combined as the query goes ('turn' makes that what is meant)}}
    ent.accumulate @Stream "value" add %born : i32
  }
}

// -----

ent.component @Thing (born: i32)
ent.resource @Stream (value: i32)
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  ent.query (%t: !ent.ref<@Thing>) by @Thing "born" {
    %born = ent.get %t "born" : !ent.ref<@Thing> -> i32
    // expected-error @+1 {{cannot write a resource inside 'ent.query'}}
    ent.write @Stream "value", %born : i32
  }
}

// -----

ent.component @Thing (born: i32)
ent.buffer @Seen (born: i32) capacity 4
ent.archetype @Things (@Thing) capacity 4
ent.system @s() {
  ent.query (%t: !ent.ref<@Thing>) by @Thing "born" {
    %born = ent.get %t "born" : !ent.ref<@Thing> -> i32
    ent.append @Seen (%born) : i32
    // expected-error @+1 {{reads a buffer that the query, which goes by a field, appends to}}
    %n = ent.buffer.len @Seen
  }
}
