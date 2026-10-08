// The window device (devices/window.ent), drawn with raylib. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <raylib.h>
#include <rlgl.h>
#include <stdio.h>
#include <string.h>

// 0xRRGGBB, opaque.
static Color ent_window_color(int32_t rgb) {
  return (Color){(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8),
                 (unsigned char)rgb, 255};
}

// raylib takes its texts terminated.
#define ENT_WINDOW_TEXT(name, text)                                          \
  char name[sizeof((text)->bytes) + 1];                                      \
  memcpy(name, (text)->bytes, (text)->length);                               \
  name[(text)->length] = 0

void ent_window_open_window(int32_t width, int32_t height,
                            const ent_text62 *title) {
  ENT_WINDOW_TEXT(name, title);
  SetTraceLogLevel(LOG_WARNING);
  InitWindow(width, height, name);
}

bool ent_window_should_close(void) { return WindowShouldClose(); }
float ent_window_frame_seconds(void) { return GetFrameTime(); }

void ent_window_begin_frame(int32_t background, int32_t fps) {
  static int32_t held = -1;
  if (fps != held) {
    SetTargetFPS(fps);
    held = fps;
  }
  BeginDrawing();
  ClearBackground(ent_window_color(background));
}

void ent_window_end_frame(void) { EndDrawing(); }

float ent_window_mouse_x(void) { return (float)GetMouseX(); }
float ent_window_mouse_y(void) { return (float)GetMouseY(); }
bool ent_window_mouse_down(int32_t button) { return IsMouseButtonDown(button); }
bool ent_window_key_down(int32_t key) { return IsKeyDown(key); }

void ent_window_rect(float x, float y, float width, float height,
                     int32_t color) {
  DrawRectangleRec((Rectangle){x, y, width, height}, ent_window_color(color));
}

void ent_window_circle(float x, float y, float radius, int32_t color) {
  DrawCircleV((Vector2){x, y}, radius, ent_window_color(color));
}

void ent_window_sector(float x, float y, float radius, float from, float to,
                       int32_t color) {
  DrawCircleSector((Vector2){x, y}, radius, from, to, 48,
                   ent_window_color(color));
}

void ent_window_line(float x0, float y0, float x1, float y1, int32_t color) {
  DrawLineV((Vector2){x0, y0}, (Vector2){x1, y1}, ent_window_color(color));
}

void ent_window_label(const ent_text126 *text, float x, float y, float size,
                      int32_t color) {
  ENT_WINDOW_TEXT(line, text);
  DrawText(line, (int)x, (int)y, (int)size, ent_window_color(color));
}

float ent_window_text_width(const ent_text126 *text, float size) {
  ENT_WINDOW_TEXT(line, text);
  return (float)MeasureText(line, (int)size);
}

// Breaks a text into lines no wider than `width`, at its spaces (a word
// wider than that has a line to itself), draws them if asked, and says
// how many there are.
static int ent_window_wrap(const char *text, float size, float width,
                           int draw, float x, float y, Color color) {
  char line[128], trial[128];
  int length = 0, lines = 0;
  float step = size + size / 4;
  const char *at = text;
  for (;;) {
    while (*at == ' ')
      at++;
    const char *end = at;
    while (*end && *end != ' ')
      end++;
    int word = (int)(end - at);
    if (!word)
      break;
    int both = length ? snprintf(trial, sizeof trial, "%.*s %.*s", length,
                                 line, word, at)
                      : snprintf(trial, sizeof trial, "%.*s", word, at);
    if (length && MeasureText(trial, (int)size) > width) {
      if (draw)
        DrawText(line, (int)x, (int)(y + lines * step), (int)size, color);
      lines++;
      length = snprintf(line, sizeof line, "%.*s", word, at);
    } else {
      memcpy(line, trial, both + 1);
      length = both;
    }
    at = end;
  }
  if (length) {
    if (draw)
      DrawText(line, (int)x, (int)(y + lines * step), (int)size, color);
    lines++;
  }
  return lines;
}

float ent_window_text_min_width(const ent_text126 *text, float size) {
  ENT_WINDOW_TEXT(all, text);
  char word[128];
  int widest = 0;
  const char *at = all;
  for (;;) {
    while (*at == ' ')
      at++;
    const char *end = at;
    while (*end && *end != ' ')
      end++;
    if (end == at)
      break;
    snprintf(word, sizeof word, "%.*s", (int)(end - at), at);
    int width = MeasureText(word, (int)size);
    if (width > widest)
      widest = width;
    at = end;
  }
  return (float)widest;
}

void ent_window_paragraph(const ent_text126 *text, float x, float y,
                          float width, float size, int32_t color) {
  ENT_WINDOW_TEXT(all, text);
  ent_window_wrap(all, size, width, 1, x, y, ent_window_color(color));
}

float ent_window_text_height(const ent_text126 *text, float size,
                             float width) {
  ENT_WINDOW_TEXT(all, text);
  int lines = ent_window_wrap(all, size, width, 0, 0, 0, BLACK);
  return lines ? lines * size + (lines - 1) * (size / 4) : 0;
}

// Fonts: the window's own, and those read from files.
static Font ent_window_fonts[16];
static int ent_window_font_count = 1;

static Font ent_window_font_of(int32_t font) {
  if (font <= 0 || font >= ent_window_font_count)
    return GetFontDefault();
  return ent_window_fonts[font];
}

