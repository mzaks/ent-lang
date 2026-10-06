// The window device (devices/window.ent), drawn with raylib. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <raylib.h>
#include <rlgl.h>
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

void ent_window_screenshot(const ent_text126 *file) {
  ENT_WINDOW_TEXT(name, file);
  // What was drawn so far is still waiting to go to the picture.
  rlDrawRenderBatchActive();
  TakeScreenshot(name);
}
