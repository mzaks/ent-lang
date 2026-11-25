// Regular expressions as JavaScript writes them (devices/regex.ent): a
// pattern read into a tree, the tree into a program, the program run over
// the text as Pike's machine runs one: every way the pattern may go is
// followed at once, a character at a time, so a match takes time in
// proportion to the text times the program, never more (no pattern makes
// it go back and try again, as a backtracking one may for ever). Among
// the ways that match, the one JavaScript would take is the one given:
// the leftmost start, then alternatives and greedy or lazy repeats in
// their order.
//
// What is read: characters (UTF-8, a code point each), `.`, classes
// `[a-z]` `[^...]` with `\d \w \s` and their opposites in them or out,
// `^ $` (of lines with the flag m), `\b \B`, groups `( )` `(?: )`
// `(?<name> )`, `|`, repeats `* + ? {n} {n,} {n,m}` and lazy ones with a
// `?` after them, escapes `\n \r \t \f \v \0 \xHH \uHHHH` and `\` before a
// sign. Flags: i (A to Z as a to z), m, s (`.` takes \n too); g, u, y and
// d are taken and change nothing. Not read: back references (\1) and
// looking ahead or behind ((?= (?! (?<= (?<!): those need going back.
//
// Written for ent-lang. The techniques are published ones: the pattern
// made into a program of steps as in Ken Thompson's "Regular Expression
// Search Algorithm" (Communications of the ACM, 1968), and the program run
// as Rob Pike's machine runs one, as Russ Cox describes it in "Regular
// Expression Matching: the Virtual Machine Approach" (2009,
// swtch.com/~rsc/regexp/regexp2.html). The syntax and the order in which
// matches are preferred are JavaScript's (ECMAScript's RegExp).
#include "ent_extern.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#define ENT_REGEX_SAME_RESULT __attribute__((annotate("ent.same_result")))
#else
#define ENT_REGEX_SAME_RESULT
#endif

// The tree.
enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_BOL, N_EOL, N_WORD, N_NOT_WORD, N_GROUP };
typedef struct {
  int kind, a, b;     // children (CAT, ALT; REPEAT's a)
  int32_t value;      // a CHAR's code point, a CLASS's number, a GROUP's
  int min, max;       // a REPEAT's (max -1: no end)
  bool greedy;
} rx_node;
// A class: ranges of code points, from `first` on, `count` of them.
typedef struct { int first, count; bool negated; } rx_class;

// The program.
// (SAVE keeps where the text is as a group's start or end: its value
// 2 * group, or 2 * group + 1.)
enum { I_CHAR, I_ANY, I_ANY_NL, I_CLASS, I_SPLIT, I_JMP, I_MATCH, I_BOL, I_EOL, I_WORD, I_NOT_WORD, I_SAVE };
typedef struct { int op, x, y; int32_t value; } rx_inst;

enum { RX_MAX_NODES = 20000, RX_MAX_PROGRAM = 50000, RX_MAX_REPEAT = 1000 };

typedef struct {
  const unsigned char *p;
  size_t n, at;
  const char *error;
  rx_node *nodes;
  int node_count, node_room;
  rx_class *classes;
  int class_count, class_room;
  int32_t *ranges;   // pairs: low, high
  int range_count, range_room;
  rx_inst *program;
  int program_count, program_room;
  bool ignore_case, multiline, dot_all;
  bool bare_assertion; // (The atom read last is ^ $ \b or \B, not in a group.)
  int group_count;     // groups that keep what they match: ( ) and (?<name> )
} rx;

static bool rx_grow(void **items, int *room, int need, size_t size) {
  if (need <= *room)
    return true;
  int more = *room ? *room * 2 : 64;
  while (more < need)
    more *= 2;
  void *bigger = realloc(*items, (size_t)more * size);
  if (!bigger)
    return false;
  *items = bigger;
  *room = more;
  return true;
}

static int rx_node_new(rx *r, int kind) {
  if (r->node_count >= RX_MAX_NODES) {
    r->error = "the pattern is too long";
    return 0;
  }
  if (!rx_grow((void **)&r->nodes, &r->node_room, r->node_count + 1, sizeof(rx_node))) {
    r->error = "out of memory";
    return 0;
  }
  rx_node *made = &r->nodes[r->node_count];
  memset(made, 0, sizeof *made);
  made->kind = kind;
  return r->node_count++;
}

