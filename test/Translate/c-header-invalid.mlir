// RUN: ent-translate --ent-to-c-header %s -split-input-file -verify-diagnostics

// expected-error @+1 {{field "h" has type 'f16', which world storage does not support}}
ent.component @Half (h: f16)
ent.archetype @A (@Half) capacity 10

// -----

ent.component @P (x: f32)
ent.archetype @"a b" (@P) capacity 10
// expected-error @+1 {{C name 'a_b' is generated twice}}
ent.archetype @a_b (@P) capacity 10

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 10
// The archetype column A.P.x and the resource field A_P.x both map to the
// accessor ent_A_P_x.
// expected-error @+1 {{C name 'ent_A_P_x' is generated twice}}
ent.resource @A_P (x: f32)

