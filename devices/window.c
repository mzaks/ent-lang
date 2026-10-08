// The window device (devices/window.ent), drawn with raylib. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <raylib.h>
#include <rlgl.h>
#include <stdio.h>
#include <string.h>

// 0xRRGGBB, opaque; or 0xAARRGGBB, so much of it (AA from 01, nearly
// nothing, to ff, all; 00 is all too, so that 0xRRGGBB is itself).
// With what `shade` laid on everything that is drawn: so much of that
// colour in place of its own.
static Color ent_window_shading;
static float ent_window_shading_part;

static Color ent_window_color(int32_t rgb) {
  unsigned char alpha = (unsigned char)((uint32_t)rgb >> 24);
  Color color = {(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8),
                 (unsigned char)rgb, alpha ? alpha : 255};
  if (ent_window_shading_part > 0) {
    float part = ent_window_shading_part * (float)ent_window_shading.a / 255.0f;
    color.r += (unsigned char)(((float)ent_window_shading.r - color.r) * part);
    color.g += (unsigned char)(((float)ent_window_shading.g - color.g) * part);
    color.b += (unsigned char)(((float)ent_window_shading.b - color.b) * part);
  }
  return color;
}

void ent_window_shade(int32_t color, float part) {
  ent_window_shading_part = 0;
  ent_window_shading = ent_window_color(color);
  ent_window_shading_part = part < 0 ? 0 : part > 1 ? 1 : part;
}

// What is drawn from here on is so far to the right and down from where
// it says: whole pixels, which keep texts sharp.
static int ent_window_shifted;

void ent_window_shift(float dx, float dy) {
  if (ent_window_shifted)
    rlPopMatrix();
  ent_window_shifted = 0;
  dx = (float)(int)(dx < 0 ? dx - 0.5f : dx + 0.5f);
  dy = (float)(int)(dy < 0 ? dy - 0.5f : dy + 0.5f);
  if (dx == 0 && dy == 0)
    return;
  rlPushMatrix();
  rlTranslatef(dx, dy, 0);
  ent_window_shifted = 1;
}

// raylib takes its texts terminated.
#define ENT_WINDOW_TEXT(name, text)                                          \
  char name[sizeof((text)->bytes) + 1];                                      \
  memcpy(name, (text)->bytes, (text)->length);                               \
  name[(text)->length] = 0

void ent_window_open_window(int32_t width, int32_t height,
                            const ent_text62 *title, bool resizable) {
  ENT_WINDOW_TEXT(name, title);
  SetTraceLogLevel(LOG_WARNING);
  // (In points: on a screen with small pixels the picture has more of
  // them, and everything is as big as on any other.)
  SetConfigFlags(FLAG_WINDOW_HIGHDPI | (resizable ? FLAG_WINDOW_RESIZABLE : 0));
  InitWindow(width, height, name);
}

void ent_window_window_limits(int32_t min_width, int32_t min_height) {
  SetWindowMinSize(min_width, min_height);
}

float ent_window_window_scale(void) { return GetWindowScaleDPI().x; }

void ent_window_resize_window(int32_t width, int32_t height) {
  SetWindowSize(width, height);
}

// How big the window is, in points: by the picture that is drawn into,
// which is as big as the window really is (one that the program asked
// another size for may not have got it).
int32_t ent_window_window_width(void) {
  float scale = GetWindowScaleDPI().x;
  return (int32_t)((float)GetRenderWidth() / (scale > 0 ? scale : 1) + 0.5f);
}
int32_t ent_window_window_height(void) {
  float scale = GetWindowScaleDPI().y;
  return (int32_t)((float)GetRenderHeight() / (scale > 0 ? scale : 1) + 0.5f);
}

// Without a border, as big as the screen it is on: no other video mode,
// and it is as it was when it is no longer asked for.
void ent_window_fill_screen(bool on) {
  if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) != on)
    ToggleBorderlessWindowed();
}

bool ent_window_should_close(void) { return WindowShouldClose(); }
float ent_window_frame_seconds(void) { return GetFrameTime(); }

// Everything is drawn so many times as big as it says (`Window.zoom`).
static float ent_window_zoom = 1;

