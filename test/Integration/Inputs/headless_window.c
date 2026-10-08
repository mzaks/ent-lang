// The window device (devices/window.ent) without a window: what would be
// drawn is printed, a line a call, and a text is as wide as its letters
// are many (half its size each). For tests, which cannot ask for a
// display and raylib: copied next to a copy of window.ent, in place of
// devices/window.c.

#include "ent_extern.h"

#include <stdio.h>
#include <string.h>

#define TEXT(name, text)                                                     \
  char name[sizeof((text)->bytes) + 1];                                      \
  memcpy(name, (text)->bytes, (text)->length);                               \
  name[(text)->length] = 0

// What is laid on and how far off things are, said when it is another.
static int32_t shade_color;
static float shade_part, shift_x, shift_y;
static float clip_at[4] = {0, 0, -1, -1};

void ent_window_shade(int32_t color, float part) {
  if (part <= 0)
    color = 0, part = 0;
  if (color == shade_color && part == shade_part)
    return;
  shade_color = color;
  shade_part = part;
  printf("shade %06x %.2f\n", (unsigned)color & 0xffffff, part);
}

void ent_window_shift(float dx, float dy) {
  if (dx == shift_x && dy == shift_y)
    return;
  shift_x = dx;
  shift_y = dy;
  printf("shift %g %g\n", dx, dy);
}

void ent_window_clip(float x, float y, float w, float h) {
  // (More than any window: no clipping.)
  if (w > 1e8f) {
    ent_window_unclip();
    return;
  }
  if (x == clip_at[0] && y == clip_at[1] && w == clip_at[2] && h == clip_at[3])
    return;
  clip_at[0] = x, clip_at[1] = y, clip_at[2] = w, clip_at[3] = h;
  printf("clip %g %g %g %g\n", x, y, w, h);
}

void ent_window_unclip(void) {
  if (clip_at[2] < 0)
    return;
  clip_at[2] = clip_at[3] = -1;
  printf("unclip\n");
}

void ent_window_rect(float x, float y, float w, float h, int32_t color) {
  printf("rect %g %g %g %g %08x\n", x, y, w, h, (unsigned)color);
}

void ent_window_rounded(float x, float y, float w, float h, float tl, float tr,
                        float bl, float br, int32_t color) {
  printf("rounded %g %g %g %g by %g %g %g %g %08x\n", x, y, w, h, tl, tr, bl,
         br, (unsigned)color);
}

void ent_window_edges(float x, float y, float w, float h, float left,
                      float right, float top, float bottom, float tl, float tr,
                      float bl, float br, int32_t color) {
  printf("edges %g %g %g %g of %g %g %g %g %08x\n", x, y, w, h, left, right,
         top, bottom, (unsigned)color);
}

void ent_window_label(const ent_text126 *text, float x, float y, float size,
                      int32_t color) {
  TEXT(said, text);
  printf("label %g %g \"%s\"\n", x, y, said);
}

void ent_window_write(const ent_text126 *text, float x, float y, float width,
                      int32_t font, float size, float spacing, float line,
                      int32_t breaks, int32_t along, int32_t color) {
  TEXT(said, text);
  printf("write %g %g in %g \"%s\" %08x\n", x, y, width, said,
         (unsigned)color);
}

void ent_window_picture(int32_t image, float x, float y, float w, float h,
                        int32_t tint) {
  printf("picture %d %g %g %g %g\n", image, x, y, w, h);
}

// A letter is half as wide as the text is high.
float ent_window_text_extent(const ent_text126 *text, int32_t font, float size,
                             float spacing, int32_t breaks) {
  return (float)text->length * size * 0.5f;
}

float ent_window_text_least(const ent_text126 *text, int32_t font, float size,
                            float spacing, int32_t breaks) {
  // The longest word.
  int longest = 0, run = 0;
  for (int i = 0; i < text->length; ++i) {
    run = text->bytes[i] == ' ' ? 0 : run + 1;
    if (run > longest)
      longest = run;
  }
  return (float)longest * size * 0.5f;
}

