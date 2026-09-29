// RUN: ecs-translate --ecs-to-c-header %s -split-input-file -verify-diagnostics

// expected-error @+1 {{field "h" has type 'f16', which world storage does not support}}
ecs.component @Half (h: f16)
ecs.archetype @A (@Half) capacity 10

// -----

ecs.component @P (x: f32)
ecs.archetype @"a b" (@P) capacity 10
// expected-error @+1 {{C name 'a_b' is generated twice}}
ecs.archetype @a_b (@P) capacity 10
