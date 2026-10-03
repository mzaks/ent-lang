// RUN: ent-opt %s -split-input-file -verify-diagnostics | FileCheck %s

// CHECK: ent.component @Label (name: !ent.text<30>)
ent.component @Label (name: !ent.text<30>)
// CHECK: ent.relation @Named (as: !ent.text<6>) capacity 4
ent.relation @Named (as: !ent.text<6>) capacity 4

// -----

// expected-error @+1 {{field 'name' is a text of capacity 0; a text holds 1 to 4094 bytes}}
ent.component @Label (name: !ent.text<0>)

// -----

// expected-error @+1 {{field 'name' is a text of capacity 5000; a text holds 1 to 4094 bytes}}
ent.resource @Label (name: !ent.text<5000>)