int32_t ent_window_font(const ent_text126 *file, int32_t size) {
  ENT_WINDOW_TEXT(name, file);
  if (ent_window_font_count >= 16 || !FileExists(name))
    return 0;
  Font font = LoadFontEx(name, size, 0, 0);
  if (font.texture.id == 0)
    return 0;
  ent_window_fonts[ent_window_font_count] = font;
  return ent_window_font_count++;
}

// (raylib draws its own font with a tenth of the size between letters.)
static float ent_window_spacing(int32_t font, float size, float spacing) {
  if (font <= 0 || font >= ent_window_font_count)
    spacing += size < 10 ? 1 : size / 10;
  return spacing;
}

static float ent_window_measure(const char *text, int32_t font, float size,
                                float spacing) {
  if (!*text)
    return 0;
  return MeasureTextEx(ent_window_font_of(font), text, size,
                       ent_window_spacing(font, size, spacing))
      .x;
}

// Sets a text in lines: no wider than `width` where it breaks at words
// (breaks 0), at "\n" then and with breaks 1, nowhere with 2. Calls
// `each` for every line, if given, and says how many there are, and how
// wide the widest is.
typedef void ent_window_each_line(const char *line, float wide, int index,
                             void *with);
static int ent_window_set(const char *text, float width, int32_t font,
                          float size, float spacing, int breaks,
                          float *widest, ent_window_each_line *each, void *with) {
  char line[128], trial[128];
  int length = 0, lines = 0;
  float most = 0;
  const char *at = text;
#define ENT_WINDOW_END_LINE()                                                \
  do {                                                                       \
    line[length] = 0;                                                        \
    float wide = ent_window_measure(line, font, size, spacing);              \
    if (wide > most)                                                         \
      most = wide;                                                           \
    if (each)                                                                \
      each(line, wide, lines, with);                                         \
    lines++;                                                                 \
    length = 0;                                                              \
  } while (0)
  if (breaks == 2) {
    // All of it, a line break a space.
    for (; *at && length < 126; at++)
      line[length++] = *at == '\n' ? ' ' : *at;
    ENT_WINDOW_END_LINE();
  } else if (breaks == 1) {
    for (;; at++) {
      if (*at == '\n' || !*at) {
        ENT_WINDOW_END_LINE();
        if (!*at)
          break;
      } else if (length < 126) {
        line[length++] = *at;
      }
    }
  } else {
    for (;;) {
      while (*at == ' ')
        at++;
      if (*at == '\n') {
        ENT_WINDOW_END_LINE();
        at++;
        continue;
      }
      const char *end = at;
      while (*end && *end != ' ' && *end != '\n')
        end++;
      int word = (int)(end - at);
      if (!word)
        break;
      line[length] = 0;
      int both = length ? snprintf(trial, sizeof trial, "%s %.*s", line,
                                   word, at)
                        : snprintf(trial, sizeof trial, "%.*s", word, at);
      if (length &&
          ent_window_measure(trial, font, size, spacing) > width + 0.01f) {
        ENT_WINDOW_END_LINE();
        length = snprintf(line, sizeof line, "%.*s", word, at);
      } else {
        memcpy(line, trial, both + 1);
        length = both;
      }
      at = end;
    }
    if (length || !lines)
      ENT_WINDOW_END_LINE();
  }
#undef ENT_WINDOW_END_LINE
  if (widest)
    *widest = most;
  return lines;
}

struct ent_window_writing {
  float x, y, width, size, spacing, line;
  int32_t font, align;
  Color color;
};

static void ent_window_write_line(const char *line, float wide, int index,
                                  void *with) {
  struct ent_window_writing *w = with;
  float x = w->x + (w->width - wide) * (w->align == 1 ? 0.5f
                                        : w->align == 2 ? 1.0f : 0.0f);
  DrawTextEx(ent_window_font_of(w->font), line,
             (Vector2){(float)(int)x, (float)(int)(w->y + index * w->line)},
             w->size, ent_window_spacing(w->font, w->size, w->spacing),
             w->color);
}

void ent_window_write(const ent_text126 *text, float x, float y, float width,
                      int32_t font, float size, float spacing, float line,
                      int32_t breaks, int32_t align, int32_t color) {
  ENT_WINDOW_TEXT(all, text);
  struct ent_window_writing w = {x, y, width, size, spacing,
                                 line > 0 ? line : size, font, align,
                                 ent_window_color(color)};
  ent_window_set(all, width, font, size, spacing, breaks, 0,
                 ent_window_write_line, &w);
}

int32_t ent_window_text_lines(const ent_text126 *text, float width,
                              int32_t font, float size, float spacing,
                              int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  return ent_window_set(all, width, font, size, spacing, breaks, 0, 0, 0);
}

float ent_window_text_extent(const ent_text126 *text, int32_t font,
                             float size, float spacing, int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  float widest = 0;
  // (With all the room it wants: broken only where it must be.)
  ent_window_set(all, 1.0e30f, font, size, spacing, breaks, &widest, 0, 0);
  return widest;
}

float ent_window_text_least(const ent_text126 *text, int32_t font, float size,
                            float spacing, int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  float widest = 0;
  // (With no room at all: every word a line, where it breaks at words.)
  ent_window_set(all, 0, font, size, spacing, breaks, &widest, 0, 0);
  return widest;
}

void ent_window_screenshot(const ent_text126 *file) {
  ENT_WINDOW_TEXT(name, file);
  // What was drawn so far is still waiting to go to the picture.
  rlDrawRenderBatchActive();
  TakeScreenshot(name);
}
