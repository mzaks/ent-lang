// Compiled to an object by the test and handed to the driver as that.
#include <stdint.h>
#include <stdio.h>

typedef struct {
  uint16_t length;
  char bytes[30];
} text30;

void ent_say(const text30 *line) {
  printf("%.*s\n", line->length, line->bytes);
  fflush(stdout);
}
