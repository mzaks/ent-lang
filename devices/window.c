// The window device (devices/window.ent), drawn with raylib. Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#include "ent_extern.h"
#include "ent_files.h"

#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
// (Not windows.h, whose names raylib's clash with.)
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
#elif !defined(__EMSCRIPTEN__)
#include <time.h>
#endif
// Pictures are read with a copy of stb_image of the window's own (static:
// raylib has stb_image's functions too, built without .jpg). On every
// system, a web page's and Windows' too.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

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

// raylib takes its texts terminated. (One of any length is: a 0 comes
// after its bytes.)
#define ENT_WINDOW_TEXT(name, text)                                          \
  char name[sizeof((text)->bytes) + 1];                                      \
  memcpy(name, (text)->bytes, (text)->length);                               \
  name[(text)->length] = 0

// How many pixels a point is across beyond what the window says itself
// (1: no more). A window that is shown through another kind of screen
// (an X window on a Wayland desktop that leaves such windows their own
// pixels) is told that a point is a pixel, on a screen where the desktop
// draws everything else twice as big: all of it would be half as big as
// meant, and its text too small to read. What the desktop says its
// scale is, is then taken for the window's: ENT_WINDOW_SCALE where that
// is set (1 leaves the window as it is told), else GDK_SCALE or
// QT_SCALE_FACTOR, which such desktops set for programs like this one.
static float ent_window_more = 1;

// How many of the picture's pixels a point is across, as the screen says
// (where the window is told of small pixels). On a web page they are in
// ent_window_more, with how big the page has the window (ent_window_fit).
static float ent_window_dpi(void) {
#ifdef __EMSCRIPTEN__
  return 1;
#else
  return GetWindowScaleDPI().x;
#endif
}

#ifdef __EMSCRIPTEN__
// On a web page the window is as big as the browser's tab, and sharp on
// a screen of small pixels (the picture has devicePixelRatio of them to a
// pixel of the page). A window the program lets be made another size has
// the whole tab, in as many points as the tab is pixels across; where the
// tab is smaller than the window's least (`min_width` by `min_height`, at
// least half the size the program asked for: a phone's), a point is less
// than a pixel, so that the window still has its least, smaller. A
// window of one size keeps its points and is made as big as fits in the
// tab. Looked at after every frame: the tab may be made another size, a
// phone turned.
#include <emscripten.h>

// (What raylib says of a picture read before the window opens, too: only
// its errors, as in open_window.)
__attribute__((constructor)) static void ent_window_quiet(void) {
  SetTraceLogLevel(LOG_ERROR);
}

static int32_t ent_window_asked_width, ent_window_asked_height;
static int32_t ent_window_least_width, ent_window_least_height;
static bool ent_window_any_size;
static double ent_window_page_was[3];

// (The page's room for the window: all of the tab but for the keys of a
// touch screen, where they are under it (devices/web.html).)
EM_JS(double, ent_window_page_width, (void), {
  const room = document.querySelector("main");
  return room ? room.clientWidth : window.innerWidth;
});
EM_JS(double, ent_window_page_height, (void), {
  const room = document.querySelector("main");
  return room ? room.clientHeight : window.innerHeight;
});
EM_JS(double, ent_window_page_pixels, (void), {
  return window.devicePixelRatio || 1;
});
EM_JS(void, ent_window_page_canvas, (double width, double height), {
  Module.canvas.style.width = width + "px";
  Module.canvas.style.height = height + "px";
});
// (A window made for a screen that is wider than high, on a phone held
// upright: the page says it has more room turned (devices/web.html).)
EM_JS(void, ent_window_page_turn, (bool turn), {
  const hint = document.getElementById("turn");
  if (hint)
    hint.hidden = !turn;
});

static void ent_window_fit(void) {
  double wide = ent_window_page_width(), high = ent_window_page_height(),
         pixels = ent_window_page_pixels();
  if (wide < 1 || high < 1 || pixels <= 0)
    return;
  if (wide == ent_window_page_was[0] && high == ent_window_page_was[1] &&
      pixels == ent_window_page_was[2])
    return;
  ent_window_page_was[0] = wide;
  ent_window_page_was[1] = high;
  ent_window_page_was[2] = pixels;
  double points_wide, points_high, part;
  if (ent_window_any_size) {
    double least_wide = fmax(ent_window_least_width, ent_window_asked_width / 2.0);
    double least_high = fmax(ent_window_least_height, ent_window_asked_height / 2.0);
    part = fmin(1, fmin(wide / fmax(least_wide, 1), high / fmax(least_high, 1)));
    points_wide = wide / part;
    points_high = high / part;
  } else {
    points_wide = ent_window_asked_width;
    points_high = ent_window_asked_height;
    part = fmin(wide / points_wide, high / points_high);
  }
  ent_window_page_turn(ent_window_asked_width > ent_window_asked_height * 1.15 &&
                       wide < high && wide < 600);
  ent_window_more = (float)(part * pixels);
  int across = (int)(points_wide * part * pixels + 0.5);
  int down = (int)(points_high * part * pixels + 0.5);
  SetWindowSize(across, down);
  ent_window_page_canvas(across / pixels, down / pixels);
}
#endif

static float ent_window_asked_scale(void) {
  const char *names[] = {"ENT_WINDOW_SCALE", "GDK_SCALE", "QT_SCALE_FACTOR"};
  for (int i = 0; i < 3; ++i) {
    const char *said = getenv(names[i]);
    float scale = said ? (float)atof(said) : 0;
    if (scale >= 0.5f && scale <= 8)
      return scale;
  }
  return 1;
}

void ent_window_open_window(int32_t width, int32_t height,
                            const ent_text62 *title, bool resizable) {
  ENT_WINDOW_TEXT(name, title);
#ifdef __EMSCRIPTEN__
  // (In a web page what raylib warns of is WebGL's, nothing a visitor
  // can do about, shown under the canvas: only its errors are.)
  SetTraceLogLevel(LOG_ERROR);
#else
  SetTraceLogLevel(LOG_WARNING);
#endif
  // (In points: on a screen with small pixels the picture has more of
  // them, and everything is as big as on any other.)
#ifdef __EMSCRIPTEN__
  // (Its size the page's, ent_window_fit: raylib is told of none.)
  ent_window_asked_width = width > 0 ? width : 800;
  ent_window_asked_height = height > 0 ? height : 450;
  ent_window_any_size = resizable;
  InitWindow(width, height, name);
#else
  SetConfigFlags(FLAG_WINDOW_HIGHDPI | (resizable ? FLAG_WINDOW_RESIZABLE : 0));
  InitWindow(width, height, name);
#endif
  // (No screen to open it on, one that is off, say: nothing can be drawn.)
  if (!IsWindowReady()) {
    fprintf(stderr, "%s: the window could not be opened: is a display on?\n",
            name);
    exit(1);
  }
#ifdef __EMSCRIPTEN__
  ent_window_fit();
#else
  // (Only where the window is told nothing of small pixels itself.)
  float asked = ent_window_asked_scale();
  if (GetWindowScaleDPI().x <= 1.01f && asked > 1.01f) {
    ent_window_more = asked;
    SetWindowSize((int)(width * asked + 0.5f), (int)(height * asked + 0.5f));
  }
#endif
}

void ent_window_window_limits(int32_t min_width, int32_t min_height) {
#ifdef __EMSCRIPTEN__
  ent_window_least_width = min_width;
  ent_window_least_height = min_height;
  ent_window_page_was[0] = 0;
  ent_window_fit();
#else
  SetWindowMinSize((int)(min_width * ent_window_more + 0.5f),
                   (int)(min_height * ent_window_more + 0.5f));
#endif
}

float ent_window_window_scale(void) {
  return ent_window_dpi() * ent_window_more;
}

void ent_window_resize_window(int32_t width, int32_t height) {
#ifdef __EMSCRIPTEN__
  // (The page has it as big as the tab; the size asked for is the one
  // made to fit in it.)
  ent_window_asked_width = width;
  ent_window_asked_height = height;
  ent_window_page_was[0] = 0;
  ent_window_fit();
#else
  SetWindowSize((int)(width * ent_window_more + 0.5f),
                (int)(height * ent_window_more + 0.5f));
#endif
}

// How big the window is, in points: by the picture that is drawn into,
// which is as big as the window really is (one that the program asked
// another size for may not have got it).
int32_t ent_window_window_width(void) {
  float scale = ent_window_dpi() * ent_window_more;
  return (int32_t)((float)GetRenderWidth() / (scale > 0 ? scale : 1) + 0.5f);
}
int32_t ent_window_window_height(void) {
  float scale = ent_window_dpi() * ent_window_more;
  return (int32_t)((float)GetRenderHeight() / (scale > 0 ? scale : 1) + 0.5f);
}

#ifdef __EMSCRIPTEN__
// (On a web page: the browser's full screen, which a page may ask for
// only just after a key or a touch, as the program's asking is.)
EM_JS(void, ent_window_page_full, (bool on), {
  const now = !!document.fullscreenElement;
  if (on && !now && document.documentElement.requestFullscreen)
    document.documentElement.requestFullscreen().catch(() => {});
  else if (!on && now && document.exitFullscreen)
    document.exitFullscreen().catch(() => {});
});
#endif

// Without a border, as big as the screen it is on: no other video mode,
// and it is as it was when it is no longer asked for.
void ent_window_fill_screen(bool on) {
#ifdef __EMSCRIPTEN__
  static bool held = false;
  if (on != held)
    ent_window_page_full(on);
  held = on;
#else
  if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) != on)
    ToggleBorderlessWindowed();
#endif
}

// The key that closes the window: Escape, or none where the program
// wants that key for itself.
// (Whether the program has Escape: a menu takes it while it is open.)
static bool ent_window_escape_owned;
void ent_window_own_escape(bool own) {
  ent_window_escape_owned = own;
  SetExitKey(own ? KEY_NULL : KEY_ESCAPE);
}

#ifdef __EMSCRIPTEN__
// In a web page the browser shows the frames: the program waits for the
// browser's next one (requestAnimationFrame), and for more of them where
// it asks for fewer frames a second than the screen shows, so that a
// frame is as long as `Window.fps` says on a screen of 60 Hz or of 144.
// It gives the browser its turn so (Asyncify), where raylib would wait
// in EndDrawing without (busy) and then 12 ms more in WindowShouldClose.
// The page is never closed by the program's window.

EM_ASYNC_JS(void, ent_window_next_frame, (double seconds), {
  if (!Module.entFrameAt)
    Module.entFrameAt = performance.now();
  Module.entFrameAt += seconds * 1000;
  const now = performance.now();
  // (Late by more than a frame: from now on.)
  if (Module.entFrameAt < now - seconds * 1000)
    Module.entFrameAt = now;
  do {
    await new Promise(requestAnimationFrame);
  } while (performance.now() < Module.entFrameAt - 2);
});

static int32_t ent_window_fps = 60;
// (How long the last frame was, waiting for the browser too: raylib's
// GetFrameTime counts from BeginDrawing to EndDrawing only.)
static double ent_window_frame_was = 1.0 / 60, ent_window_frame_end = 0;
bool ent_window_should_close(void) { return false; }
float ent_window_frame_seconds(void) { return (float)ent_window_frame_was; }
#else
// (Once a frame went without a picture (`still_frame`), its length is
// measured here: raylib's GetFrameTime counts the frames it drew.)
static int ent_window_any_still;
static double ent_window_frame_was = 1.0 / 60, ent_window_frame_end = 0;
bool ent_window_should_close(void) { return WindowShouldClose(); }
float ent_window_frame_seconds(void) {
  return ent_window_any_still ? (float)ent_window_frame_was : GetFrameTime();
}
#endif

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <objc/message.h>
#include <objc/runtime.h>
// (AppKit is the window's already, through GLFW; the program is linked
// against it and the Objective-C runtime from here, which every Mac has.)
__asm__(".linker_option \"-lobjc\"\n"
        ".linker_option \"-framework\", \"AppKit\"");
typedef struct {
  double x, y;
} ent_window_ns_point;
static SEL ent_window_sel(const char *name) { return sel_registerName(name); }
static id ent_window_send(id to, const char *name) {
  return ((id(*)(id, SEL))objc_msgSend)(to, ent_window_sel(name));
}
static id ent_window_class(const char *name) { return (id)objc_getClass(name); }
static id ent_window_ns_text(const char *s) {
  return ((id(*)(id, SEL, const char *))objc_msgSend)(
      ent_window_class("NSString"), ent_window_sel("stringWithUTF8String:"), s);
}
#endif

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
// A window made bigger or smaller with the mouse, on a Mac: AppKit
// follows the mouse in a loop of its own until the button is let go,
// within the events taken in at a frame's end, and the program draws
// nothing meanwhile (the picture it had is stretched). So the events are
// taken in on a stack of their own (a fiber, on the same thread): where
// the window's size changes within that loop, the program is let go on
// with its frames, one for each size, until it ends one (the picture
// shown at once); then the loop goes on.
#include <sys/mman.h>

