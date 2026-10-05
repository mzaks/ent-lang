// RUN: ent-translate --ent-to-c-header %s | FileCheck %s

// One struct per capacity, as large as the program stores it: the length,
// the bytes, and padding up to a multiple of 16.
// Each is guarded: the header of the extern fns and procs has them too.
// CHECK:      #ifndef ENT_TEXT14
// CHECK-NEXT: #define ENT_TEXT14
// CHECK-NEXT: typedef struct {
// CHECK-NEXT:   uint16_t length;
// CHECK-NEXT:   char bytes[14];
// CHECK-NEXT: } ent_text14;
// CHECK-NEXT: ENT__STATIC_ASSERT(sizeof(ent_text14) == 16, "text layout");
// CHECK-NEXT: #endif
// CHECK-NEXT: #ifndef ENT_TEXT20
// CHECK-NEXT: #define ENT_TEXT20
// CHECK-NEXT: typedef struct {
// CHECK-NEXT:   uint16_t length;
// CHECK-NEXT:   char bytes[20];
// CHECK-NEXT:   char ent__padding[10];
// CHECK-NEXT: } ent_text20;
// CHECK-NEXT: ENT__STATIC_ASSERT(sizeof(ent_text20) == 32, "text layout");
// CHECK: static inline ent_text14 *ent_Ship_Label_name(ent_world *world) {
// CHECK: static inline ent_text20 *ent_Title_value(ent_world *world) {
ent.component @Label (name: !ent.text<14>, n: i32)
ent.resource @Title (value: !ent.text<20>)
ent.resource @Other (value: !ent.text<14>)
ent.archetype @Ship (@Label) capacity 4
