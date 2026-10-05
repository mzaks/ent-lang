// RUN: ent-translate --ent-to-c-header %s -split-input-file -verify-diagnostics \
// RUN:   | FileCheck %s

ent.component @Position (x: f32)
ent.archetype @Rock (@Position) capacity 8

// The header declares what the program calls; the entry point adds nothing.
// CHECK:      // Extern systems.
// CHECK:      void ent_draw(ent_world *world, float arg0, bool arg1, ent_entity arg2);
// CHECK-NEXT: void ent_beep(ent_world *world);
// CHECK-EMPTY:
// CHECK-NEXT: #undef ENT__STATIC_ASSERT
// CHECK-NEXT: #ifdef __cplusplus
// CHECK-NEXT: } // extern "C"
// CHECK-NEXT: #endif
ent.extern @draw(f32, i1, !ent.entity) reads [@Position]
ent.extern @beep()
ent.schedule @frame() {
  ent.run @beep()
}
ent.main {
  ent.call @frame()
}

// -----

ent.component @Position (x: f32)
ent.archetype @Rock (@Position) capacity 8
// expected-error @+1 {{C name 'ent_Rock_Position_x' is generated twice}}
ent.extern @Rock_Position_x()

// -----

ent.component @Position (x: f32)
ent.archetype @Rock (@Position) capacity 8
// expected-error @+1 {{parameter #0 has type 'f16', which has no C equivalent here}}
ent.extern @half(f16)
