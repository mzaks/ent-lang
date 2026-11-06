// The C of through.ent, shaped as the C Goose makes: a flag that says
// the program has started, checked by every function it exports (a
// panic where it has not), and data stacks reached through a global
// pointer, where what a function keeps for a while is put and taken back.
#include "ent_extern.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int64_t *top;
} stack;

int started;
stack *stacks;
_Thread_local stack *thread_stacks;

static void panic(const char *why) {
  fprintf(stderr, "panic: %s\n", why);
  abort();
}

static stack *new_stack(void) {
  stack *s = malloc(sizeof(stack));
  if (!s) abort();
  s->top = malloc(sizeof(int64_t) * 4096);
  if (!s->top) abort();
  return s;
}

__attribute__((constructor)) static void start(void) {
  stacks = new_stack();
  thread_stacks = new_stack();
  started = 1;
}

// splitmix64's finish: reads nothing but `started`, to leave where it is
// not.
uint64_t ent_mix(uint64_t x) {
  if (!started) panic("not started");
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// n pushed n times onto the data stack and added up (n * n), and the
// stack as it was.
int64_t ent_stacked(int32_t n) {
  if (!started) panic("not started");
  int64_t *base = stacks->top, *top = base;
  for (int32_t i = 0; i < n; i++) *top++ = n;
  stacks->top = top;
  int64_t sum = 0;
  for (int64_t *at = base; at < top; at++) sum += *at;
  stacks->top = base;
  return sum;
}

// The same on the thread's own stack.
int64_t ent_thread_stacked(int32_t n) {
  if (!started) panic("not started");
  int64_t *base = thread_stacks->top, *top = base;
  for (int32_t i = 0; i < n; i++) *top++ = n;
  thread_stacks->top = top;
  int64_t sum = 0;
  for (int64_t *at = base; at < top; at++) sum += *at;
  thread_stacks->top = base;
  return sum;
}

void ent_say(const ent_text62 *line) {
  printf("%.*s\n", line->length, line->bytes);
}