// From one stack to the other: the registers a function keeps are kept
// on the stack left, its top put in *from; then `to`'s are taken back.
void ent_window_fiber_switch(void **from, void *to);
#if defined(__aarch64__)
__asm__(".text\n.p2align 2\n.globl _ent_window_fiber_switch\n"
        "_ent_window_fiber_switch:\n"
        "  sub sp, sp, #160\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mov x9, sp\n  str x9, [x0]\n  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #160\n  ret\n");
#define ENT_WINDOW_FIBER_KEPT 160
#elif defined(__x86_64__)
__asm__(".text\n.globl _ent_window_fiber_switch\n"
        "_ent_window_fiber_switch:\n"
        "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n"
        "  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
        "  popq %rbx\n  popq %rbp\n  ret\n");
#define ENT_WINDOW_FIBER_KEPT 48
#endif

#ifdef ENT_WINDOW_FIBER_KEPT
static void *ent_window_main_top, *ent_window_events_top;
// What the events' stack is asked to do (1 end the frame: EndDrawing;
// 2 take the events in), whether it waits within AppKit's loop for the
// program's next frame, and whether it is on it now.
static int ent_window_events_job;
static bool ent_window_events_parked, ent_window_on_events;

static void ent_window_events_main(void) {
  for (;;) {
    ent_window_on_events = true;
    if (ent_window_events_job == 1)
      EndDrawing();
    else
      PollInputEvents();
    ent_window_events_job = 0;
    ent_window_on_events = false;
    ent_window_fiber_switch(&ent_window_events_top, ent_window_main_top);
  }
}

// A new size within AppKit's loop (NSWindowDidResizeNotification, after
// GLFW's, which tells raylib): the program draws a frame for it.
static void ent_window_resized_live(id self, SEL command, id note) {
  (void)self, (void)command, (void)note;
  if (!ent_window_on_events)
    return;
  ent_window_events_parked = true;
  ent_window_on_events = false;
  ent_window_fiber_switch(&ent_window_events_top, ent_window_main_top);
  ent_window_on_events = true;
  ent_window_events_parked = false;
}

static bool ent_window_events_ready(void) {
  static int made; // 0 not yet, 1 made, -1 could not be
  if (made == 0) {
    made = -1;
    size_t size = 8u << 20, page = 16384;
    char *stack = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    id window = (id)GetWindowHandle();
    if (stack != MAP_FAILED && window) {
      mprotect(stack, page, PROT_NONE); // (a guard at its end)
      // Its top as if it had been left by ent_window_fiber_switch, to go
      // on in ent_window_events_main.
      char *top = stack + size - 64;
      top = (char *)((uintptr_t)top & ~(uintptr_t)15);
#if defined(__aarch64__)
      top -= ENT_WINDOW_FIBER_KEPT;
      memset(top, 0, ENT_WINDOW_FIBER_KEPT);
      ((void **)top)[11] = (void *)ent_window_events_main; // x30
#else
      top -= 16; // (as if called: once it is taken, the top is 8 off even)
      *(void **)top = (void *)ent_window_events_main;
      top -= ENT_WINDOW_FIBER_KEPT;
      memset(top, 0, ENT_WINDOW_FIBER_KEPT);
#endif
      ent_window_events_top = top;
      Class watcher = objc_getClass("EntWindowResizeWatcher");
      if (!watcher) {
        watcher = objc_allocateClassPair(objc_getClass("NSObject"),
                                         "EntWindowResizeWatcher", 0);
        class_addMethod(watcher, ent_window_sel("resized:"),
                        (IMP)ent_window_resized_live, "v@:@");
        objc_registerClassPair(watcher);
      }
      id watching = ent_window_send(ent_window_send((id)watcher, "alloc"), "init");
      ((void (*)(id, SEL, id, SEL, id, id))objc_msgSend)(
          ent_window_send(ent_window_class("NSNotificationCenter"), "defaultCenter"),
          ent_window_sel("addObserver:selector:name:object:"), watching,
          ent_window_sel("resized:"), ent_window_ns_text("NSWindowDidResizeNotification"),
          window);
      made = 1;
    }
  }
  return made == 1;
}

// The frame's end (1) or the events (2) on the events' stack. Where it
// waits within AppKit's loop, the frame just drawn is shown first.
static bool ent_window_events_turn(int job, bool drawn) {
  if (getenv("ENT_WINDOW_LIVE") && strcmp(getenv("ENT_WINDOW_LIVE"), "no") == 0)
    return false;
  if (!ent_window_events_parked) {
    if (!ent_window_events_ready())
      return false;
    ent_window_events_job = job;
  } else if (drawn) {
    rlDrawRenderBatchActive();
    SwapScreenBuffer();
  }
  ent_window_fiber_switch(&ent_window_main_top, ent_window_events_top);
  return true;
}
#endif
#endif

// Everything is drawn so many times as big as it says (`Window.zoom`).
static float ent_window_zoom = 1;
static float ent_window_zoom_now(void) { return ent_window_zoom; }

// (Between begin_frame and end_frame: a picture is being drawn.)
static bool ent_window_drawing;

void ent_window_begin_frame(int32_t background, int32_t fps, float zoom) {
#ifdef __EMSCRIPTEN__
  ent_window_fps = fps;
#else
  static int32_t held = -1;
  if (fps != held) {
    SetTargetFPS(fps);
    held = fps;
  }
#endif
  BeginDrawing();
  ent_window_drawing = true;
  ent_window_shading_part = 0;
  ClearBackground(ent_window_color(background));
  // (With what a point is across beyond what the window says.)
  ent_window_zoom = (zoom > 0 ? zoom : 1) * ent_window_more;
  rlPushMatrix();
  rlScalef(ent_window_zoom, ent_window_zoom, 1);
}

void ent_window_end_frame(void) {
  ent_window_shift(0, 0);
  ent_window_unclip();
  rlPopMatrix();
#ifdef ENT_WINDOW_FIBER_KEPT
  if (!ent_window_events_turn(1, true))
    EndDrawing();
#else
  EndDrawing();
#endif
  ent_window_drawing = false;
#ifdef __EMSCRIPTEN__
  ent_window_next_frame(ent_window_fps > 0 ? 1.0 / ent_window_fps : 0);
  ent_window_fit();
#endif
  double now = GetTime();
  if (ent_window_frame_end > 0)
    ent_window_frame_was = now - ent_window_frame_end;
  ent_window_frame_end = now;
}

// A frame without a picture: no drawing and no swap, so the window shows
// the picture it had; what happened is taken in (as EndDrawing does),
// and the frame lasts as long as `fps` says (on a web page: until the
// browser's next frame, as many as `fps` asks for).
void ent_window_still_frame(int32_t fps) {
#ifdef __EMSCRIPTEN__
  ent_window_fps = fps;
  PollInputEvents();
  ent_window_next_frame(fps > 0 ? 1.0 / fps : 0);
  ent_window_fit();
#else
  ent_window_any_still = 1;
#ifdef ENT_WINDOW_FIBER_KEPT
  bool parked = ent_window_events_parked;
  if (!ent_window_events_turn(2, false))
    PollInputEvents();
  if (parked)
    fps = 0; // (AppKit's loop sets the pace while the window is resized.)
#else
  PollInputEvents();
#endif
  if (fps > 0) {
    double until = ent_window_frame_end + 1.0 / fps;
    double now = GetTime();
    // (Asleep all the while: raylib's WaitTime spins the end of it.)
    if (until > now) {
#if defined(_WIN32)
      Sleep((unsigned long)((until - now) * 1000));
#else
      double wait = until - now;
      struct timespec span = {(time_t)wait,
                              (long)((wait - (double)(time_t)wait) * 1e9)};
      nanosleep(&span, NULL);
#endif
    }
  }
#endif
  double now = GetTime();
  if (ent_window_frame_end > 0)
    ent_window_frame_was = now - ent_window_frame_end;
  ent_window_frame_end = now;
}

// For a device that draws in three dimensions within the frame (the
// scene device, devices/scene.c): the window's own way of drawing put
// aside (its points, its zoom, a shift or a clip), and taken up again.
// (rlgl lays a matrix that is pushed on every point it is given, a 3D
// one too: so it is popped while the scene is drawn.)
void ent_window_aside(void) {
  ent_window_shift(0, 0);
  ent_window_unclip();
  rlDrawRenderBatchActive();
  rlPopMatrix();
}
void ent_window_again(void) {
  rlPushMatrix();
  rlScalef(ent_window_zoom, ent_window_zoom, 1);
}
// How many of the picture's pixels a point is across (with the zoom).
float ent_window_pixels(void) {
  return ent_window_dpi() * ent_window_zoom;
}

float ent_window_mouse_x(void) { return (float)GetMouseX() / ent_window_more; }
float ent_window_mouse_y(void) { return (float)GetMouseY() / ent_window_more; }
static bool ent_window_button(int32_t button) {
#ifdef __EMSCRIPTEN__
  // (A touch is the left button. It puts the pointer where it is in the
  // frame it starts: down from the frame after, as a mouse that was
  // moved there first, so that what is under the pointer (a layout's
  // box that is pressed) is known before.)
  return IsMouseButtonDown(button) && !IsMouseButtonPressed(button);
#else
  return IsMouseButtonDown(button);
#endif
}

// The buttons as the system means them, taken in once a frame
// (ent_window_mouse_clicks, which `begin` asks first): on a Mac a left
// press with Control down is the secondary button's, for as long as it
// is held; a press of the left button within the system's double-click
// time and a few points of the one before goes on a run of clicks.
static bool ent_window_left_was, ent_window_right_was;
static bool ent_window_control_press, ent_window_secondary_now;
static int32_t ent_window_click_run;
static double ent_window_click_at;
static float ent_window_click_x, ent_window_click_y;

static double ent_window_double_click(void);

// A press and its release both between two frames (a quick tap on a
// trackpad, a click while a frame takes long): raylib, which reads the
// buttons as they are at a frame, would see neither. On a Mac each left
// press is also counted as AppKit hands it to the window (a local
// monitor of its events), with the system's count of clicks in a row; a
// press seen so and gone by the frame is the left button down for that
// frame (`ent_window_left_tapped`), let go at the next. And right
// presses so: a secondary click all the same.
static bool ent_window_left_tapped;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
static int ent_window_presses, ent_window_right_presses;
static long ent_window_press_count;
static void ent_window_watch_presses(void) {
  static id monitor;
  if (monitor || !GetWindowHandle())
    return;
  id (^seen)(id) = ^id(id event) {
    if (((id(*)(id, SEL))objc_msgSend)(event, sel_registerName("window")) ==
        (id)GetWindowHandle()) {
      // (NSEventTypeRightMouseDown is 3.)
      if (((unsigned long (*)(id, SEL))objc_msgSend)(event, sel_registerName("type")) == 3)
        ent_window_right_presses++;
      else {
        ent_window_presses++;
        ent_window_press_count =
            ((long (*)(id, SEL))objc_msgSend)(event, sel_registerName("clickCount"));
      }
    }
    return event;
  };
  // (NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown.)
  monitor = ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)(
      ent_window_class("NSEvent"),
      ent_window_sel("addLocalMonitorForEventsMatchingMask:handler:"), (1ULL << 1) | (1ULL << 3),
      (id)seen);
  if (monitor)
    ent_window_send(monitor, "retain");
}
#endif

bool ent_window_mouse_down(int32_t button) {
  if (button == 0)
    return (ent_window_button(0) || ent_window_left_tapped) && !ent_window_control_press;
  if (button == 1)
    return ent_window_button(1) ||
           ((ent_window_button(0) || ent_window_left_tapped) && ent_window_control_press);
  return ent_window_button(button);
}