int32_t ent_window_text_lines(const ent_text126 *text, float width,
                              int32_t font, float size, float spacing,
                              int32_t breaks) {
  // Word after word, in the next line where one would reach past.
  int room = (int)(width / (size * 0.5f)), lines = 1, at = 0, word = 0;
  for (int i = 0; i <= text->length; ++i) {
    if (i < text->length && text->bytes[i] != ' ') {
      ++word;
      continue;
    }
    if (at > 0 && at + 1 + word > room) {
      ++lines;
      at = word;
    } else {
      at += (at > 0) + word;
    }
    word = 0;
  }
  return lines;
}

int32_t ent_window_blend(int32_t from, int32_t to, float part) {
  if (part <= 0)
    return from;
  if (part >= 1)
    return to;
  uint32_t a = (uint32_t)from, b = (uint32_t)to;
  if (!(a >> 24))
    a |= 0xff000000u;
  if (!(b >> 24))
    b |= 0xff000000u;
  uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    float x = (float)((a >> shift) & 255), y = (float)((b >> shift) & 255);
    result |= (uint32_t)(x + (y - x) * part + 0.5f) << shift;
  }
  return (int32_t)result;
}

int32_t ent_window_solid(int32_t color) {
  return (int32_t)((uint32_t)color | 0xff000000u);
}

float ent_window_strength(int32_t color, float part) {
  uint32_t alpha = (uint32_t)color >> 24;
  return part * (alpha ? (float)alpha / 255.0f : 1.0f);
}

// The rest does nothing here.
bool ent_window_key_down(int32_t key) { return false; }
void ent_window_circle(float x, float y, float r, int32_t color) {}
void ent_window_sector(float x, float y, float r, float from, float to,
                       int32_t color) {}
void ent_window_line(float x0, float y0, float x1, float y1, int32_t color) {}
float ent_window_text_width(const ent_text126 *text, float size) {
  return (float)text->length * size * 0.5f;
}
void ent_window_paragraph(const ent_text126 *text, float x, float y,
                          float width, float size, int32_t color) {}
float ent_window_text_min_width(const ent_text126 *text, float size) {
  return 0;
}
float ent_window_text_height(const ent_text126 *text, float size, float width) {
  return size;
}
int32_t ent_window_font(const ent_text126 *file, int32_t size) { return 0; }
int32_t ent_window_picture_load(const ent_text126 *file) { return 0; }
float ent_window_picture_ratio(int32_t image) { return 1; }
void ent_window_screenshot(const ent_text126 *file) {}
// A window that is as big as it was opened; one that can be given
// another size is 100 wider and 60 higher from its third frame on, as if
// the user had dragged its corner; and 1280 by 720 while it fills the
// screen.
static int32_t opened_width, opened_height, frames;
static bool can_resize, fills;

void ent_window_open_window(int32_t width, int32_t height,
                            const ent_text62 *title, bool resizable) {
  opened_width = width;
  opened_height = height;
  can_resize = resizable;
}
int32_t ent_window_window_width(void) {
  return fills ? 1280 : opened_width + (can_resize && frames >= 2 ? 100 : 0);
}
int32_t ent_window_window_height(void) {
  return fills ? 720 : opened_height + (can_resize && frames >= 2 ? 60 : 0);
}
void ent_window_fill_screen(bool on) { fills = on; }
bool ent_window_should_close(void) { return false; }
float ent_window_frame_seconds(void) { return 0; }
void ent_window_begin_frame(int32_t background, int32_t fps) { ++frames; }
void ent_window_end_frame(void) {}
float ent_window_mouse_x(void) { return 0; }
float ent_window_mouse_y(void) { return 0; }
bool ent_window_mouse_down(int32_t button) { return false; }
float ent_window_mouse_wheel_x(void) { return 0; }
float ent_window_mouse_wheel_y(void) { return 0; }