void ent_window_begin_frame(int32_t background, int32_t fps, float zoom) {
  static int32_t held = -1;
  if (fps != held) {
    SetTargetFPS(fps);
    held = fps;
  }
  BeginDrawing();
  ent_window_shading_part = 0;
  ClearBackground(ent_window_color(background));
  ent_window_zoom = zoom > 0 ? zoom : 1;
  rlPushMatrix();
  rlScalef(ent_window_zoom, ent_window_zoom, 1);
}

void ent_window_end_frame(void) {
  ent_window_shift(0, 0);
  ent_window_unclip();
  rlPopMatrix();
  EndDrawing();
}

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
  // (A line is as long as a text can be, where nothing breaks it.)
  char line[1024], trial[1024];
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
// A font from a file is drawn from letters made as big as they are to
// show: read again at that size where the window's zoom, or a screen
// with small pixels, has them more pixels high than the size they were
// read at. (A few sizes of each are kept.)
#define ENT_WINDOW_SIZES 4
static struct {
  char file[128];
  Font at[ENT_WINDOW_SIZES];
  int pixels[ENT_WINDOW_SIZES];
  int next;
} ent_window_faces[16];

static Font ent_window_font_shown(int32_t font, float size) {
  if (font <= 0 || font >= ent_window_font_count)
    return GetFontDefault();
  int pixels = (int)(size * ent_window_zoom * GetWindowScaleDPI().y + 0.5f);
  if (pixels <= ent_window_fonts[font].baseSize)
    return ent_window_fonts[font];
  for (int i = 0; i < ENT_WINDOW_SIZES; ++i)
    if (ent_window_faces[font].pixels[i] == pixels)
      return ent_window_faces[font].at[i];
  Font made = LoadFontEx(ent_window_faces[font].file, pixels, 0, 0);
  if (made.texture.id == 0)
    return ent_window_fonts[font];
  SetTextureFilter(made.texture, TEXTURE_FILTER_BILINEAR);
  int slot = ent_window_faces[font].next;
  ent_window_faces[font].next = (slot + 1) % ENT_WINDOW_SIZES;
  if (ent_window_faces[font].pixels[slot])
    UnloadFont(ent_window_faces[font].at[slot]);
  ent_window_faces[font].at[slot] = made;
  ent_window_faces[font].pixels[slot] = pixels;
  return made;
}

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
  SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
  ent_window_fonts[ent_window_font_count] = font;
  strncpy(ent_window_faces[ent_window_font_count].file, name, 127);
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
  // (A line is as long as a text can be, where nothing breaks it.)
  char line[1024], trial[1024];
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
    for (; *at && length < 1022; at++)
      line[length++] = *at == '\n' ? ' ' : *at;
    ENT_WINDOW_END_LINE();
  } else if (breaks == 1) {
    for (;; at++) {
      if (*at == '\n' || !*at) {
        ENT_WINDOW_END_LINE();
        if (!*at)
          break;
      } else if (length < 1022) {
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
  DrawTextEx(ent_window_font_shown(w->font, w->size), line,
             (Vector2){(float)(int)x, (float)(int)(w->y + index * w->line)},
             w->size, ent_window_spacing(w->font, w->size, w->spacing),
             w->color);
}

void ent_window_write(const ent_text1022 *text, float x, float y, float width,
                      int32_t font, float size, float spacing, float line,
                      int32_t breaks, int32_t align, int32_t color) {
  ENT_WINDOW_TEXT(all, text);
  struct ent_window_writing w = {x, y, width, size, spacing,
                                 line > 0 ? line : size, font, align,
                                 ent_window_color(color)};
  ent_window_set(all, width, font, size, spacing, breaks, 0,
                 ent_window_write_line, &w);
}

int32_t ent_window_text_lines(const ent_text1022 *text, float width,
                              int32_t font, float size, float spacing,
                              int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  return ent_window_set(all, width, font, size, spacing, breaks, 0, 0, 0);
}

float ent_window_text_extent(const ent_text1022 *text, int32_t font,
                             float size, float spacing, int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  float widest = 0;
  // (With all the room it wants: broken only where it must be.)
  ent_window_set(all, 1.0e30f, font, size, spacing, breaks, &widest, 0, 0);
  return widest;
}

float ent_window_text_least(const ent_text1022 *text, int32_t font, float size,
                            float spacing, int32_t breaks) {
  ENT_WINDOW_TEXT(all, text);
  float widest = 0;
  // (With no room at all: every word a line, where it breaks at words.)
  ent_window_set(all, 0, font, size, spacing, breaks, &widest, 0, 0);
  return widest;
}

// A quarter of a disc or of a ring at a corner: `quarter` 0 top left, 1
// top right, 2 bottom right, 3 bottom left (angles go clockwise from the
// right).
static void ent_window_corner(float cx, float cy, float inner, float outer,
                              int quarter, Color color) {
  static const float from[4] = {180, 270, 0, 90};
  if (outer <= 0)
    return;
  if (inner <= 0)
    DrawCircleSector((Vector2){cx, cy}, outer, from[quarter],
                     from[quarter] + 90, 12, color);
  else
    DrawRing((Vector2){cx, cy}, inner, outer, from[quarter],
             from[quarter] + 90, 12, color);
}

void ent_window_rounded(float x, float y, float w, float h, float tl,
                        float tr, float bl, float br, int32_t rgb) {
  Color color = ent_window_color(rgb);
  float most = (w < h ? w : h) / 2;
  if (tl > most) tl = most;
  if (tr > most) tr = most;
  if (bl > most) bl = most;
  if (br > most) br = most;
  float left = tl > bl ? tl : bl, right = tr > br ? tr : br;
  // The middle, then the two sides between their corners, then those.
  DrawRectangleRec((Rectangle){x + left, y, w - left - right, h}, color);
  DrawRectangleRec((Rectangle){x, y + tl, left, h - tl - bl}, color);
  DrawRectangleRec((Rectangle){x + w - right, y + tr, right, h - tr - br},
                   color);
  if (tl < left)
    DrawRectangleRec((Rectangle){x + tl, y, left - tl, tl}, color);
  if (bl < left)
    DrawRectangleRec((Rectangle){x + bl, y + h - bl, left - bl, bl}, color);
  if (tr < right)
    DrawRectangleRec((Rectangle){x + w - right, y, right - tr, tr}, color);
  if (br < right)
    DrawRectangleRec((Rectangle){x + w - right, y + h - br, right - br, br},
                     color);
  ent_window_corner(x + tl, y + tl, 0, tl, 0, color);
  ent_window_corner(x + w - tr, y + tr, 0, tr, 1, color);
  ent_window_corner(x + w - br, y + h - br, 0, br, 2, color);
  ent_window_corner(x + bl, y + h - bl, 0, bl, 3, color);
}

void ent_window_edges(float x, float y, float w, float h, float left,
                      float right, float top, float bottom, float tl,
                      float tr, float bl, float br, int32_t rgb) {
  Color color = ent_window_color(rgb);
  // The straight parts, between the corners; a corner is a ring as thick
  // as the thicker of the two edges that meet in it, or their square
  // where it is not rounded.
  if (top > 0)
    DrawRectangleRec((Rectangle){x + tl, y, w - tl - tr, top}, color);
  if (bottom > 0)
    DrawRectangleRec((Rectangle){x + bl, y + h - bottom, w - bl - br, bottom},
                     color);
  if (left > 0)
    DrawRectangleRec((Rectangle){x, y + tl, left, h - tl - bl}, color);
  if (right > 0)
    DrawRectangleRec((Rectangle){x + w - right, y + tr, right, h - tr - br},
                     color);
#define ENT_WINDOW_THICK(a, b) ((a) > (b) ? (a) : (b))
  if (tl > 0 && (top > 0 || left > 0))
    ent_window_corner(x + tl, y + tl, tl - ENT_WINDOW_THICK(top, left), tl, 0,
                      color);
  if (tr > 0 && (top > 0 || right > 0))
    ent_window_corner(x + w - tr, y + tr, tr - ENT_WINDOW_THICK(top, right),
                      tr, 1, color);
  if (br > 0 && (bottom > 0 || right > 0))
    ent_window_corner(x + w - br, y + h - br,
                      br - ENT_WINDOW_THICK(bottom, right), br, 2, color);
  if (bl > 0 && (bottom > 0 || left > 0))
    ent_window_corner(x + bl, y + h - bl, bl - ENT_WINDOW_THICK(bottom, left),
                      bl, 3, color);
#undef ENT_WINDOW_THICK
}

// Pictures read from files.
static Texture2D ent_window_pictures[64];
static int ent_window_picture_count = 1;

int32_t ent_window_picture_load(const ent_text126 *file) {
  ENT_WINDOW_TEXT(name, file);
  if (ent_window_picture_count >= 64 || !FileExists(name))
    return 0;
  Texture2D texture = LoadTexture(name);
  if (texture.id == 0)
    return 0;
  ent_window_pictures[ent_window_picture_count] = texture;
  return ent_window_picture_count++;
}

void ent_window_picture(int32_t image, float x, float y, float w, float h,
                        int32_t tint) {
  if (image <= 0 || image >= ent_window_picture_count)
    return;
  Texture2D texture = ent_window_pictures[image];
  DrawTexturePro(texture,
                 (Rectangle){0, 0, (float)texture.width, (float)texture.height},
                 (Rectangle){x, y, w, h}, (Vector2){0, 0}, 0,
                 (Color){(unsigned char)(tint >> 16), (unsigned char)(tint >> 8),
                         (unsigned char)tint, 255});
  // (What `shade` lays on everything, over the picture.)
  if (ent_window_shading_part > 0) {
    Color over = ent_window_shading;
    over.a = (unsigned char)((float)over.a * ent_window_shading_part);
    DrawRectangleRec((Rectangle){x, y, w, h}, over);
  }
}

float ent_window_picture_ratio(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count ||
      !ent_window_pictures[image].height)
    return 1;
  return (float)ent_window_pictures[image].width /
         (float)ent_window_pictures[image].height;
}

// A colour with all of it there (AA ff), and how much of a colour there
// is when `part` of it is laid on: its AA's share of that.
int32_t ent_window_solid(int32_t color) {
  return (int32_t)((uint32_t)color | 0xff000000u);
}

float ent_window_strength(int32_t color, float part) {
  uint32_t alpha = (uint32_t)color >> 24;
  return part * (alpha ? (float)alpha / 255.0f : 1.0f);
}

// From one colour to another: `part` of the way (0 to 1), each of red,
// green, blue and how much of it there is.
int32_t ent_window_blend(int32_t from, int32_t to, float part) {
  if (part <= 0)
    return from;
  if (part >= 1)
    return to;
  uint32_t a = (uint32_t)from, b = (uint32_t)to;
  // (No AA is all of it.)
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

float ent_window_mouse_wheel_x(void) { return GetMouseWheelMoveV().x; }
float ent_window_mouse_wheel_y(void) { return GetMouseWheelMoveV().y; }

// The rectangle drawing is clipped to, if any: set again only where it
// is another, since setting it sends what was drawn so far on its way.
static int ent_window_clipped;
static int ent_window_clip_at[4];

void ent_window_unclip(void) {
  if (ent_window_clipped)
    EndScissorMode();
  ent_window_clipped = 0;
}

void ent_window_clip(float x, float y, float width, float height) {
  // (As big as it is drawn.)
  x *= ent_window_zoom;
  y *= ent_window_zoom;
  width *= ent_window_zoom;
  height *= ent_window_zoom;
  // (All of the window, or more: no clipping.)
  if (x <= 0 && y <= 0 && x + width >= GetScreenWidth() &&
      y + height >= GetScreenHeight()) {
    ent_window_unclip();
    return;
  }
  int at[4] = {(int)x, (int)y, (int)(width + 0.5f), (int)(height + 0.5f)};
  if (at[2] < 0)
    at[2] = 0;
  if (at[3] < 0)
    at[3] = 0;
  if (ent_window_clipped && !memcmp(at, ent_window_clip_at, sizeof at))
    return;
  if (ent_window_clipped)
    EndScissorMode();
  BeginScissorMode(at[0], at[1], at[2], at[3]);
  memcpy(ent_window_clip_at, at, sizeof at);
  ent_window_clipped = 1;
}

void ent_window_screenshot(const ent_text126 *file) {
  ENT_WINDOW_TEXT(name, file);
  // What was drawn so far is still waiting to go to the picture.
  rlDrawRenderBatchActive();
  TakeScreenshot(name);
}