int32_t ent_window_mouse_clicks(void) {
  bool left = ent_window_button(0), right = ent_window_button(1);
  bool left_new = left && !ent_window_left_was;
  long system_count = 0;
  ent_window_left_tapped = false;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  ent_window_watch_presses();
  if (ent_window_presses > 0) {
    // (A press since the last frame that raylib did not see begin: down
    // and up between them, or up and down again.)
    if (!left_new) {
      left_new = true;
      if (!left) {
        ent_window_left_tapped = true;
        left = true;
      }
    }
    system_count = ent_window_press_count;
    ent_window_presses = 0;
  }
#endif
  bool right_new = right && !ent_window_right_was;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  if (ent_window_right_presses > 0)
    right_new = true;
  ent_window_right_presses = 0;
#endif
  ent_window_left_was = left;
  ent_window_right_was = right;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  if (left_new && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)))
    ent_window_control_press = true;
#endif
  if (!left)
    ent_window_control_press = false;
  ent_window_secondary_now = right_new || (left_new && ent_window_control_press);
  if (!left_new || ent_window_control_press)
    return 0;
  double now = GetTime();
  float x = ent_window_mouse_x(), y = ent_window_mouse_y();
  if (system_count > 0)
    ent_window_click_run = (int32_t)system_count;
  else if (ent_window_click_run > 0 &&
      now - ent_window_click_at <= ent_window_double_click() &&
      fabsf(x - ent_window_click_x) <= 4 && fabsf(y - ent_window_click_y) <= 4)
    ent_window_click_run++;
  else
    ent_window_click_run = 1;
  ent_window_click_at = now;
  ent_window_click_x = x;
  ent_window_click_y = y;
  return ent_window_click_run;
}

bool ent_window_mouse_secondary(void) { return ent_window_secondary_now; }
bool ent_window_key_down(int32_t key) { return IsKeyDown(key); }
bool ent_window_key_pressed(int32_t key) {
  return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

// The characters typed: raylib queues them as they come, and gives them
// one at a time until it has none (0). All are taken, those that do not
// fit counted and lost, so that none is left for the frame after.
int32_t ent_window_take_typed(int32_t *count, int32_t capacity,
                              int32_t *code) {
  int32_t typed = 0;
  for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
    if (typed < capacity)
      code[typed] = c;
    typed++;
  }
  *count = typed < capacity ? typed : capacity;
  return typed;
}

const ent_text *ent_window_clipboard(void) {
  const char *held = GetClipboardText();
  size_t length = held ? strlen(held) : 0;
  if (!length || length > UINT32_MAX)
    return NULL;
  ent_text *text = ent_text_new((uint32_t)length);
  memcpy(text->bytes, held, length);
  return text;
}

void ent_window_set_clipboard(const ent_text *t) {
  uint32_t length = t ? t->length : 0;
  char *bytes = malloc((size_t)length + 1);
  if (!bytes)
    return;
  memcpy(bytes, length ? t->bytes : "", length);
  bytes[length] = 0;
  SetClipboardText(bytes);
  free(bytes);
}

void ent_window_rect(float x, float y, float width, float height,
                     int32_t color) {
  DrawRectangleRec((Rectangle){x, y, width, height}, ent_window_color(color));
}

// Many rectangles and circles with one call. Where the graphics card
// takes a picture for each of many things in one go (OpenGL 3.3), the
// buffer's columns are handed to it as they are: a corner of a square is
// placed by a row's `x`, `y`, `w`, `h`, and a row that is `round` shows
// the disc inside its square, with a soft edge.
static const char *const ent_window_shapes_vertex =
    "#version 330\n"
    "in vec2 corner;\n"
    "in float sx;\nin float sy;\nin float sw;\nin float sh;\n"
    "in vec4 scolor;\nin float sround;\n"
    "uniform mat4 mvp;\nuniform vec4 shade;\n"
    "out vec2 at;\nout vec4 tint;\nout float rounded;\n"
    "void main() {\n"
    "  at = corner;\n"
    "  rounded = sround;\n"
    // (The bytes of 0xAARRGGBB as they lie; no AA is all of it.)
    "  vec4 c = scolor.bgra;\n"
    "  if (c.a == 0.0) c.a = 1.0;\n"
    "  c.rgb += (shade.rgb - c.rgb) * shade.a;\n"
    "  tint = c;\n"
    "  gl_Position = mvp * vec4(sx + corner.x * sw, sy + corner.y * sh,\n"
    "                           0.0, 1.0);\n"
    "}\n";
static const char *const ent_window_shapes_fragment =
    "#version 330\n"
    "in vec2 at;\nin vec4 tint;\nin float rounded;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  float cover = 1.0;\n"
    "  if (rounded > 0.5) {\n"
    "    float far = length(at - vec2(0.5)) * 2.0;\n"
    "    cover = clamp((1.0 - far) / max(fwidth(far), 1e-6), 0.0, 1.0);\n"
    "  }\n"
    "  color = vec4(tint.rgb, tint.a * cover);\n"
    "}\n";

