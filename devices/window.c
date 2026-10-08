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

void ent_window_screenshot(const ent_text126 *file) {
  ENT_WINDOW_TEXT(name, file);
  // What was drawn so far is still waiting to go to the picture.
  rlDrawRenderBatchActive();
  TakeScreenshot(name);
}