static void rx_range(rx *r, int32_t low, int32_t high) {
  if (!rx_grow((void **)&r->ranges, &r->range_room, (r->range_count + 1) * 2,
               sizeof(int32_t))) {
    r->error = "out of memory";
    return;
  }
  r->ranges[r->range_count * 2] = low;
  r->ranges[r->range_count * 2 + 1] = high;
  r->range_count++;
}

// A code point of UTF-8 at `at` (a byte that starts none: as it is), and
// where the next starts.
static int32_t rx_decode(const unsigned char *p, size_t n, size_t at, size_t *next) {
  unsigned char c = p[at];
  int more = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (c < 0x80 || more == 0 || at + (size_t)more >= n) {
    *next = at + 1;
    return c;
  }
  int32_t code = c & (0x3F >> more);
  for (int k = 1; k <= more; ++k) {
    if (at + (size_t)k >= n || (p[at + k] & 0xC0) != 0x80) {
      *next = at + 1;
      return c;
    }
    code = (code << 6) | (p[at + k] & 0x3F);
  }
  *next = at + (size_t)more + 1;
  return code;
}

static bool rx_more(rx *r) { return r->at < r->n; }
static int rx_peek(rx *r) { return r->at < r->n ? r->p[r->at] : -1; }

static int rx_hex(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
// \xHH or \uHHHH after the letter: the code point (-1: not there).
static int32_t rx_hex_digits(rx *r, int count) {
  if (r->at + (size_t)count > r->n)
    return -1;
  int32_t v = 0;
  for (int k = 0; k < count; ++k) {
    int d = rx_hex(r->p[r->at + k]);
    if (d < 0)
      return -1;
    v = v * 16 + d;
  }
  r->at += (size_t)count;
  return v;
}

// The ranges of \d \w \s (and \D \W \S as negated classes).
static void rx_shorthand_ranges(rx *r, int letter) {
  switch (letter) {
  case 'd':
    rx_range(r, '0', '9');
    break;
  case 'w':
    rx_range(r, '0', '9');
    rx_range(r, 'A', 'Z');
    rx_range(r, '_', '_');
    rx_range(r, 'a', 'z');
    break;
  case 's':
    rx_range(r, '\t', '\r');
    rx_range(r, ' ', ' ');
    rx_range(r, 0xA0, 0xA0);
    rx_range(r, 0x1680, 0x1680);
    rx_range(r, 0x2000, 0x200A);
    rx_range(r, 0x2028, 0x2029);
    rx_range(r, 0x202F, 0x202F);
    rx_range(r, 0x205F, 0x205F);
    rx_range(r, 0x3000, 0x3000);
    rx_range(r, 0xFEFF, 0xFEFF);
    break;
  }
}
static int rx_class_new(rx *r, bool negated) {
  if (!rx_grow((void **)&r->classes, &r->class_room, r->class_count + 1, sizeof(rx_class))) {
    r->error = "out of memory";
    return 0;
  }
  r->classes[r->class_count].first = r->range_count;
  r->classes[r->class_count].count = 0;
  r->classes[r->class_count].negated = negated;
  return r->class_count++;
}
static void rx_class_done(rx *r, int k) {
  r->classes[k].count = r->range_count - r->classes[k].first;
}

// An escape's code point after the `\` (the letter read): for one that
// stands for a character; -2 where it is a class (\d ...), which the
// caller makes; -3 for \b \B outside a class.
static int32_t rx_escape(rx *r, bool in_class) {
  if (!rx_more(r)) {
    r->error = "\\ at the end of the pattern";
    return 0;
  }
  size_t next;
  int32_t c = rx_decode(r->p, r->n, r->at, &next);
  r->at = next;
  switch (c) {
  case 'n': return '\n';
  case 'r': return '\r';
  case 't': return '\t';
  case 'f': return '\f';
  case 'v': return '\v';
  case '0': return 0;
  case 'd': case 'D': case 'w': case 'W': case 's': case 'S': return -2;
  case 'b': if (in_class) return '\b'; return -3;
  case 'B': if (in_class) return 'B'; return -3;
  case 'x': {
    int32_t v = rx_hex_digits(r, 2);
    return v >= 0 ? v : 'x';
  }
  case 'u': {
    int32_t v = rx_hex_digits(r, 4);
    return v >= 0 ? v : 'u';
  }
  case 'c':
    if (rx_more(r) && ((rx_peek(r) | 0x20) >= 'a' && (rx_peek(r) | 0x20) <= 'z'))
      return r->p[r->at++] % 32;
    return 'c';
  default:
    if (c >= '1' && c <= '9') {
      r->error = "back references (\\1) are not supported";
      return 0;
    }
    return c;
  }
}

static int rx_alternatives(rx *r, int depth);

// A class: after its `[`.
static int rx_bracket(rx *r) {
  bool negated = rx_peek(r) == '^';
  if (negated)
    r->at++;
  int k = rx_class_new(r, negated);
  bool first = true;
  while (rx_more(r) && !(rx_peek(r) == ']' && !first) && !r->error) {
    first = false;
    if (rx_peek(r) == ']') { // ([] matches nothing; [^] anything.)
      break;
    }
    int32_t low;
    size_t next;
    if (rx_peek(r) == '\\') {
      r->at++;
      size_t letter_at = r->at;
      low = rx_escape(r, true);
      if (low == -2) {
        int letter = r->p[letter_at];
        if (letter >= 'a') {
          rx_shorthand_ranges(r, letter);
        } else {
          // (\D \W \S in a class: what lies between the ranges.)
          int from = r->range_count;
          rx_shorthand_ranges(r, letter | 0x20);
          int to = r->range_count;
          int32_t start = 0;
          int32_t gaps[32];
          int gap_count = 0;
          for (int i = from; i < to; ++i) {
            if (r->ranges[i * 2] > start && gap_count < 30) {
              gaps[gap_count++] = start;
              gaps[gap_count++] = r->ranges[i * 2] - 1;
            }
            start = r->ranges[i * 2 + 1] + 1;
          }
          r->range_count = from;
          for (int i = 0; i < gap_count; i += 2)
            rx_range(r, gaps[i], gaps[i + 1]);
          rx_range(r, start, 0x10FFFF);
        }
        continue;
      }
    } else {
      low = rx_decode(r->p, r->n, r->at, &next);
      r->at = next;
    }
    int32_t high = low;
    if (rx_peek(r) == '-' && r->at + 1 < r->n && r->p[r->at + 1] != ']') {
      r->at++;
      if (rx_peek(r) == '\\') {
        r->at++;
        high = rx_escape(r, true);
        if (high == -2) { // ([a-\d]: a, -, and the digits.)
          rx_range(r, low, low);
          rx_range(r, '-', '-');
          rx_shorthand_ranges(r, r->p[r->at - 1]);
          continue;
        }
      } else {
        high = rx_decode(r->p, r->n, r->at, &next);
        r->at = next;
      }
      if (high < low) {
        r->error = "a range of a class goes backwards";
        return 0;
      }
    }
    rx_range(r, low, high);
  }
  if (!rx_more(r) || rx_peek(r) != ']') {
    if (!r->error)
      r->error = "a [ is not closed";
    return 0;
  }
  r->at++;
  rx_class_done(r, k);
  int node = rx_node_new(r, N_CLASS);
  if (!r->error)
    r->nodes[node].value = k;
  return node;
}

// One thing that may be repeated.
static int rx_atom(rx *r, int depth) {
  int c = rx_peek(r);
  r->bare_assertion = c == '^' || c == '$' ||
                      (c == '\\' && r->at + 1 < r->n && (r->p[r->at + 1] == 'b' || r->p[r->at + 1] == 'B'));
  if (c == '(') {
    r->at++;
    bool keeps = true;
    if (rx_peek(r) == '?') {
      r->at++;
      int kind = rx_peek(r);
      if (kind == ':') {
        r->at++;
        keeps = false;
      } else if (kind == '<' && r->at + 1 < r->n && r->p[r->at + 1] != '=' &&
                 r->p[r->at + 1] != '!') {
        while (rx_more(r) && rx_peek(r) != '>')
          r->at++;
        if (!rx_more(r)) {
          r->error = "a group's name is not closed";
          return 0;
        }
        r->at++;
      } else if (kind == '=' || kind == '!' || kind == '<') {
        r->error = "looking ahead or behind ((?= (?! (?<= (?<!) is not supported";
        return 0;
      } else {
        r->error = "(? is followed by what no group is";
        return 0;
      }
    }
    if (depth > 500) {
      r->error = "groups are nested too deep";
      return 0;
    }
    // (Numbered as their "(" come, from 1.)
    int group = keeps ? ++r->group_count : 0;
    int inner = rx_alternatives(r, depth + 1);
    if (r->error)
      return 0;
    if (rx_peek(r) != ')') {
      r->error = "a ( is not closed";
      return 0;
    }
    r->at++;
    r->bare_assertion = false;
    if (!keeps)
      return inner;
    int node = rx_node_new(r, N_GROUP);
    if (r->error)
      return 0;
    r->nodes[node].a = inner;
    r->nodes[node].value = group;
    return node;
  }
  if (c == '[') {
    r->at++;
    return rx_bracket(r);
  }
  if (c == '.') {
    r->at++;
    return rx_node_new(r, N_ANY);
  }
  if (c == '^') {
    r->at++;
    return rx_node_new(r, N_BOL);
  }
  if (c == '$') {
    r->at++;
    return rx_node_new(r, N_EOL);
  }
  if (c == '*' || c == '+' || c == '?') {
    r->error = "a repeat (* + ?) follows nothing";
    return 0;
  }
  if (c == '\\') {
    r->at++;
    size_t letter_at = r->at;
    int32_t v = rx_escape(r, false);
    if (r->error)
      return 0;
    int letter = r->p[letter_at];
    if (v == -3)
      return rx_node_new(r, letter == 'b' ? N_WORD : N_NOT_WORD);
    if (v == -2) {
      int k = rx_class_new(r, letter < 'a');
      rx_shorthand_ranges(r, letter | 0x20);
      rx_class_done(r, k);
      int node = rx_node_new(r, N_CLASS);
      if (!r->error)
        r->nodes[node].value = k;
      return node;
    }
    int node = rx_node_new(r, N_CHAR);
    if (!r->error)
      r->nodes[node].value = v;
    return node;
  }
  size_t next;
  int32_t code = rx_decode(r->p, r->n, r->at, &next);
  r->at = next;
  int node = rx_node_new(r, N_CHAR);
  if (!r->error)
    r->nodes[node].value = code;
  return node;
}

// {n}, {n,}, {n,m} at `at`: true where it is one (read), its numbers set.
static bool rx_braces(rx *r, int *min, int *max) {
  size_t i = r->at + 1;
  long a = 0, b = -1;
  size_t digits = 0;
  while (i < r->n && r->p[i] >= '0' && r->p[i] <= '9') {
    if (a < 100000)
      a = a * 10 + (r->p[i] - '0');
    i++;
    digits++;
  }
  if (!digits)
    return false;
  if (i < r->n && r->p[i] == ',') {
    i++;
    size_t more = 0;
    long v = 0;
    while (i < r->n && r->p[i] >= '0' && r->p[i] <= '9') {
      if (v < 100000)
        v = v * 10 + (r->p[i] - '0');
      i++;
      more++;
    }
    b = more ? v : -1;
  } else {
    b = a;
  }
  if (i >= r->n || r->p[i] != '}')
    return false;
  r->at = i + 1;
  *min = (int)a;
  *max = (int)b;
  return true;
}

static int rx_repeated(rx *r, int depth) {
  int atom = rx_atom(r, depth);
  bool repeated = false;
  while (!r->error && rx_more(r)) {
    int c = rx_peek(r);
    int min, max;
    if (c == '*') {
      min = 0, max = -1;
      r->at++;
    } else if (c == '+') {
      min = 1, max = -1;
      r->at++;
    } else if (c == '?') {
      min = 0, max = 1;
      r->at++;
    } else if (c == '{' && rx_braces(r, &min, &max)) {
      if (max >= 0 && max < min) {
        r->error = "a repeat's {n,m} has m below n";
        return 0;
      }
      if (min > RX_MAX_REPEAT || max > RX_MAX_REPEAT) {
        r->error = "a repeat's number is above 1000";
        return 0;
      }
    } else {
      break;
    }
    if (r->bare_assertion) {
      r->error = "a repeat follows ^ $ \\b or \\B";
      return 0;
    }
    if (repeated) {
      r->error = "a repeat follows a repeat (a** or a+*)";
      return 0;
    }
    repeated = true;
    bool greedy = true;
    if (rx_peek(r) == '?') {
      greedy = false;
      r->at++;
    }
    int node = rx_node_new(r, N_REPEAT);
    if (r->error)
      return 0;
    r->nodes[node].a = atom;
    r->nodes[node].min = min;
    r->nodes[node].max = max;
    r->nodes[node].greedy = greedy;
    atom = node;
  }
  return atom;
}

static int rx_sequence(rx *r, int depth) {
  int seq = -1;
  while (!r->error && rx_more(r) && rx_peek(r) != '|' && rx_peek(r) != ')') {
    int item = rx_repeated(r, depth);
    if (r->error)
      return 0;
    if (seq < 0) {
      seq = item;
    } else {
      int cat = rx_node_new(r, N_CAT);
      if (r->error)
        return 0;
      r->nodes[cat].a = seq;
      r->nodes[cat].b = item;
      seq = cat;
    }
  }
  return seq < 0 ? rx_node_new(r, N_EMPTY) : seq;
}

static int rx_alternatives(rx *r, int depth) {
  int left = rx_sequence(r, depth);
  while (!r->error && rx_peek(r) == '|') {
    r->at++;
    int right = rx_sequence(r, depth);
    int alt = rx_node_new(r, N_ALT);
    if (r->error)
      return 0;
    r->nodes[alt].a = left;
    r->nodes[alt].b = right;
    left = alt;
  }
  return left;
}

static int rx_emit(rx *r, int op) {
  if (r->program_count >= RX_MAX_PROGRAM) {
    r->error = "the pattern makes too long a program (repeats of repeats)";
    return 0;
  }
  if (!rx_grow((void **)&r->program, &r->program_room, r->program_count + 1, sizeof(rx_inst))) {
    r->error = "out of memory";
    return 0;
  }
  rx_inst *i = &r->program[r->program_count];
  i->op = op;
  i->x = i->y = 0;
  i->value = 0;
  return r->program_count++;
}

// The program of a node. (A SPLIT's x is the way taken first.)
static void rx_compile(rx *r, int n) {
  if (r->error)
    return;
  rx_node *node = &r->nodes[n];
  switch (node->kind) {
  case N_EMPTY:
    break;
  case N_CHAR: {
    int i = rx_emit(r, I_CHAR);
    if (!r->error)
      r->program[i].value = node->value;
    break;
  }
  case N_ANY:
    rx_emit(r, r->dot_all ? I_ANY_NL : I_ANY);
    break;
  case N_CLASS: {
    int i = rx_emit(r, I_CLASS);
    if (!r->error)
      r->program[i].value = node->value;
    break;
  }
  case N_BOL: rx_emit(r, I_BOL); break;
  case N_EOL: rx_emit(r, I_EOL); break;
  case N_WORD: rx_emit(r, I_WORD); break;
  case N_NOT_WORD: rx_emit(r, I_NOT_WORD); break;
  case N_GROUP: {
    int child = node->a, group = node->value;
    int open = rx_emit(r, I_SAVE);
    if (!r->error)
      r->program[open].value = 2 * group;
    rx_compile(r, child);
    int close = rx_emit(r, I_SAVE);
    if (!r->error)
      r->program[close].value = 2 * group + 1;
    break;
  }
  case N_CAT: {
    int a = node->a, b = node->b;
    rx_compile(r, a);
    rx_compile(r, b);
    break;
  }
  case N_ALT: {
    int a = node->a, b = node->b;
    int split = rx_emit(r, I_SPLIT);
    rx_compile(r, a);
    int jump = rx_emit(r, I_JMP);
    if (r->error)
      return;
    r->program[split].x = split + 1;
    r->program[split].y = r->program_count;
    rx_compile(r, b);
    if (!r->error)
      r->program[jump].x = r->program_count;
    break;
  }
  case N_REPEAT: {
    int child = node->a, min = node->min, max = node->max;
    bool greedy = node->greedy;
    for (int k = 0; k < min && !r->error; ++k)
      rx_compile(r, child);
    if (max < 0) {
      // L: split body, out; body; jmp L
      int split = rx_emit(r, I_SPLIT);
      rx_compile(r, child);
      int jump = rx_emit(r, I_JMP);
      if (r->error)
        return;
      r->program[jump].x = split;
      int out = r->program_count;
      r->program[split].x = greedy ? split + 1 : out;
      r->program[split].y = greedy ? out : split + 1;
    } else {
      // (max - min) times: split body, out — all going out to the end.
      int optional = max - min;
      int splits[RX_MAX_REPEAT + 1];
      for (int k = 0; k < optional && !r->error; ++k) {
        splits[k] = rx_emit(r, I_SPLIT);
        rx_compile(r, child);
      }
      if (r->error)
        return;
      int out = r->program_count;
      for (int k = 0; k < optional; ++k) {
        r->program[splits[k]].x = greedy ? splits[k] + 1 : out;
        r->program[splits[k]].y = greedy ? out : splits[k] + 1;
      }
    }
    break;
  }
  }
}

static void rx_free(rx *r) {
  free(r->nodes);
  free(r->classes);
  free(r->ranges);
  free(r->program);
}

static const char *rx_flags(rx *r, const ent_text *flags) {
  for (uint32_t i = 0; flags && i < flags->length; ++i) {
    char f = flags->bytes[i];
    if (f == 'i')
      r->ignore_case = true;
    else if (f == 'm')
      r->multiline = true;
    else if (f == 's')
      r->dot_all = true;
    else if (f != 'g' && f != 'u' && f != 'y' && f != 'd')
      return "a flag is none of i m s g u y d";
  }
  return 0;
}

// The pattern read and its program made; false where it is wrong
// (`r->error` says why).
static bool rx_make(rx *r, const ent_text *pattern, const ent_text *flags) {
  memset(r, 0, sizeof *r);
  r->p = (const unsigned char *)(pattern ? pattern->bytes : "");
  r->n = pattern ? pattern->length : 0;
  r->error = rx_flags(r, flags);
  if (r->error)
    return false;
  int root = rx_alternatives(r, 0);
  if (!r->error && rx_more(r))
    r->error = "a ) has no ( before it";
  if (!r->error)
    rx_compile(r, root);
  if (!r->error)
    rx_emit(r, I_MATCH);
  return !r->error;
}

static int32_t rx_fold(int32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool rx_in_class(const rx *r, int k, int32_t c) {
  const rx_class *cl = &r->classes[k];
  bool in = false;
  for (int i = 0; i < cl->count && !in; ++i) {
    int32_t low = r->ranges[(cl->first + i) * 2], high = r->ranges[(cl->first + i) * 2 + 1];
    if (c >= low && c <= high)
      in = true;
    else if (r->ignore_case) {
      int32_t other = c >= 'a' && c <= 'z' ? c - 32 : rx_fold(c);
      if (other != c && other >= low && other <= high)
        in = true;
    }
  }
  return in != cl->negated;
}
static bool rx_word_byte(const unsigned char *p, size_t n, size_t at) {
  if (at >= n)
    return false;
  unsigned char c = p[at];
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool rx_word_before(const unsigned char *p, size_t n, size_t at) {
  return at > 0 && rx_word_byte(p, n, at - 1);
}

// The machine: a list of threads, each a place in the program and where
// its match started, in the order of their priority.
// Where groups are asked for (`slots`: 2 for each, and 2 for the whole
// match), each thread has where its groups started and ended so far
// (SIZE_MAX: not yet), `slots` of them in `caps` from `cap`.
typedef struct { int pc; size_t start; } rx_thread;
typedef struct {
  rx_thread *items;
  int count;
  size_t *caps;
} rx_list;

// A thread added at `pc`, following jumps, splits and what matches no
// character (in order: the way a split takes first, first), each place
// once a step (`mark`).
static void rx_add(const rx *r, rx_list *list, int *marks, int mark, int pc, size_t start,
                   const unsigned char *t, size_t n, size_t at, int *stack,
                   const size_t *caps, int slots, size_t *scratch) {
  // (Kept on a stack of its own: a program may be long. With groups, each
  // place on it has its groups in `scratch`: stack[k + 1] says which.)
  int top = 0, used = 0;
  stack[top++] = pc;
  if (slots) {
    memcpy(scratch, caps, sizeof(size_t) * (size_t)slots);
    stack[top++] = used++;
  }
  while (top > 0) {
    int slot = slots ? stack[--top] : 0;
    int at_pc = stack[--top];
    size_t *mine = slots ? scratch + (size_t)slot * (size_t)slots : NULL;
    if (marks[at_pc] == mark)
      continue;
    marks[at_pc] = mark;
    const rx_inst *i = &r->program[at_pc];
#define RX_PUSH(pc_, slot_)                                                    \
  do {                                                                         \
    stack[top++] = (pc_);                                                      \
    if (slots)                                                                 \
      stack[top++] = (slot_);                                                  \
  } while (0)
    switch (i->op) {
    case I_JMP:
      RX_PUSH(i->x, slot);
      break;
    case I_SPLIT:
      // (The way taken second gets a copy: the first may change its own.)
      if (slots) {
        int other = used++;
        memcpy(scratch + (size_t)other * (size_t)slots, mine, sizeof(size_t) * (size_t)slots);
        RX_PUSH(i->y, other);
      } else
        RX_PUSH(i->y, 0);
      RX_PUSH(i->x, slot);
      break;
    case I_SAVE:
      if (slots && i->value < slots)
        mine[i->value] = at;
      RX_PUSH(at_pc + 1, slot);
      break;
    case I_BOL:
      if (at == 0 || (r->multiline && (t[at - 1] == '\n' || t[at - 1] == '\r')))
        RX_PUSH(at_pc + 1, slot);
      break;
    case I_EOL:
      if (at == n || (r->multiline && (t[at] == '\n' || t[at] == '\r')))
        RX_PUSH(at_pc + 1, slot);
      break;
    case I_WORD:
      if (rx_word_before(t, n, at) != rx_word_byte(t, n, at))
        RX_PUSH(at_pc + 1, slot);
      break;
    case I_NOT_WORD:
      if (rx_word_before(t, n, at) == rx_word_byte(t, n, at))
        RX_PUSH(at_pc + 1, slot);
      break;
    default:
      list->items[list->count].pc = at_pc;
      list->items[list->count].start = start;
      if (slots)
        memcpy(list->caps + (size_t)list->count * (size_t)slots, mine,
               sizeof(size_t) * (size_t)slots);
      list->count++;
    }
#undef RX_PUSH
  }
}

// The first match from byte `from` on: where it starts and ends; false
// where there is none.
static bool rx_run_groups(const rx *r, const ent_text *text, int32_t from, size_t *found_start,
                          size_t *found_end, size_t *groups);
static bool rx_run(const rx *r, const ent_text *text, int32_t from, size_t *found_start,
                   size_t *found_end) {
  return rx_run_groups(r, text, from, found_start, found_end, NULL);
}
// The same, and where `groups` is given, where each group of the match
// started and ended (2 for each, group 1 first; SIZE_MAX where it matched
// nothing).
static bool rx_run_groups(const rx *r, const ent_text *text, int32_t from, size_t *found_start,
                          size_t *found_end, size_t *groups) {
  const unsigned char *t = (const unsigned char *)(text ? text->bytes : "");
  size_t n = text ? text->length : 0;
  if (from < 0)
    from = 0;
  if ((size_t)from > n)
    return false;
  int size = r->program_count;
  int slots = groups ? 2 * (r->group_count + 1) : 0;
  rx_thread *a = malloc(sizeof(rx_thread) * (size_t)size);
  rx_thread *b = malloc(sizeof(rx_thread) * (size_t)size);
  int *marks = malloc(sizeof(int) * (size_t)size);
  int *stack = malloc(sizeof(int) * (size_t)(size * 4 + 4));
  size_t *a_caps = slots ? malloc(sizeof(size_t) * (size_t)size * (size_t)slots) : NULL;
  size_t *b_caps = slots ? malloc(sizeof(size_t) * (size_t)size * (size_t)slots) : NULL;
  size_t *scratch = slots ? malloc(sizeof(size_t) * (size_t)(size * 2 + 2) * (size_t)slots) : NULL;
  size_t *none = slots ? malloc(sizeof(size_t) * (size_t)slots) : NULL;
  if (!a || !b || !marks || !stack || (slots && (!a_caps || !b_caps || !scratch || !none))) {
    free(a), free(b), free(marks), free(stack), free(a_caps), free(b_caps), free(scratch), free(none);
    return false;
  }
  for (int k = 0; k < slots; ++k)
    none[k] = SIZE_MAX;
  for (int k = 0; k < size; ++k)
    marks[k] = -1;
  rx_list now = {a, 0, a_caps}, next = {b, 0, b_caps};
  bool matched = false;
  int mark = 0;
  size_t at = (size_t)from;
  rx_add(r, &now, marks, mark, 0, at, t, n, at, stack, none, slots, scratch);
  for (;;) {
    size_t step;
    int32_t c = at < n ? rx_decode(t, n, at, &step) : -1;
    if (at >= n)
      step = n;
    mark++;
    next.count = 0;
    for (int k = 0; k < now.count; ++k) {
      const rx_inst *i = &r->program[now.items[k].pc];
      bool takes = false;
      switch (i->op) {
      case I_MATCH:
        matched = true;
        *found_start = now.items[k].start;
        *found_end = at;
        if (slots)
          memcpy(groups, now.caps + (size_t)k * (size_t)slots + 2,
                 sizeof(size_t) * (size_t)(slots - 2));
        // (Threads after it in priority are dropped.)
        now.count = k + 1;
        break;
      case I_CHAR:
        takes = c >= 0 && (c == i->value || (r->ignore_case && rx_fold(c) == rx_fold(i->value)));
        break;
      case I_ANY:
        takes = c >= 0 && c != '\n' && c != '\r' && c != 0x2028 && c != 0x2029;
        break;
      case I_ANY_NL:
        takes = c >= 0;
        break;
      case I_CLASS:
        takes = c >= 0 && rx_in_class(r, i->value, c);
        break;
      }
      if (takes)
        rx_add(r, &next, marks, mark, now.items[k].pc + 1, now.items[k].start, t, n, step, stack,
               slots ? now.caps + (size_t)k * (size_t)slots : NULL, slots, scratch);
    }
    if (at >= n)
      break;
    // A new start here, last in priority, where no match is found yet.
    if (!matched)
      rx_add(r, &next, marks, mark, 0, step, t, n, step, stack, none, slots, scratch);
    // (No thread left: done where one matched, else on to the next start.)
    if (next.count == 0 && matched)
      break;
    rx_list swap = now;
    now = next;
    next = swap;
    at = step;
  }
  free(a), free(b), free(marks), free(stack), free(a_caps), free(b_caps), free(scratch), free(none);
  return matched;
}

// What devices/regex.ent declares.

ENT_REGEX_SAME_RESULT const ent_text *ent_regex_regex_error(const ent_text *pattern,
                                                             const ent_text *flags) {
  rx r;
  bool ok = rx_make(&r, pattern, flags);
  const char *why = ok ? "" : r.error;
  rx_free(&r);
  size_t length = strlen(why);
  ent_text *said = ent_text_new((uint32_t)length);
  if (said && length)
    memcpy(said->bytes, why, length);
  return said;
}

ENT_REGEX_SAME_RESULT bool ent_regex_regex_matches(const ent_text *t, const ent_text *pattern,
                                                   const ent_text *flags) {
  rx r;
  size_t start, end;
  bool found = rx_make(&r, pattern, flags) && rx_run(&r, t, 0, &start, &end);
  rx_free(&r);
  return found;
}

ENT_REGEX_SAME_RESULT int32_t ent_regex_regex_find(const ent_text *t, const ent_text *pattern,
                                                   const ent_text *flags, int32_t from) {
  rx r;
  size_t start = 0, end = 0;
  bool found = rx_make(&r, pattern, flags) && rx_run(&r, t, from, &start, &end);
  rx_free(&r);
  return found ? (int32_t)start : -1;
}

ENT_REGEX_SAME_RESULT int32_t ent_regex_regex_end(const ent_text *t, const ent_text *pattern,
                                                  const ent_text *flags, int32_t from) {
  rx r;
  size_t start = 0, end = 0;
  bool found = rx_make(&r, pattern, flags) && rx_run(&r, t, from, &start, &end);
  rx_free(&r);
  return found ? (int32_t)end : -1;
}

// What group `group` of the first match from byte `from` on matched ("" where
// there is no match, no such group, or it matched nothing); group 0 is the
// whole match.
ENT_REGEX_SAME_RESULT const ent_text *ent_regex_regex_group(const ent_text *t, const ent_text *pattern,
                                                             const ent_text *flags, int32_t from,
                                                             int32_t group) {
  rx r;
  size_t start = 0, end = 0;
  bool made = rx_make(&r, pattern, flags);
  int count = made ? r.group_count : 0;
  size_t *groups = malloc(sizeof(size_t) * (size_t)(2 * count + 2));
  bool found = made && groups && group >= 0 && group <= count &&
               rx_run_groups(&r, t, from, &start, &end, groups);
  size_t a = 0, z = 0;
  if (found && group == 0) {
    a = start;
    z = end;
  } else if (found && groups[2 * (group - 1)] != SIZE_MAX && groups[2 * (group - 1) + 1] != SIZE_MAX) {
    a = groups[2 * (group - 1)];
    z = groups[2 * (group - 1) + 1];
  }
  rx_free(&r);
  free(groups);
  size_t length = z > a ? z - a : 0;
  ent_text *said = ent_text_new((uint32_t)length);
  if (said && length)
    memcpy(said->bytes, t->bytes + a, length);
  return said;
}