static bool ent_window_shapes_at_once(int32_t count, const float *x,
                                      const float *y, const float *w,
                                      const float *h, const int32_t *color,
                                      const bool *round) {
  enum { COLUMNS = 6 };
  static const char *const names[COLUMNS] = {"sx", "sy", "sw",
                                             "sh", "scolor", "sround"};
  static const int sizes[COLUMNS] = {4, 4, 4, 4, 4, 1};
  static int tried, room;
  static unsigned int shader, array, corners, columns[COLUMNS];
  static int mvp, shade;
  if (!tried) {
    tried = 1;
    int version = rlGetVersion();
    if (version == RL_OPENGL_33 || version == RL_OPENGL_43)
      shader = rlLoadShaderProgram(ent_window_shapes_vertex,
                                   ent_window_shapes_fragment);
    if (shader) {
      mvp = rlGetLocationUniform(shader, "mvp");
      shade = rlGetLocationUniform(shader, "shade");
    }
  }
  if (!shader)
    return false;
  if (count > room) {
    // More rows than there was room for on the card: room for these,
    // and half as many again.
    if (array) {
      rlUnloadVertexArray(array);
      rlUnloadVertexBuffer(corners);
      for (int i = 0; i < COLUMNS; i++)
        rlUnloadVertexBuffer(columns[i]);
    }
    room = count + count / 2 + 1024;
    static const float square[12] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
    array = rlLoadVertexArray();
    rlEnableVertexArray(array);
    corners = rlLoadVertexBuffer(square, sizeof(square), false);
    unsigned int place = (unsigned int)rlGetLocationAttrib(shader, "corner");
    rlSetVertexAttribute(place, 2, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(place);
    for (int i = 0; i < COLUMNS; i++) {
      columns[i] = rlLoadVertexBuffer(NULL, room * sizes[i], true);
      int found = rlGetLocationAttrib(shader, names[i]);
      if (found < 0)
        continue;
      place = (unsigned int)found;
      if (i < 4)
        rlSetVertexAttribute(place, 1, RL_FLOAT, false, 0, 0);
      else if (i == 4)
        rlSetVertexAttribute(place, 4, RL_UNSIGNED_BYTE, true, 0, 0);
      else
        rlSetVertexAttribute(place, 1, RL_UNSIGNED_BYTE, false, 0, 0);
      rlEnableVertexAttribute(place);
      rlSetVertexAttributeDivisor(place, 1);
    }
    rlDisableVertexArray();
  }
  // What was drawn before lies under these.
  rlDrawRenderBatchActive();
  const void *const data[COLUMNS] = {x, y, w, h, color, round};
  for (int i = 0; i < COLUMNS; i++)
    rlUpdateVertexBuffer(columns[i], data[i], count * sizes[i], 0);
  rlEnableShader(shader);
  rlSetUniformMatrix(mvp, MatrixMultiply(MatrixMultiply(rlGetMatrixTransform(),
                                                       rlGetMatrixModelview()),
                                         rlGetMatrixProjection()));
  float part = ent_window_shading_part * (float)ent_window_shading.a / 255.0f;
  float shading[4] = {(float)ent_window_shading.r / 255.0f,
                      (float)ent_window_shading.g / 255.0f,
                      (float)ent_window_shading.b / 255.0f, part};
  rlSetUniform(shade, shading, RL_SHADER_UNIFORM_VEC4, 1);
  rlEnableVertexArray(array);
  rlDrawVertexArrayInstanced(0, 6, count);
  rlDisableVertexArray();
  rlDisableShader();
  return true;
}

// Elsewhere each is one quad of a picture of a white disc, a circle all
// of it and a rectangle its middle, so that both are of one batch and lie
// in the order they came in.
void ent_window_draw_shapes(int32_t count, const float *x, const float *y,
                            const float *w, const float *h,
                            const int32_t *color, const bool *round) {
  enum { SIDE = 128, AT_ONCE = 1024 };
  static Texture2D disc;
  if (count <= 0 ||
      ent_window_shapes_at_once(count, x, y, w, h, color, round))
    return;
  if (!disc.id) {
    Image image = GenImageColor(SIDE, SIDE, BLANK);
    Color *pixels = image.data;
    for (int row = 0; row < SIDE; row++)
      for (int column = 0; column < SIDE; column++) {
        // (How much of the pixel the disc covers: a soft edge one pixel
        // wide.)
        float dx = (float)column + 0.5f - SIDE / 2.0f;
        float dy = (float)row + 0.5f - SIDE / 2.0f;
        float inside = SIDE / 2.0f - sqrtf(dx * dx + dy * dy);
        float cover = inside < 0 ? 0 : inside > 1 ? 1 : inside;
        pixels[row * SIDE + column] =
            (Color){255, 255, 255, (unsigned char)(cover * 255.0f)};
      }
    disc = LoadTextureFromImage(image);
    UnloadImage(image);
    GenTextureMipmaps(&disc);
    SetTextureFilter(disc, TEXTURE_FILTER_TRILINEAR);
  }
  rlSetTexture(disc.id);
  for (int32_t from = 0; from < count; from += AT_ONCE) {
    int32_t to = from + AT_ONCE < count ? from + AT_ONCE : count;
    rlCheckRenderBatchLimit(4 * (to - from));
    rlBegin(RL_QUADS);
    rlNormal3f(0.0f, 0.0f, 1.0f);
    for (int32_t i = from; i < to; i++) {
      Color c = ent_window_color(color[i]);
      float low = round[i] ? 0.0f : 0.5f, high = round[i] ? 1.0f : 0.5f;
      rlColor4ub(c.r, c.g, c.b, c.a);
      rlTexCoord2f(low, low);
      rlVertex2f(x[i], y[i]);
      rlTexCoord2f(low, high);
      rlVertex2f(x[i], y[i] + h[i]);
      rlTexCoord2f(high, high);
      rlVertex2f(x[i] + w[i], y[i] + h[i]);
      rlTexCoord2f(high, low);
      rlVertex2f(x[i] + w[i], y[i]);
    }
    rlEnd();
  }
  rlSetTexture(0);
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

// A filled shape of four corners, as two triangles, each drawn both ways
// round: the corners may be given clockwise or not.
void ent_window_quad(float x0, float y0, float x1, float y1, float x2,
                     float y2, float x3, float y3, int32_t rgb) {
  Color color = ent_window_color(rgb);
  Vector2 a = {x0, y0}, b = {x1, y1}, c = {x2, y2}, d = {x3, y3};
  DrawTriangle(a, b, c, color);
  DrawTriangle(a, c, b, color);
  DrawTriangle(a, c, d, color);
  DrawTriangle(a, d, c, color);
}

// The font of `label` and `paragraph`: one of the machine's own, the
// first there is of those most machines have (ENT_WINDOW_FONT names
// another file, or "none" for the window library's own letters of a few
// pixels, which is also what is drawn where none of them is there). Read
// at the size it is shown at, so that it is sharp at any: a few sizes
// are kept.
#define ENT_WINDOW_PLAIN_SIZES 8
static struct {
  int looked;
  const char *file;
  int pixels[ENT_WINDOW_PLAIN_SIZES];
  Font at[ENT_WINDOW_PLAIN_SIZES];
  int next;
  // (One the program named, `plain_font`: its bytes, wherever they came
  // from, the program itself too.)
  struct ent_file held;
  char kind[8];
} ent_window_plain_face;

static const char *ent_window_plain_file(void) {
  if (ent_window_plain_face.looked)
    return ent_window_plain_face.file;
  ent_window_plain_face.looked = 1;
  const char *asked = getenv("ENT_WINDOW_FONT");
  if (asked && *asked) {
    if (strcmp(asked, "none") != 0 && FileExists(asked))
      ent_window_plain_face.file = asked;
    return ent_window_plain_face.file;
  }
  static const char *known[] = {
      // (On a web page, the one `ent build --web` put in it.)
      "/ent/plain.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/Library/Fonts/Arial.ttf",
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
  };
  for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    if (FileExists(known[i])) {
      ent_window_plain_face.file = known[i];
      break;
    }
  return ent_window_plain_face.file;
}

static float ent_window_zoom_now(void);

// The letters a font is read with: those of Latin-1, as the library's
// own font has them, the marks texts are written with beyond it (… – —
// ‘ ’ “ ” • € ™), and the signs keys and arrows are written with (← ↑ →
// ↓ ↩ ⇥ ⇧ ⌃ ⌘ ⌥ ⌫ ⌦ ⎋ ✓ ✗), where the font has them.
#define ENT_WINDOW_LETTERS 220
static int ent_window_letters(int *letters) {
  static const int more[] = {0x2026, 0x2013, 0x2014, 0x2018, 0x2019,
                             0x201c, 0x201d, 0x2022, 0x20ac, 0x2122,
                             0x2190, 0x2191, 0x2192, 0x2193, 0x21a9,
                             0x21e5, 0x21e7, 0x2303, 0x2318, 0x2325,
                             0x232b, 0x2326, 0x238b, 0x2713, 0x2717};
  enum { latin = 191, all = latin + sizeof more / sizeof more[0] };
  _Static_assert(all <= ENT_WINDOW_LETTERS, "room for the letters");
  for (int i = 0; i < all; ++i)
    letters[i] = i < 95 ? 32 + i : i < latin ? 160 + (i - 95) : more[i - latin];
  return all;
}
// (A font that lacks some of them is no news: raylib's warning of it is
// not said.)
static Font ent_window_font_load(const char *kind, const unsigned char *bytes,
                                 int size, const char *file, int pixels,
                                 int *letters, int all) {
  SetTraceLogLevel(LOG_ERROR);
  Font made = bytes ? LoadFontFromMemory(kind, bytes, size, pixels, letters, all)
                    : LoadFontEx(file, pixels, letters, all);
#ifndef __EMSCRIPTEN__
  if (IsWindowReady())
    SetTraceLogLevel(LOG_WARNING);
#endif
  return made;
}

static Font ent_window_plain(float size) {
  const char *file = ent_window_plain_file();
  if (!file || !IsWindowReady())
    return GetFontDefault();
  int pixels = (int)(size * ent_window_zoom_now() * ent_window_dpi() + 0.5f);
  if (pixels < 8)
    pixels = 8;
  for (int i = 0; i < ENT_WINDOW_PLAIN_SIZES; ++i)
    if (ent_window_plain_face.pixels[i] == pixels)
      return ent_window_plain_face.at[i];
  int letters[ENT_WINDOW_LETTERS];
  int all = ent_window_letters(letters);
  Font made = ent_window_font_load(ent_window_plain_face.kind,
                                   ent_window_plain_face.held.bytes,
                                   (int)ent_window_plain_face.held.size, file,
                                   pixels, letters, all);
  if (!made.texture.id || !made.glyphCount) {
    ent_window_plain_face.file = 0;
    return GetFontDefault();
  }
  SetTextureFilter(made.texture, TEXTURE_FILTER_BILINEAR);
  int slot = ent_window_plain_face.next;
  ent_window_plain_face.next = (slot + 1) % ENT_WINDOW_PLAIN_SIZES;
  if (ent_window_plain_face.pixels[slot]) {
    // (What was written with the letters given up may not be on the
    // screen yet: it is put there first.)
    rlDrawRenderBatchActive();
    UnloadFont(ent_window_plain_face.at[slot]);
  }
  ent_window_plain_face.at[slot] = made;
  ent_window_plain_face.pixels[slot] = pixels;
  return made;
}

// (The library's own letters stand a tenth of their size apart; a font
// of the machine's has its own distances.)
static float ent_window_plain_apart(float size) {
  return ent_window_plain_file() && IsWindowReady() ? 0 : (float)((int)size / 10);
}
static void ent_window_plain_draw(const char *line, float x, float y, float size,
                                  Color color) {
  size = (float)(int)size;
  DrawTextEx(ent_window_plain(size), line, (Vector2){(float)(int)x, (float)(int)y},
             size, ent_window_plain_apart(size), color);
}
static float ent_window_plain_width(const char *line, float size) {
  size = (float)(int)size;
  if (!*line)
    return 0;
  return MeasureTextEx(ent_window_plain(size), line, size,
                       ent_window_plain_apart(size)).x;
}

void ent_window_label(const ent_text126 *text, float x, float y, float size,
                      int32_t color) {
  ENT_WINDOW_TEXT(line, text);
  ent_window_plain_draw(line, x, y, size, ent_window_color(color));
}

void ent_window_label_turned(const ent_text126 *text, float x, float y,
                             float size, float degrees, int32_t color) {
  ENT_WINDOW_TEXT(line, text);
  size = (float)(int)size;
  DrawTextPro(ent_window_plain(size), line, (Vector2){x, y}, (Vector2){0, 0},
              degrees, size, ent_window_plain_apart(size),
              ent_window_color(color));
}

float ent_window_text_width(const ent_text126 *text, float size) {
  ENT_WINDOW_TEXT(line, text);
  return (float)ent_window_plain_width(line, size);
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
    if (length && ent_window_plain_width(trial, size) > width) {
      if (draw)
        ent_window_plain_draw(line, x, (y + lines * step), size, color);
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
      ent_window_plain_draw(line, x, (y + lines * step), size, color);
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
    int width = ent_window_plain_width(word, size);
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
// read at. (A few sizes of each are kept, and a size is the next four
// pixels up: text that grows as a view is zoomed is then not read anew
// at every step.)
#define ENT_WINDOW_SIZES 8
static struct {
  // (The file's bytes, kept: letters of another size are made of them.)
  struct ent_file file;
  char kind[8];
  Font at[ENT_WINDOW_SIZES];
  int pixels[ENT_WINDOW_SIZES];
  int next;
  int size, read, broken;
} ent_window_faces[16];

// A font is read when it is first asked for with the window open: its
// number is given before, so a program can ask for it in `world`.
static void ent_window_font_read(int32_t font) {
  if (ent_window_faces[font].read || !IsWindowReady())
    return;
  ent_window_faces[font].read = 1;
  int letters[ENT_WINDOW_LETTERS];
  int all = ent_window_letters(letters);
  Font made = ent_window_font_load(ent_window_faces[font].kind,
                                   ent_window_faces[font].file.bytes,
                                   (int)ent_window_faces[font].file.size, 0,
                                   ent_window_faces[font].size, letters, all);
  // (One that cannot be read after all: the window's own.)
  if (made.texture.id == 0) {
    ent_window_faces[font].broken = 1;
    return;
  }
  SetTextureFilter(made.texture, TEXTURE_FILTER_BILINEAR);
  ent_window_fonts[font] = made;
}

static int ent_window_font_known(int32_t font) {
  if (font <= 0 || font >= ent_window_font_count)
    return 0;
  ent_window_font_read(font);
  return ent_window_faces[font].read && !ent_window_faces[font].broken;
}

// Font 0, the window's own (and one that cannot be read): that of
// `label` and `paragraph`, where there is one; else the window library's
// own letters of a few pixels.
static int ent_window_plain_there(void) {
  return ent_window_plain_file() && IsWindowReady();
}

static Font ent_window_font_shown(int32_t font, float size) {
  if (!ent_window_font_known(font))
    return ent_window_plain_there() ? ent_window_plain(size) : GetFontDefault();
  int pixels = (int)(size * ent_window_zoom * ent_window_dpi() + 0.5f);
  if (pixels <= ent_window_fonts[font].baseSize)
    return ent_window_fonts[font];
  pixels = (pixels + 3) / 4 * 4;
  if (pixels > 512)
    pixels = 512;
  for (int i = 0; i < ENT_WINDOW_SIZES; ++i)
    if (ent_window_faces[font].pixels[i] == pixels)
      return ent_window_faces[font].at[i];
  int letters[ENT_WINDOW_LETTERS];
  int all = ent_window_letters(letters);
  Font made = ent_window_font_load(ent_window_faces[font].kind,
                                   ent_window_faces[font].file.bytes,
                                   (int)ent_window_faces[font].file.size, 0,
                                   pixels, letters, all);
  if (made.texture.id == 0)
    return ent_window_fonts[font];
  SetTextureFilter(made.texture, TEXTURE_FILTER_BILINEAR);
  int slot = ent_window_faces[font].next;
  ent_window_faces[font].next = (slot + 1) % ENT_WINDOW_SIZES;
  if (ent_window_faces[font].pixels[slot]) {
    // (What was written with the letters given up may not be on the
    // screen yet: it is put there first, while they are still there.)
    rlDrawRenderBatchActive();
    UnloadFont(ent_window_faces[font].at[slot]);
  }
  ent_window_faces[font].at[slot] = made;
  ent_window_faces[font].pixels[slot] = pixels;
  return made;
}

static Font ent_window_font_of(int32_t font, float size) {
  if (!ent_window_font_known(font))
    return ent_window_plain_there() ? ent_window_plain(size) : GetFontDefault();
  return ent_window_fonts[font];
}

// Whether a file is a font all of which is there: it starts with what
// says which kind of font it is, lists its tables, each of which lies in
// the file and adds up to the sum the list has for it, and has the
// tables a font cannot be without. (What reads a font's letters trusts
// the file: of one that is cut short or damaged it reads past the end.)
static uint32_t ent_window_long(const unsigned char *at) {
  return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 |
         (uint32_t)at[2] << 8 | at[3];
}

static int ent_window_font_whole(const char *name, const unsigned char *all,
                                 size_t size) {
  const char *wrong = 0;
  char tag[5] = {0};
  uint32_t start = 0;
  if (!all || size < 12)
    wrong = "is too short";
  // (Of a collection of fonts, the first is the one that is used.)
  if (!wrong && !memcmp(all, "ttcf", 4)) {
    start = size >= 16 ? ent_window_long(all + 12) : 0;
    if (start < 16 || start > (uint32_t)size - 12)
      wrong = "is a collection without a first font";
  }
  if (!wrong && !memcmp(all + start, "typ1", 4))
    return 1;
  if (!wrong && memcmp(all + start, "\0\1\0\0", 4) &&
      memcmp(all + start, "OTTO", 4) && memcmp(all + start, "true", 4))
    wrong = "does not start as a font does";
  uint32_t tables = wrong ? 0 : all[start + 4] << 8 | all[start + 5];
  if (!wrong && (!tables || (uint64_t)start + 12 + 16ull * tables >
                                (uint64_t)size))
    wrong = "is cut short in its list of tables";
  // (cmap, head, hhea, hmtx, maxp; the letters' shapes one way or other.)
  int has = 0;
  static const char *const needed[] = {"cmap", "head", "hhea", "hmtx",
                                       "maxp", "glyf", "loca", "CFF ",
                                       "CFF2"};
  for (uint32_t i = 0; i < tables && !wrong; ++i) {
    const unsigned char *entry = all + start + 12 + 16 * i;
    uint32_t sum = ent_window_long(entry + 4);
    uint32_t from = ent_window_long(entry + 8);
    uint32_t length = ent_window_long(entry + 12);
    memcpy(tag, entry, 4);
    if ((uint64_t)from + length > (uint64_t)size) {
      wrong = "is cut short";
      break;
    }
    for (int k = 0; k < 9; ++k)
      if (!memcmp(tag, needed[k], 4))
        has |= 1 << k;
    // (The table's bytes as numbers of four, the last filled with
    // zeros; in `head`, without the sum over the whole file it holds.)
    int head = !memcmp(tag, "head", 4);
    uint32_t added = 0;
    for (uint32_t at = 0; at < length; at += 4) {
      unsigned char four[4] = {0};
      memcpy(four, all + from + at, length - at < 4 ? length - at : 4);
      if (!(head && at == 8))
        added += ent_window_long(four);
    }
    if (added != sum)
      wrong = "is damaged";
    else if (head && (length < 54 ||
                      ent_window_long(all + from + 12) != 0x5F0F3CF5u))
      wrong = "is damaged";
    if (!wrong)
      tag[0] = 0;
  }
  if (!wrong && ((has & 31) != 31 ||
                 !((has & 96) == 96 || (has & 384))))
    wrong = "lacks a table a font needs";
  if (wrong) {
    fprintf(stderr, "ent: the font '%s' %s%s%s%s\n", name, wrong,
            tag[0] ? " (table '" : "", tag, tag[0] ? "')" : "");
    return 0;
  }
  return 1;
}

int32_t ent_window_font(const ent_text126 *file, int32_t size) {
  ENT_WINDOW_TEXT(named, file);
  const char *kind = GetFileExtension(named);
  struct ent_file held;
  if (ent_window_font_count >= 16 || !kind ||
      strlen(kind) >= sizeof ent_window_faces[0].kind ||
      !ent_file_open(named, &held))
    return 0;
  // A file that is no font, or a damaged one, is none.
  if (IsFileExtension(named, ".ttf;.otf") &&
      !ent_window_font_whole(named, held.bytes, held.size)) {
    ent_file_close(&held);
    return 0;
  }
  ent_window_faces[ent_window_font_count].file = held;
  strcpy(ent_window_faces[ent_window_font_count].kind, kind);
  ent_window_faces[ent_window_font_count].size = size;
  return ent_window_font_count++;
}

// The font of `label` and `paragraph`, named by the program: found as
// `font` finds one (next to the program, or in it). Where there is no
// such font, or it is damaged, they keep the one they had.
void ent_window_plain_font(const ent_text126 *file) {
  ENT_WINDOW_TEXT(named, file);
  const char *kind = GetFileExtension(named);
  struct ent_file held;
  if (!kind || strlen(kind) >= sizeof ent_window_plain_face.kind ||
      !ent_file_open(named, &held))
    return;
  if (!ent_window_font_whole(named, held.bytes, held.size)) {
    ent_file_close(&held);
    return;
  }
  for (int i = 0; i < ENT_WINDOW_PLAIN_SIZES; ++i)
    if (ent_window_plain_face.pixels[i]) {
      UnloadFont(ent_window_plain_face.at[i]);
      ent_window_plain_face.pixels[i] = 0;
    }
  if (ent_window_plain_face.held.bytes)
    ent_file_close(&ent_window_plain_face.held);
  ent_window_plain_face.held = held;
  strcpy(ent_window_plain_face.kind, kind);
  ent_window_plain_face.looked = 1;
  ent_window_plain_face.file = "(the program's)";
  ent_window_plain_face.next = 0;
}

// (raylib draws its own font with a tenth of the size between letters.)
static float ent_window_spacing(int32_t font, float size, float spacing) {
  if (!ent_window_font_known(font) && !ent_window_plain_there())
    spacing += size < 10 ? 1 : size / 10;
  return spacing;
}

static float ent_window_measure(const char *text, int32_t font, float size,
                                float spacing) {
  if (!*text)
    return 0;
  return MeasureTextEx(ent_window_font_of(font, size), text, size,
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
  // (A line is as long as the text, where nothing breaks it.)
  int room = (int)strlen(text) + 2;
  char *line = malloc(2 * (size_t)room), *trial = line + room;
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
    for (; *at; at++)
      line[length++] = *at == '\n' ? ' ' : *at;
    ENT_WINDOW_END_LINE();
  } else if (breaks == 1) {
    for (;; at++) {
      if (*at == '\n' || !*at) {
        ENT_WINDOW_END_LINE();
        if (!*at)
          break;
      } else {
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
      int both = length ? snprintf(trial, room, "%s %.*s", line, word, at)
                        : snprintf(trial, room, "%.*s", word, at);
      if (length &&
          ent_window_measure(trial, font, size, spacing) > width + 0.01f) {
        ENT_WINDOW_END_LINE();
        length = snprintf(line, room, "%.*s", word, at);
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
  free(line);
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

void ent_window_write(const ent_text *text, float x, float y, float width,
                      int32_t font, float size, float spacing, float line,
                      int32_t breaks, int32_t align, int32_t color) {
  const char *all = text->bytes;
  struct ent_window_writing w = {x, y, width, size, spacing,
                                 line > 0 ? line : size, font, align,
                                 ent_window_color(color)};
  ent_window_set(all, width, font, size, spacing, breaks, 0,
                 ent_window_write_line, &w);
}

int32_t ent_window_text_lines(const ent_text *text, float width,
                              int32_t font, float size, float spacing,
                              int32_t breaks) {
  const char *all = text->bytes;
  return ent_window_set(all, width, font, size, spacing, breaks, 0, 0, 0);
}

float ent_window_text_extent(const ent_text *text, int32_t font,
                             float size, float spacing, int32_t breaks) {
  const char *all = text->bytes;
  float widest = 0;
  // (With all the room it wants: broken only where it must be.)
  ent_window_set(all, 1.0e30f, font, size, spacing, breaks, &widest, 0, 0);
  return widest;
}

float ent_window_text_least(const ent_text *text, int32_t font, float size,
                            float spacing, int32_t breaks) {
  const char *all = text->bytes;
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

// Pictures read from files: read at once, and given to the screen when
// first drawn, so a program can ask for one in `world`, before the
// window is open. One that is higher than a graphics card takes (a sheet
// of many little pictures, one under the other) is given to it in strips
// of so many rows, and what is drawn of it is put together of them.
#define ENT_WINDOW_STRIP 4096
#define ENT_WINDOW_STRIPS 32
struct ent_window_sheet {
  Texture2D strip[ENT_WINDOW_STRIPS];
  int strips, width, height;
};
// (As many pictures as a game with a picture for every thing reads.)
#define ENT_WINDOW_PICTURES 2048
static struct ent_window_sheet ent_window_pictures[ENT_WINDOW_PICTURES];
static Image ent_window_images[ENT_WINDOW_PICTURES];
static int ent_window_picture_count = 1;
// The numbers of pictures let go (`picture_free`), given again first.
static int32_t ent_window_picture_let_go[ENT_WINDOW_PICTURES];
static int ent_window_picture_let_gone = 0;

// A number for a picture read: one let go, or the next; 0 where there is
// none left.
static int32_t ent_window_picture_keep(Image image) {
  if (!image.data)
    return 0;
  int32_t number;
  if (ent_window_picture_let_gone > 0)
    number = ent_window_picture_let_go[--ent_window_picture_let_gone];
  else if (ent_window_picture_count < ENT_WINDOW_PICTURES)
    number = ent_window_picture_count++;
  else {
    UnloadImage(image);
    return 0;
  }
  ent_window_images[number] = image;
  ent_window_pictures[number].strips = 0;
  return number;
}

// A picture from bytes as a file has them (.png, .jpg, .gif's first
// picture, .bmp, .tga, .psd): stb_image tells the kind by how they start.
static Image ent_window_picture_read(const unsigned char *bytes, size_t size) {
  int width = 0, height = 0, channels = 0;
  if (size == 0 || size > 0x7fffffff)
    return (Image){0};
  unsigned char *pixels = stbi_load_from_memory(bytes, (int)size, &width,
                                                &height, &channels, 4);
  if (!pixels)
    return (Image){0};
  // (stb_image's memory is malloc's, which UnloadImage gives back.)
  return (Image){.data = pixels, .width = width, .height = height,
                 .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
}

int32_t ent_window_picture_load(const ent_text126 *file) {
  ENT_WINDOW_TEXT(named, file);
  struct ent_file held;
  if (!ent_file_open(named, &held))
    return 0;
  Image image = ent_window_picture_read((const unsigned char *)held.bytes,
                                        (size_t)held.size);
  // (Kinds stb_image does not read, and raylib does: .qoi, .dds.)
  const char *kind = GetFileExtension(named);
  if (!image.data && kind)
    image = LoadImageFromMemory(kind, (const unsigned char *)held.bytes,
                                (int)held.size);
  ent_file_close(&held);
  return ent_window_picture_keep(image);
}

// A picture from its bytes (as an answer over the network brings them).
int32_t ent_window_picture_from(const ent_text *bytes) {
  if (!bytes)
    return 0;
  return ent_window_picture_keep(ent_window_picture_read(
      (const unsigned char *)bytes->bytes, bytes->length));
}

// A picture let go: its memory, and the graphics card's, given back, and
// its number given to the next picture read. Drawing it after draws
// nothing (until the number is given again).
void ent_window_picture_free(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count)
    return;
  for (int i = 0; i < ent_window_picture_let_gone; i++)
    if (ent_window_picture_let_go[i] == image)
      return;
  struct ent_window_sheet *sheet = &ent_window_pictures[image];
  for (int i = 0; i < sheet->strips; i++)
    if (sheet->strip[i].id)
      UnloadTexture(sheet->strip[i]);
  sheet->strips = 0;
  sheet->width = sheet->height = 0;
  if (ent_window_images[image].data)
    UnloadImage(ent_window_images[image]);
  ent_window_images[image] = (Image){0};
  ent_window_picture_let_go[ent_window_picture_let_gone++] = image;
}

// The picture as the screen has it: given to it now, if it was not.
static struct ent_window_sheet *ent_window_sheet_of(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count)
    return 0;
  struct ent_window_sheet *sheet = &ent_window_pictures[image];
  Image *read = &ent_window_images[image];
  if (read->data) {
    sheet->width = read->width;
    sheet->height = read->height;
    sheet->strips = 0;
    for (int top = 0;
         top < read->height && sheet->strips < ENT_WINDOW_STRIPS;
         top += ENT_WINDOW_STRIP) {
      int rows = read->height - top;
      if (rows > ENT_WINDOW_STRIP)
        rows = ENT_WINDOW_STRIP;
      if (rows == read->height) {
        sheet->strip[sheet->strips++] = LoadTextureFromImage(*read);
      } else {
        Image cut = ImageFromImage(
            *read, (Rectangle){0, (float)top, (float)read->width, (float)rows});
        sheet->strip[sheet->strips++] = LoadTextureFromImage(cut);
        UnloadImage(cut);
      }
    }
    UnloadImage(*read);
    read->data = 0;
  }
  return sheet->strips > 0 && sheet->strip[0].id != 0 ? sheet : 0;
}

void ent_window_picture_part(int32_t image, float from_x, float from_y,
                             float from_w, float from_h, float x, float y,
                             float w, float h, int32_t tint) {
  struct ent_window_sheet *sheet = ent_window_sheet_of(image);
  if (!sheet || from_h <= 0 || from_w <= 0)
    return;
  // (A tint may let what is under the picture show through, as any
  // colour: 0xAARRGGBB.)
  unsigned char alpha = (unsigned char)((uint32_t)tint >> 24);
  Color color = {(unsigned char)(tint >> 16), (unsigned char)(tint >> 8),
                 (unsigned char)tint, alpha ? alpha : 255};
  for (int i = 0; i < sheet->strips; i++) {
    float top = (float)(i * ENT_WINDOW_STRIP);
    float first = from_y > top ? from_y : top;
    float end = top + (float)sheet->strip[i].height;
    if (from_y + from_h < end)
      end = from_y + from_h;
    if (end <= first)
      continue;
    DrawTexturePro(sheet->strip[i],
                   (Rectangle){from_x, first - top, from_w, end - first},
                   (Rectangle){x, y + (first - from_y) * h / from_h, w,
                               (end - first) * h / from_h},
                   (Vector2){0, 0}, 0, color);
  }
  // (What `shade` lays on everything, over the picture.)
  if (ent_window_shading_part > 0) {
    Color over = ent_window_shading;
    over.a = (unsigned char)((float)over.a * ent_window_shading_part);
    DrawRectangleRec((Rectangle){x, y, w, h}, over);
  }
}

void ent_window_picture(int32_t image, float x, float y, float w, float h,
                        int32_t tint) {
  struct ent_window_sheet *sheet = ent_window_sheet_of(image);
  if (sheet)
    ent_window_picture_part(image, 0, 0, (float)sheet->width,
                            (float)sheet->height, x, y, w, h, tint);
}

// The program's own picture: its pixels go to the graphics card as they
// are each time, in a texture that is made anew when the size changes.
void ent_window_draw_pixels(int32_t count, const int32_t *color, int32_t width,
                            int32_t height, float x, float y, float w,
                            float h) {
  static Texture2D texture;
  static Color *pixels;
  if (width <= 0 || height <= 0 || count < width * height)
    return;
  if (texture.id == 0 || texture.width != width || texture.height != height) {
    if (texture.id != 0)
      UnloadTexture(texture);
    free(pixels);
    pixels = malloc(sizeof(Color) * (size_t)width * (size_t)height);
    Image image = GenImageColor(width, height, BLACK);
    texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
  }
  if (!pixels || texture.id == 0)
    return;
  for (int32_t i = 0; i < width * height; i++)
    pixels[i] = (Color){(unsigned char)(color[i] >> 16),
                        (unsigned char)(color[i] >> 8),
                        (unsigned char)color[i], 255};
  UpdateTexture(texture, pixels);
  DrawTexturePro(texture, (Rectangle){0, 0, (float)width, (float)height},
                 (Rectangle){x, y, w, h}, (Vector2){0, 0}, 0, WHITE);
}

// How many pixels wide and high a picture is (0 for none).
int32_t ent_window_picture_width(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count)
    return 0;
  return ent_window_images[image].data ? ent_window_images[image].width
                                       : ent_window_pictures[image].width;
}
int32_t ent_window_picture_height(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count)
    return 0;
  return ent_window_images[image].data ? ent_window_images[image].height
                                       : ent_window_pictures[image].height;
}

float ent_window_picture_ratio(int32_t image) {
  if (image <= 0 || image >= ent_window_picture_count)
    return 1;
  // (As wide and high as it was read, given to the screen or not yet.)
  int width = ent_window_images[image].data ? ent_window_images[image].width
                                            : ent_window_pictures[image].width;
  int height = ent_window_images[image].data
                   ? ent_window_images[image].height
                   : ent_window_pictures[image].height;
  return height ? (float)width / (float)height : 1;
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
float ent_window_mouse_dx(void) { return GetMouseDelta().x / ent_window_more; }
float ent_window_mouse_dy(void) { return GetMouseDelta().y / ent_window_more; }
void ent_window_grab_mouse(bool grab) {
  if (grab)
    DisableCursor();
  else
    EnableCursor();
}

// The paths of all the files dropped when take_dropped last found some,
// a line each (dropped_paths).
static char *ent_window_dropped_all;
static size_t ent_window_dropped_length;

// The last of the files dropped on the window since it was last asked.
int32_t ent_window_take_dropped(int32_t *count, int32_t capacity,
                                uint8_t *value) {
  *count = 0;
  if (!IsFileDropped())
    return 0;
  FilePathList dropped = LoadDroppedFiles();
  int32_t length = 0;
  if (dropped.count > 0) {
    const char *path = dropped.paths[dropped.count - 1];
    size_t n = strlen(path);
    length = n > INT32_MAX ? INT32_MAX : (int32_t)n;
    int32_t kept = length < capacity ? length : capacity;
    memcpy(value, path, (size_t)kept);
    *count = kept;
    size_t all = 0;
    for (unsigned i = 0; i < dropped.count; i++)
      all += strlen(dropped.paths[i]) + 1;
    char *joined = malloc(all);
    if (joined) {
      size_t at = 0;
      for (unsigned i = 0; i < dropped.count; i++) {
        size_t m = strlen(dropped.paths[i]);
        memcpy(joined + at, dropped.paths[i], m);
        joined[at + m] = '\n';
        at += m + 1;
      }
      free(ent_window_dropped_all);
      ent_window_dropped_all = joined;
      ent_window_dropped_length = all;
    }
  }
  UnloadDroppedFiles(dropped);
  return length;
}

const ent_text *ent_window_dropped_paths(void) {
  size_t length = ent_window_dropped_length;
  if (!length || length > UINT32_MAX)
    return NULL;
  ent_text *text = ent_text_new((uint32_t)length);
  memcpy(text->bytes, ent_window_dropped_all, length);
  return text;
}

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
  // (Cut to the window before it is made whole pixels: a clip that does
  // not limit one way (a layout's box that clips only up and down) comes
  // as one of thousands of millions of points across, more pixels than
  // an int holds where a point is three of them.)
  float right = fminf(x + width, (float)GetScreenWidth());
  float bottom = fminf(y + height, (float)GetScreenHeight());
  x = fmaxf(x, 0);
  y = fmaxf(y, 0);
  int at[4] = {(int)x, (int)y, (int)(fmaxf(right - x, 0) + 0.5f),
               (int)(fmaxf(bottom - y, 0) + 0.5f)};
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
  // (Not TakeScreenshot: raylib 6.0 scales the size it reads by the
  // screen's scale once more on a Retina Mac, where it is in pixels
  // already, and the picture comes out twice as big with the window in
  // a corner. The pixels drawn, as they are, are read here.)
  Image picture = LoadImageFromScreen();
  ExportImage(picture, name);
  UnloadImage(picture);
}

// ---------------------------------------------------------------------
// What systems say differently: the key shortcuts are made with, the
// double-click time, the pointer's shape, and a menu at the pointer.

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
// (The Objective-C runtime's helpers are near the top.)
#elif defined(_WIN32)
// (Not windows.h, whose names raylib's clash with.)
__declspec(dllimport) unsigned int __stdcall GetDoubleClickTime(void);
__declspec(dllimport) void *__stdcall CreatePopupMenu(void);
__declspec(dllimport) int __stdcall AppendMenuW(void *menu, unsigned int flags,
                                                unsigned long long id,
                                                const unsigned short *text);
__declspec(dllimport) int __stdcall TrackPopupMenu(void *menu, unsigned int flags,
                                                   int x, int y, int reserved,
                                                   void *window, const void *rect);
__declspec(dllimport) int __stdcall DestroyMenu(void *menu);
__declspec(dllimport) int __stdcall GetCursorPos(void *point);
__declspec(dllimport) int __stdcall PostMessageW(void *window, unsigned int message,
                                                 unsigned long long w, long long l);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int page,
                                                        unsigned long flags,
                                                        const char *from, int n,
                                                        unsigned short *to,
                                                        int room);
#endif

static double ent_window_double_click(void) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  static double interval;
  if (interval <= 0)
    interval = ((double (*)(id, SEL))objc_msgSend)(
        ent_window_class("NSEvent"), ent_window_sel("doubleClickInterval"));
  return interval > 0 ? interval : 0.5;
#elif defined(_WIN32)
  return GetDoubleClickTime() / 1000.0;
#else
  return 0.5;
#endif
}

bool ent_window_command_down(void) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#elif defined(__EMSCRIPTEN__)
  // (A page does not know whether the machine is a Mac: either.)
  return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
         IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#else
  return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

const ent_text *ent_window_command_name(void) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  const char *name = "Cmd";
#else
  const char *name = "Ctrl";
#endif
  size_t n = strlen(name);
  ent_text *made = ent_text_new((uint32_t)n);
  memcpy(made->bytes, name, n);
  return made;
}

bool ent_window_word_modifier_down(void) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
#else
  return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

void ent_window_pointer_shape(int32_t shape) {
  static int32_t shown = 0;
  if (shape < 0 || shape > 10)
    shape = 0;
  if (shape != shown && IsWindowReady()) {
    SetMouseCursor(shape);
    shown = shape;
  }
}

// The items of a menu: a line each; "-" a separator, "~" before one that
// cannot be chosen. (Split in place: `lines` point into `held`.)
#define ENT_WINDOW_MENU_ITEMS 64
typedef struct {
  char *held;
  const char *lines[ENT_WINDOW_MENU_ITEMS];
  bool dim[ENT_WINDOW_MENU_ITEMS], separator[ENT_WINDOW_MENU_ITEMS];
  int n;
} ent_window_menu_items;

static bool ent_window_menu_split(const ent_text *items, ent_window_menu_items *m) {
  uint32_t n = items ? items->length : 0;
  m->n = 0;
  m->held = malloc((size_t)n + 1);
  if (!m->held)
    return false;
  if (n)
    memcpy(m->held, items->bytes, n);
  m->held[n] = 0;
  char *at = m->held;
  while (*at && m->n < ENT_WINDOW_MENU_ITEMS) {
    char *end = strchr(at, '\n');
    if (end)
      *end = 0;
    size_t length = strlen(at);
    if (length && at[length - 1] == '\r')
      at[length - 1] = 0;
    m->dim[m->n] = at[0] == '~';
    m->separator[m->n] = strcmp(at, "-") == 0;
    m->lines[m->n] = m->dim[m->n] ? at + 1 : at;
    m->n++;
    if (!end)
      break;
    at = end + 1;
  }
  return true;
}

// A menu the window draws itself, where the system has none to give (or
// ENT_WINDOW_MENU=drawn asks for it): over the picture that is shown,
// until an item is chosen or it is let go (Escape, a click outside). It
// is drawn in pictures of its own; the program's picture is drawn again
// after (a lazy window's too: the program paints the frame after).
static int32_t ent_window_menu_drawn(ent_window_menu_items *m) {
  if (!IsWindowReady() || m->n == 0)
    return -1;
  float more = ent_window_more, size = 14, pad = 6, row = 22, gap = 9;
  float w = 0, h = 2 * pad;
  for (int i = 0; i < m->n; ++i) {
    if (!m->separator[i]) {
      float wide = ent_window_plain_width(m->lines[i], size);
      if (wide > w)
        w = wide;
    }
    h += m->separator[i] ? gap : row;
  }
  w += 2 * 14 + 2 * pad;
  if (w < 120)
    w = 120;
  float room_w = GetScreenWidth() / more, room_h = GetScreenHeight() / more;
  float x = GetMouseX() / more, y = GetMouseY() / more;
  if (x + w > room_w)
    x = x - w > 0 ? x - w : room_w - w;
  if (y + h > room_h)
    y = y - h > 0 ? y - h : room_h - h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  bool was_drawing = ent_window_drawing;
  if (was_drawing) {
    ent_window_shift(0, 0);
    ent_window_unclip();
    rlPopMatrix();
    EndDrawing();
  }
  SetExitKey(KEY_NULL);
  int32_t chosen = -1, hot = -1;
  double opened = GetTime();
  bool done = false;
  while (!done && !WindowShouldClose()) {
    BeginDrawing();
    rlPushMatrix();
    rlScalef(more, more, 1);
    DrawRectangleRounded((Rectangle){x + 1, y + 2, w, h}, 0.04f, 6,
                         (Color){0, 0, 0, 90});
    DrawRectangleRounded((Rectangle){x, y, w, h}, 0.04f, 6,
                         (Color){246, 246, 246, 255});
    DrawRectangleRoundedLines((Rectangle){x, y, w, h}, 0.04f, 6,
                              (Color){190, 190, 190, 255});
    float at = y + pad;
    for (int i = 0; i < m->n; ++i) {
      if (m->separator[i]) {
        DrawRectangle((int)(x + pad), (int)(at + gap / 2), (int)(w - 2 * pad), 1,
                      (Color){210, 210, 210, 255});
        at += gap;
        continue;
      }
      if (i == hot)
        DrawRectangleRounded((Rectangle){x + pad / 2, at, w - pad, row}, 0.2f, 4,
                             (Color){10, 100, 220, 255});
      Color ink = m->dim[i] ? (Color){170, 170, 170, 255}
                  : i == hot ? (Color){255, 255, 255, 255}
                             : (Color){30, 30, 30, 255};
      ent_window_plain_draw(m->lines[i], x + pad + 14, at + (row - size) / 2, size,
                            ink);
      at += row;
    }
    rlPopMatrix();
    EndDrawing();
    // (What was done, from the input EndDrawing took in.)
    float px = GetMouseX() / more, py = GetMouseY() / more;
    int under = -1;
    if (px >= x && px < x + w) {
      float top = y + pad;
      for (int i = 0; i < m->n; ++i) {
        float tall = m->separator[i] ? gap : row;
        if (py >= top && py < top + tall && !m->separator[i] && !m->dim[i])
          under = i;
        top += tall;
      }
    }
    Vector2 moved = GetMouseDelta();
    if (moved.x != 0 || moved.y != 0)
      hot = under;
    bool inside = px >= x && px < x + w && py >= y && py < y + h;
    if (IsKeyPressed(KEY_ESCAPE)) {
      done = true;
    } else if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && hot >= 0) {
      chosen = hot;
      done = true;
    } else if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN) ||
               IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
      int step = IsKeyDown(KEY_DOWN) ? 1 : -1;
      for (int k = 1; k <= m->n; ++k) {
        int i = ((hot < 0 && step < 0 ? m->n : hot) + step * k + 2 * m->n) % m->n;
        if (!m->separator[i] && !m->dim[i]) {
          hot = i;
          break;
        }
      }
    } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
               IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      if (!inside)
        done = true;
    } else if ((IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
                IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) &&
               under >= 0 && GetTime() - opened > 0.2) {
      // (A press that opened it and was let go on an item, or a click.)
      chosen = under;
      done = true;
    }
  }
  SetExitKey(ent_window_escape_owned ? KEY_NULL : KEY_ESCAPE);
  if (was_drawing) {
    BeginDrawing();
    rlPushMatrix();
    rlScalef(ent_window_zoom, ent_window_zoom, 1);
  }
  return chosen;
}

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
// The system's menu (NSMenu): its items' target an object of a class made
// here, whose `pick:` keeps the number of the item chosen.
static int32_t ent_window_menu_pick;
// The menu bar's item chosen last, by the menus it is in ("File/Open").
static char ent_window_bar_pick[1024];
static void ent_window_bar_picked(id self, SEL command, id sender) {
  (void)self, (void)command;
  id path = ent_window_send(sender, "representedObject");
  const char *said =
      path ? ((const char *(*)(id, SEL))objc_msgSend)(path, ent_window_sel("UTF8String"))
           : 0;
  snprintf(ent_window_bar_pick, sizeof ent_window_bar_pick, "%s", said ? said : "");
}
static void ent_window_menu_picked(id self, SEL command, id sender) {
  (void)self, (void)command;
  ent_window_menu_pick =
      (int32_t)((long (*)(id, SEL))objc_msgSend)(sender, ent_window_sel("tag"));
}
static id ent_window_menu_target(void) {
  static id target;
  if (!target) {
    Class made = objc_getClass("EntWindowMenuTarget");
    if (!made) {
      made = objc_allocateClassPair(objc_getClass("NSObject"),
                                    "EntWindowMenuTarget", 0);
      class_addMethod(made, ent_window_sel("pick:"), (IMP)ent_window_menu_picked,
                      "v@:@");
      class_addMethod(made, ent_window_sel("barPick:"), (IMP)ent_window_bar_picked,
                      "v@:@");
      objc_registerClassPair(made);
    }
    target = ent_window_send(ent_window_send((id)made, "alloc"), "init");
  }
  return target;
}
// (The menu took the button's letting go: the window is told of it, as if
// it had seen it, or it would hold the button down.)
static void ent_window_menu_let_go(id window, bool left, bool right) {
  id app = ent_window_send(ent_window_class("NSApplication"), "sharedApplication");
  ent_window_ns_point at = ((ent_window_ns_point(*)(id, SEL))objc_msgSend)(
      window, ent_window_sel("mouseLocationOutsideOfEventStream"));
  long number = ((long (*)(id, SEL))objc_msgSend)(window, ent_window_sel("windowNumber"));
  double now = ((double (*)(id, SEL))objc_msgSend)(
      ent_window_send(ent_window_class("NSProcessInfo"), "processInfo"),
      ent_window_sel("systemUptime"));
  for (int b = 0; b < 2; ++b) {
    if (!(b == 0 ? left : right))
      continue;
    unsigned long type = b == 0 ? 2 : 4; // NSEventTypeLeftMouseUp, RightMouseUp
    id up = ((id(*)(id, SEL, unsigned long, ent_window_ns_point, unsigned long, double,
                    long, id, long, long, float))objc_msgSend)(
        ent_window_class("NSEvent"),
        ent_window_sel("mouseEventWithType:location:modifierFlags:timestamp:"
                       "windowNumber:context:eventNumber:clickCount:pressure:"),
        type, at, 0, now, number, (id)0, 0, 1, 0.0f);
    if (up)
      ((void (*)(id, SEL, id, signed char))objc_msgSend)(
          app, ent_window_sel("postEvent:atStart:"), up, 0);
  }
}
static int32_t ent_window_menu_native(ent_window_menu_items *m) {
  id window = (id)GetWindowHandle();
  if (!window || m->n == 0)
    return -1;
  id target = ent_window_menu_target();
  id menu = ((id(*)(id, SEL, id))objc_msgSend)(
      ent_window_send(ent_window_class("NSMenu"), "alloc"),
      ent_window_sel("initWithTitle:"), ent_window_ns_text(""));
  ((void (*)(id, SEL, signed char))objc_msgSend)(
      menu, ent_window_sel("setAutoenablesItems:"), 0);
  for (int i = 0; i < m->n; ++i) {
    if (m->separator[i]) {
      ((void (*)(id, SEL, id))objc_msgSend)(
          menu, ent_window_sel("addItem:"),
          ent_window_send(ent_window_class("NSMenuItem"), "separatorItem"));
      continue;
    }
    id item = ((id(*)(id, SEL, id, SEL, id))objc_msgSend)(
        ent_window_send(ent_window_class("NSMenuItem"), "alloc"),
        ent_window_sel("initWithTitle:action:keyEquivalent:"),
        ent_window_ns_text(m->lines[i]), ent_window_sel("pick:"),
        ent_window_ns_text(""));
    ((void (*)(id, SEL, id))objc_msgSend)(item, ent_window_sel("setTarget:"), target);
    ((void (*)(id, SEL, long))objc_msgSend)(item, ent_window_sel("setTag:"), i);
    ((void (*)(id, SEL, signed char))objc_msgSend)(
        item, ent_window_sel("setEnabled:"), !m->dim[i]);
    ((void (*)(id, SEL, id))objc_msgSend)(menu, ent_window_sel("addItem:"), item);
    ent_window_send(item, "release");
  }
  id view = ent_window_send(window, "contentView");
  ent_window_ns_point at = ((ent_window_ns_point(*)(id, SEL))objc_msgSend)(
      window, ent_window_sel("mouseLocationOutsideOfEventStream"));
  at = ((ent_window_ns_point(*)(id, SEL, ent_window_ns_point, id))objc_msgSend)(
      view, ent_window_sel("convertPoint:fromView:"), at, (id)0);
  bool left = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
  bool right = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
  ent_window_menu_pick = -1;
  ((signed char (*)(id, SEL, id, ent_window_ns_point, id))objc_msgSend)(
      menu, ent_window_sel("popUpMenuPositioningItem:atLocation:inView:"), (id)0,
      at, view);
  ent_window_send(menu, "release");
  ent_window_menu_let_go(window, left, right);
  return ent_window_menu_pick;
}
#elif defined(_WIN32)
// The system's menu (TrackPopupMenu), its items numbered from 1 (0: none).
static int32_t ent_window_menu_native(ent_window_menu_items *m) {
  void *window = GetWindowHandle();
  void *menu = window && m->n ? CreatePopupMenu() : 0;
  if (!menu)
    return -1;
  for (int i = 0; i < m->n; ++i) {
    unsigned short wide[256];
    if (m->separator[i]) {
      AppendMenuW(menu, 0x800, 0, 0); // MF_SEPARATOR
      continue;
    }
    int n = MultiByteToWideChar(65001, 0, m->lines[i], -1, wide, 255);
    wide[n > 0 ? n : 0] = 0;
    AppendMenuW(menu, m->dim[i] ? 1 : 0, (unsigned long long)(i + 1), wide);
  }
  struct { long x, y; } at;
  GetCursorPos(&at);
  bool left = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
  bool right = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
  // TPM_RETURNCMD | TPM_RIGHTBUTTON
  int picked = TrackPopupMenu(menu, 0x100 | 0x2, at.x, at.y, 0, window, 0);
  DestroyMenu(menu);
  long long where = ((long long)(GetMouseY() & 0xffff) << 16) | (GetMouseX() & 0xffff);
  if (left)
    PostMessageW(window, 0x0202, 0, where); // WM_LBUTTONUP
  if (right)
    PostMessageW(window, 0x0205, 0, where); // WM_RBUTTONUP
  return picked > 0 ? picked - 1 : -1;
}
#endif

