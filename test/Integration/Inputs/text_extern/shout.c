// The C of text_extern.ent: texts given back.
#include "ent_extern.h"

#include <stdio.h>
#include <string.h>

const ent_text *ent_shout(const ent_text *t) {
  ent_text *loud = ent_text_new(t->length + 1);
  for (uint32_t i = 0; i < t->length; ++i)
    loud->bytes[i] = t->bytes[i] >= 'a' && t->bytes[i] <= 'z'
                         ? (char)(t->bytes[i] - 32)
                         : t->bytes[i];
  loud->bytes[t->length] = '!';
  return loud;
}

const ent_text *ent_greeting(int32_t n) {
  char words[64];
  int length = snprintf(words, sizeof words, "hello %d times", n);
  ent_text *made = ent_text_new((uint32_t)length);
  memcpy(made->bytes, words, (size_t)length);
  return made;
}

const ent_text *ent_nothing(const ent_text *t) {
  (void)t;
  return NULL;
}

// No fn where DRIFT is set: another text each call (--check-externs
// finds it).
#include <stdlib.h>
const ent_text *ent_drift(const ent_text *t) {
  static int calls;
  if (!getenv("DRIFT"))
    return t;
  ent_text *made = ent_text_new(t->length + 1);
  memcpy(made->bytes, t->bytes, t->length);
  made->bytes[t->length] = (char)('0' + ++calls % 10);
  return made;
}
