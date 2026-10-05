// Extern fns and procs in C: in a header of their own, which is all the C
// that defines them needs, and in the world's header.
// RUN: ent-translate --ent-to-c-extern-header %s | FileCheck %s
// RUN: ent-translate --ent-to-c-extern-header %s \
// RUN:   | FileCheck %s --check-prefix=NOWORLD
// RUN: ent-translate --ent-to-c-header %s | FileCheck %s --check-prefix=WORLD
// Both together compile.
// RUN: ent-translate --ent-to-c-extern-header %s -o %t.extern.h
// RUN: ent-translate --ent-to-c-header %s -o %t.world.h
// RUN: clang -fsyntax-only -Werror -x c -include %t.extern.h \
// RUN:     -include %t.world.h /dev/null
// As C++ too: they are `extern "C"`.
// RUN: clang -fsyntax-only -Werror -x c++ -include %t.extern.h \
// RUN:     -include %t.world.h /dev/null
// CHECK:      #ifdef __cplusplus
// CHECK-NEXT: extern "C" {

ent.component @Name (text: !ent.text<14>)
ent.archetype @Named (@Name) capacity 4

// The world is not there: no type for it, no way to it.
// NOWORLD-NOT: ent_world
// NOWORLD-NOT: ent_Named

// Only the texts the functions take.
// CHECK:      #ifndef ENT_TEXT30
// CHECK-NEXT: #define ENT_TEXT30
// CHECK-NEXT: typedef struct {
// CHECK-NEXT:   uint16_t length;
// CHECK-NEXT:   char bytes[30];
// CHECK-NEXT: } ent_text30;
// CHECK:      #endif
// CHECK-NOT:  ent_text14

// CHECK:      // Extern fns and procs.
// CHECK:      int64_t ent_hash(int64_t arg0);
// CHECK-NEXT: void ent_put(const ent_text30 *arg0, bool arg1);
// CHECK-NEXT: double ent_m_seconds(void);
// CHECK-NEXT: bool ent_is_odd(int64_t arg0);
// CHECK-EMPTY:
// CHECK-NEXT: #undef ENT__STATIC_ASSERT
// CHECK-NEXT: #ifdef __cplusplus
// CHECK-NEXT: } // extern "C"
// CHECK-NEXT: #endif
// WORLD:      ent_text14;
// WORLD:      ent_text30;
// WORLD:      int64_t ent_hash(int64_t arg0);
// WORLD-NEXT: void ent_put(const ent_text30 *arg0, bool arg1);
// WORLD-NEXT: double ent_m_seconds(void);
// WORLD-NEXT: bool ent_is_odd(int64_t arg0);
ent.function @hash(i64) -> i64
ent.function proc @put(!ent.text<30>, i1)
ent.function proc @m.seconds() -> f64
ent.function @is_odd(i64) -> i1