int32_t ent_window_context_menu(const ent_text *items) {
  ent_window_menu_items m;
  if (!ent_window_menu_split(items, &m))
    return -1;
  int32_t chosen = -1;
  const char *asked = getenv("ENT_WINDOW_MENU");
  bool drawn = asked && strcmp(asked, "drawn") == 0;
#if defined(__EMSCRIPTEN__)
  (void)drawn;
#elif defined(__APPLE__)
  if (drawn || !IsWindowReady()) {
    chosen = ent_window_menu_drawn(&m);
  } else {
    chosen = ent_window_menu_native(&m);
  }
#elif defined(_WIN32)
  chosen = drawn ? ent_window_menu_drawn(&m) : ent_window_menu_native(&m);
#else
  chosen = ent_window_menu_drawn(&m);
#endif
  free(m.held);
  return chosen;
}

// ---------------------------------------------------------------------
// The menu bar, a folder chosen, an alert, and the window's title.

// A text as C has them (malloc'd; none: "").
static char *ent_window_c_text(const ent_text *t) {
  uint32_t n = t ? t->length : 0;
  char *c = malloc((size_t)n + 1);
  if (!c)
    return 0;
  if (n)
    memcpy(c, t->bytes, n);
  c[n] = 0;
  return c;
}
static const ent_text *ent_window_ent_text(const char *c) {
  size_t n = c ? strlen(c) : 0;
  ent_text *made = ent_text_new((uint32_t)n);
  if (n)
    memcpy(made->bytes, c, n);
  return made;
}

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
static id ent_window_with_text(id to, const char *name, const char *t) {
  return ((id(*)(id, SEL, id))objc_msgSend)(to, ent_window_sel(name),
                                             ent_window_ns_text(t));
}
static long ent_window_count(id menu) {
  return ((long (*)(id, SEL))objc_msgSend)(menu, ent_window_sel("numberOfItems"));
}
static id ent_window_new_menu(const char *title) {
  id menu = ent_window_with_text(ent_window_send(ent_window_class("NSMenu"), "alloc"),
                                 "initWithTitle:", title);
  ((void (*)(id, SEL, signed char))objc_msgSend)(
      menu, ent_window_sel("setAutoenablesItems:"), 0);
  return menu;
}
#endif

