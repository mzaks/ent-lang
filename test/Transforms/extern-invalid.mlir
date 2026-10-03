// RUN: ent-opt %s -split-input-file --ent-lower-to-loops -verify-diagnostics -o /dev/null

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 1
// expected-note @+1 {{declared here}}
ent.system @ent_draw() {
}
// expected-error @+1 {{is called as the C function 'ent_draw', a name this program also declares}}
ent.extern @draw()

// -----

ent.component @P (x: f32)
ent.archetype @A (@P) capacity 1
// expected-note @+1 {{declared here}}
ent.schedule @main() {
}
// expected-error @+1 {{lowers to the function 'main', a name this program also declares}}
ent.main {
  ent.call @main()
}