bool ent_window_menu_bar(const ent_text *title, const ent_text *items) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  if (!IsWindowReady())
    return false;
  id app = ent_window_send(ent_window_class("NSApplication"), "sharedApplication");
  id bar = ent_window_send(app, "mainMenu");
  if (!bar)
    return false;
  char *name = ent_window_c_text(title), *held = ent_window_c_text(items);
  if (!name || !held) {
    free(name);
    free(held);
    return false;
  }
  void *pool = objc_autoreleasePoolPush();
  id target = ent_window_menu_target();
  // In place of the menu of this title, else before Window (the last).
  long at = ((long (*)(id, SEL, id))objc_msgSend)(
      bar, ent_window_sel("indexOfItemWithTitle:"), ent_window_ns_text(name));
  if (at >= 0) {
    ((void (*)(id, SEL, long))objc_msgSend)(bar, ent_window_sel("removeItemAtIndex:"), at);
  } else {
    // (Window is the menu the application has as its windows'; its title
    // may be none.)
    id windows = ent_window_send(app, "windowsMenu");
    long n = ent_window_count(bar);
    at = n;
    for (long i = 1; i < n && windows; ++i) {
      id item = ((id(*)(id, SEL, long))objc_msgSend)(bar, ent_window_sel("itemAtIndex:"), i);
      if (ent_window_send(item, "submenu") == windows) {
        at = i;
        break;
      }
    }
  }
  id menu = ent_window_new_menu(name);
  id above = 0, inner = 0;
  char above_name[512] = "";
  char *line = held;
  while (line && *line) {
    char *end = strchr(line, '\n');
    if (end)
      *end = 0;
    size_t length = strlen(line);
    if (length && line[length - 1] == '\r')
      line[--length] = 0;
    bool in_inner = strncmp(line, "  ", 2) == 0;
    char *it = in_inner ? line + 2 : line;
    bool dim = it[0] == '~';
    if (dim)
      ++it;
    char *key = strchr(it, '\t');
    if (key)
      *key++ = 0;
    id into = menu;
    if (in_inner && above) {
      if (!inner) {
        inner = ent_window_new_menu(above_name);
        ((void (*)(id, SEL, id))objc_msgSend)(above, ent_window_sel("setSubmenu:"), inner);
        ent_window_send(inner, "release");
      }
      into = inner;
    }
    if (strcmp(it, "-") == 0) {
      ((void (*)(id, SEL, id))objc_msgSend)(
          into, ent_window_sel("addItem:"),
          ent_window_send(ent_window_class("NSMenuItem"), "separatorItem"));
    } else if (*it) {
      char path[1024];
      if (into == inner)
        snprintf(path, sizeof path, "%s/%s/%s", name, above_name, it);
      else
        snprintf(path, sizeof path, "%s/%s", name, it);
      id item = ((id(*)(id, SEL, id, SEL, id))objc_msgSend)(
          ent_window_send(ent_window_class("NSMenuItem"), "alloc"),
          ent_window_sel("initWithTitle:action:keyEquivalent:"), ent_window_ns_text(it),
          ent_window_sel("barPick:"), ent_window_ns_text(key ? key : ""));
      ((void (*)(id, SEL, id))objc_msgSend)(item, ent_window_sel("setTarget:"), target);
      ((void (*)(id, SEL, id))objc_msgSend)(item, ent_window_sel("setRepresentedObject:"),
                                            ent_window_ns_text(path));
      ((void (*)(id, SEL, signed char))objc_msgSend)(item, ent_window_sel("setEnabled:"),
                                                     !dim);
      ((void (*)(id, SEL, id))objc_msgSend)(into, ent_window_sel("addItem:"), item);
      if (into == menu) {
        above = item;
        inner = 0;
        snprintf(above_name, sizeof above_name, "%s", it);
      }
      ent_window_send(item, "release");
    }
    line = end ? end + 1 : 0;
  }
  id top = ((id(*)(id, SEL, id, SEL, id))objc_msgSend)(
      ent_window_send(ent_window_class("NSMenuItem"), "alloc"),
      ent_window_sel("initWithTitle:action:keyEquivalent:"), ent_window_ns_text(name),
      (SEL)0, ent_window_ns_text(""));
  ((void (*)(id, SEL, id))objc_msgSend)(top, ent_window_sel("setSubmenu:"), menu);
  ((void (*)(id, SEL, id, long))objc_msgSend)(bar, ent_window_sel("insertItem:atIndex:"),
                                             top, at);
  ent_window_send(menu, "release");
  ent_window_send(top, "release");
  objc_autoreleasePoolPop(pool);
  free(name);
  free(held);
  return true;
#else
  (void)title, (void)items;
  return false;
#endif
}

const ent_text *ent_window_menu_chosen(void) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  const ent_text *said = ent_window_ent_text(ent_window_bar_pick);
  ent_window_bar_pick[0] = 0;
  return said;
#else
  return ent_window_ent_text("");
#endif
}

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
// A text in single quotes for the shell ("'" as '\'').
static char *ent_window_quoted(const char *t) {
  size_t n = strlen(t), room = 3;
  for (size_t i = 0; i < n; ++i)
    room += t[i] == '\'' ? 4 : 1;
  char *q = malloc(room), *at = q;
  if (!q)
    return 0;
  *at++ = '\'';
  for (size_t i = 0; i < n; ++i) {
    if (t[i] == '\'') {
      memcpy(at, "'\\''", 4);
      at += 4;
    } else {
      *at++ = t[i];
    }
  }
  *at++ = '\'';
  *at = 0;
  return q;
}
#endif

// The system's panel for a folder (`folder`) or a file: the path chosen,
// or none.
static const ent_text *ent_window_choose(const ent_text *title, const ent_text *start,
                                         bool folder) {
  char *asked = ent_window_c_text(title), *from = ent_window_c_text(start);
  char chosen[4096] = "";
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  if (asked && from && IsWindowReady()) {
    void *pool = objc_autoreleasePoolPush();
    id panel = ent_window_send(ent_window_class("NSOpenPanel"), "openPanel");
    ((void (*)(id, SEL, signed char))objc_msgSend)(panel, ent_window_sel("setCanChooseFiles:"),
                                                   !folder);
    ((void (*)(id, SEL, signed char))objc_msgSend)(
        panel, ent_window_sel("setCanChooseDirectories:"), folder);
    ((void (*)(id, SEL, signed char))objc_msgSend)(
        panel, ent_window_sel("setCanCreateDirectories:"), folder);
    ((void (*)(id, SEL, signed char))objc_msgSend)(
        panel, ent_window_sel("setAllowsMultipleSelection:"), 0);
    ent_window_with_text(panel, "setMessage:", asked);
    ent_window_with_text(panel, "setPrompt:", folder ? "Open" : "Choose");
    if (*from) {
      id url = ((id(*)(id, SEL, id, signed char))objc_msgSend)(
          ent_window_class("NSURL"), ent_window_sel("fileURLWithPath:isDirectory:"),
          ent_window_ns_text(from), 1);
      ((void (*)(id, SEL, id))objc_msgSend)(panel, ent_window_sel("setDirectoryURL:"), url);
    }
    long answer = ((long (*)(id, SEL))objc_msgSend)(panel, ent_window_sel("runModal"));
    if (answer == 1) { // NSModalResponseOK
      id path = ent_window_send(ent_window_send(panel, "URL"), "path");
      const char *said =
          path ? ((const char *(*)(id, SEL))objc_msgSend)(path, ent_window_sel("UTF8String"))
               : 0;
      snprintf(chosen, sizeof chosen, "%s", said ? said : "");
    }
    objc_autoreleasePoolPop(pool);
  }
#elif !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  // zenity (GNOME and most others), else kdialog (KDE).
  char *q_title = asked ? ent_window_quoted(asked) : 0;
  char *q_from = from ? ent_window_quoted(*from ? from : ".") : 0;
  if (q_title && q_from) {
    size_t room = strlen(q_title) + strlen(q_from) + 256;
    char *command = malloc(room);
    if (command) {
      snprintf(command, room,
               "if command -v zenity >/dev/null 2>&1; then "
               "zenity --file-selection %s --title=%s --filename=%s/ 2>/dev/null; "
               "elif command -v kdialog >/dev/null 2>&1; then "
               "kdialog %s %s --title %s 2>/dev/null; fi",
               folder ? "--directory" : "", q_title, q_from,
               folder ? "--getexistingdirectory" : "--getopenfilename", q_from, q_title);
      FILE *out = popen(command, "r");
      if (out) {
        if (fgets(chosen, sizeof chosen, out)) {
          size_t n = strlen(chosen);
          while (n && (chosen[n - 1] == '\n' || chosen[n - 1] == '\r'))
            chosen[--n] = 0;
        }
        pclose(out);
      }
      free(command);
    }
  }
  free(q_title);
  free(q_from);
#endif
  free(asked);
  free(from);
  return ent_window_ent_text(chosen);
}
const ent_text *ent_window_choose_folder(const ent_text *title, const ent_text *start) {
  return ent_window_choose(title, start, true);
}
const ent_text *ent_window_choose_file(const ent_text *title, const ent_text *start) {
  return ent_window_choose(title, start, false);
}

// The system's file browser opened where `path` is (a file or a
// folder), with it selected: Finder on a Mac, Explorer on Windows; on
// Linux the folder it is in, by xdg-open. False where it could not be
// (it is not there).
#if defined(_WIN32)
#include <process.h>
#elif !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#endif
bool ent_window_show_in_files(const ent_text *path) {
  char *at = ent_window_c_text(path);
  bool shown = false;
  if (!at || !*at) {
    free(at);
    return false;
  }
  // (A path from the working folder made whole: Finder takes a relative
  // one for none, and opens the window it had last.)
#if defined(_WIN32)
  char *whole = _fullpath(NULL, at, 0);
#else
  char *whole = realpath(at, NULL);
#endif
  if (!whole) {
    free(at);
    return false;
  }
  free(at);
  at = whole;
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  void *pool = objc_autoreleasePoolPush();
  id url = ((id(*)(id, SEL, id))objc_msgSend)(ent_window_class("NSURL"),
                                              ent_window_sel("fileURLWithPath:"),
                                              ent_window_ns_text(at));
  id urls = ((id(*)(id, SEL, id))objc_msgSend)(ent_window_class("NSArray"),
                                               ent_window_sel("arrayWithObject:"), url);
  id workspace = ent_window_send(ent_window_class("NSWorkspace"), "sharedWorkspace");
  ((void (*)(id, SEL, id))objc_msgSend)(
      workspace, ent_window_sel("activateFileViewerSelectingURLs:"), urls);
  shown = workspace != 0;
  objc_autoreleasePoolPop(pool);
#elif defined(_WIN32)
  size_t room = strlen(at) + 16;
  char *select = malloc(room);
  if (select) {
    snprintf(select, room, "/select,\"%s\"", at);
    for (char *c = select; *c; ++c)
      if (*c == '/' && c > select + 7)
        *c = '\\';
    shown = _spawnlp(_P_NOWAIT, "explorer.exe", "explorer.exe", select, NULL) != -1;
    free(select);
  }
#elif !defined(__EMSCRIPTEN__)
  // (A file's folder: xdg-open would open the file itself.)
  char *folder = strdup(at);
  struct stat st;
  if (folder && stat(folder, &st) == 0 && !S_ISDIR(st.st_mode)) {
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
      *slash = 0;
    else if (slash)
      slash[1] = 0;
    else
      strcpy(folder, ".");
  }
  char *quoted = folder ? ent_window_quoted(folder) : 0;
  if (quoted) {
    size_t room = strlen(quoted) + 64;
    char *command = malloc(room);
    if (command) {
      snprintf(command, room, "xdg-open %s >/dev/null 2>&1 &", quoted);
      shown = system(command) == 0;
      free(command);
    }
  }
  free(quoted);
  free(folder);
#endif
  free(at);
  return shown;
}

// What `url` names opened by the program the system has for it (see
// window.ent).
bool ent_window_open_url(const ent_text *url) {
  char *at = ent_window_c_text(url);
  bool opened = false;
  if (!at || !*at || !strstr(at, ":")) {
    free(at);
    return false;
  }
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  void *pool = objc_autoreleasePoolPush();
  id link = ((id(*)(id, SEL, id))objc_msgSend)(
      ent_window_class("NSURL"), ent_window_sel("URLWithString:"),
      ent_window_ns_text(at));
  if (link) {
    id workspace =
        ent_window_send(ent_window_class("NSWorkspace"), "sharedWorkspace");
    opened = ((bool (*)(id, SEL, id))objc_msgSend)(
        workspace, ent_window_sel("openURL:"), link);
  }
  objc_autoreleasePoolPop(pool);
#elif defined(_WIN32)
  opened = _spawnlp(_P_NOWAIT, "rundll32.exe", "rundll32.exe",
                    "url.dll,FileProtocolHandler", at, NULL) != -1;
#elif !defined(__EMSCRIPTEN__)
  char *quoted = ent_window_quoted(at);
  if (quoted) {
    size_t room = strlen(quoted) + 64;
    char *command = malloc(room);
    if (command) {
      snprintf(command, room, "xdg-open %s >/dev/null 2>&1 &", quoted);
      opened = system(command) == 0;
      free(command);
    }
  }
  free(quoted);
#endif
  free(at);
  return opened;
}

// Whether the window is the one the user works in (the keys go to it).
bool ent_window_window_focused(void) { return IsWindowReady() && IsWindowFocused(); }

int32_t ent_window_alert(const ent_text *message, const ent_text *detail,
                         const ent_text *buttons) {
  char *said = ent_window_c_text(message), *more = ent_window_c_text(detail),
       *held = ent_window_c_text(buttons);
  int32_t pressed = -1;
  if (said && more && held) {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    if (IsWindowReady() && !(getenv("ENT_WINDOW_MENU") &&
                             strcmp(getenv("ENT_WINDOW_MENU"), "drawn") == 0)) {
      void *pool = objc_autoreleasePoolPush();
      id alert = ent_window_send(ent_window_send(ent_window_class("NSAlert"), "alloc"), "init");
      ent_window_with_text(alert, "setMessageText:", said);
      ent_window_with_text(alert, "setInformativeText:", more);
      int n = 0;
      for (char *line = held; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
          *end = 0;
        if (*line) {
          ent_window_with_text(alert, "addButtonWithTitle:", line);
          ++n;
        }
        line = end ? end + 1 : 0;
      }
      long answer = ((long (*)(id, SEL))objc_msgSend)(alert, ent_window_sel("runModal"));
      ent_window_send(alert, "release");
      objc_autoreleasePoolPop(pool);
      // (NSAlertFirstButtonReturn is 1000, the next 1001, ...)
      pressed = answer >= 1000 && answer < 1000 + n ? (int32_t)(answer - 1000) : -1;
      free(said);
      free(more);
      free(held);
      return pressed;
    }
#endif
#if !defined(__EMSCRIPTEN__)
    // Drawn: the message and the detail (not to be chosen), then the
    // buttons as items.
    size_t room = strlen(said) + strlen(more) + strlen(held) + 16;
    char *lines = malloc(room);
    if (lines) {
      snprintf(lines, room, "~%s\n%s%s%s-\n%s", said, *more ? "~" : "", more,
               *more ? "\n" : "", held);
      ent_text *items = (ent_text *)ent_window_ent_text(lines);
      ent_window_menu_items m;
      if (ent_window_menu_split(items, &m)) {
        int first = *more ? 3 : 2;
        int32_t chosen = ent_window_menu_drawn(&m);
        pressed = chosen >= first ? chosen - first : -1;
        free(m.held);
      }
      free(lines);
    }
#endif
  }
  free(said);
  free(more);
  free(held);
  return pressed;
}

void ent_window_window_title(const ent_text *title) {
  char *t = ent_window_c_text(title);
  if (t && IsWindowReady())
    SetWindowTitle(t);
  free(t);
}
