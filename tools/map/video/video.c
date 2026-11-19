// tools/map/video/video.ent: the ent map tour's helpers. Mouse and key
// events posted into the program's own NSApp, as if the user moved,
// clicked and typed (macOS: no permission is needed for a program's own
// window); a clock; and each picture drawn handed to ffmpeg, on a
// timeline of so many frames a second that keeps up with the clock.
#include "ent_extern.h"
#include <objc/message.h>
#include <objc/runtime.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rlDrawRenderBatchActive(void);

typedef struct { double x, y; } ent_video_point;
static SEL ent_video_sel(const char *n) { return sel_registerName(n); }
static id ent_video_app(void) {
  return ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"),
                                        ent_video_sel("sharedApplication"));
}
static id ent_video_string(const char *bytes, size_t length) {
  char b[2048];
  snprintf(b, sizeof b, "%.*s", (int)length, bytes);
  return ((id(*)(id, SEL, const char *))objc_msgSend)(
      (id)objc_getClass("NSString"), ent_video_sel("stringWithUTF8String:"), b);
}

void ent_video_post_mouse(float x, float y, int32_t kind, int32_t clicks) {
  id window = (id)GetWindowHandle();
  if (!window)
    return;
  id view = ((id(*)(id, SEL))objc_msgSend)(window, ent_video_sel("contentView"));
  struct { ent_video_point at, size; } frame =
      ((typeof(frame)(*)(id, SEL))objc_msgSend)(view, ent_video_sel("frame"));
  ent_video_point at = {x, frame.size.y - y};
  long number = ((long (*)(id, SEL))objc_msgSend)(window, ent_video_sel("windowNumber"));
  id event = ((id(*)(id, SEL, unsigned long, ent_video_point, unsigned long, double, long,
                     id, long, long, float))objc_msgSend)(
      (id)objc_getClass("NSEvent"),
      ent_video_sel("mouseEventWithType:location:modifierFlags:timestamp:"
                    "windowNumber:context:eventNumber:clickCount:pressure:"),
      (unsigned long)kind, at, 0, GetTime(), number, (id)0, 0, clicks, 1.0f);
  ((void (*)(id, SEL, id, signed char))objc_msgSend)(
      ent_video_app(), ent_video_sel("postEvent:atStart:"), event, 0);
}

// A key: down (phase 1), up (2) or both (3); its characters (Return
// "\r", Backspace "\x7f"), its key code (the Mac's: A 0, S 1, F 3, C 8,
// N 45, Return 36, Backspace 51) and Command with it or not. (raylib
// sees a key as pressed only where it is still down at a frame's end:
// down and up in frames of their own, where that matters.)
void ent_video_post_key(const ent_text *chars, int32_t code, bool command, int32_t phase) {
  id window = (id)GetWindowHandle();
  if (!window)
    return;
  long number = ((long (*)(id, SEL))objc_msgSend)(window, ent_video_sel("windowNumber"));
  // (Return and Backspace as the keyboard gives them, whatever was said.)
  id typed = code == 36   ? ent_video_string("\r", 1)
             : code == 51 ? ent_video_string("\x7f", 1)
                          : ent_video_string((const char *)chars->bytes, chars->length);
  ent_video_point at = {10, 10};
  for (int k = 0; k < 2; ++k) {
    if (!(phase & (1 << k)))
      continue;
    id event = ((id(*)(id, SEL, unsigned long, ent_video_point, unsigned long, double, long, id,
                       id, id, signed char, unsigned short))objc_msgSend)(
        (id)objc_getClass("NSEvent"),
        ent_video_sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:"
                      "context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
        k == 0 ? 10 : 11, at, command ? 1UL << 20 : 0, GetTime(), number, (id)0, typed,
        typed, 0, (unsigned short)code);
    ((void (*)(id, SEL, id, signed char))objc_msgSend)(
        ent_video_app(), ent_video_sel("postEvent:atStart:"), event, 0);
  }
}

// The Command key down or up (the left one), as the keyboard says it.
void ent_video_post_command(bool down) {
  id window = (id)GetWindowHandle();
  if (!window)
    return;
  long number = ((long (*)(id, SEL))objc_msgSend)(window, ent_video_sel("windowNumber"));
  ent_video_point at = {10, 10};
  id event = ((id(*)(id, SEL, unsigned long, ent_video_point, unsigned long, double, long, id,
                     id, id, signed char, unsigned short))objc_msgSend)(
      (id)objc_getClass("NSEvent"),
      ent_video_sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:"
                    "context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
      12, at, down ? (1UL << 20) | 0x8 : 0, GetTime(), number, (id)0,
      ent_video_string("", 0), ent_video_string("", 0), 0, 55);
  ((void (*)(id, SEL, id, signed char))objc_msgSend)(
      ent_video_app(), ent_video_sel("postEvent:atStart:"), event, 0);
}

// A file dropped on the window at (x, y), as Finder drops one: what the
// window's view is given is a drag whose pasteboard holds the file's URL.
static ent_video_point ent_video_drop_at;
static id ent_video_drop_board;
static ent_video_point ent_video_drag_location(id self, SEL c) {
  (void)self, (void)c;
  return ent_video_drop_at;
}
static id ent_video_drag_board(id self, SEL c) {
  (void)self, (void)c;
  return ent_video_drop_board;
}
static unsigned long ent_video_drag_mask(id self, SEL c) {
  (void)self, (void)c;
  return 1; // NSDragOperationCopy
}

void ent_video_post_drop(const ent_text *path, float x, float y) {
  id window = (id)GetWindowHandle();
  if (!window)
    return;
  static Class drag;
  if (!drag) {
    drag = objc_allocateClassPair(objc_getClass("NSObject"), "EntVideoDrag", 0);
    class_addMethod(drag, ent_video_sel("draggingLocation"), (IMP)ent_video_drag_location,
                    "{CGPoint=dd}@:");
    class_addMethod(drag, ent_video_sel("draggingPasteboard"), (IMP)ent_video_drag_board, "@@:");
    class_addMethod(drag, ent_video_sel("draggingSourceOperationMask"),
                    (IMP)ent_video_drag_mask, "Q@:");
    objc_registerClassPair(drag);
  }
  id view = ((id(*)(id, SEL))objc_msgSend)(window, ent_video_sel("contentView"));
  struct { ent_video_point at, size; } frame =
      ((typeof(frame)(*)(id, SEL))objc_msgSend)(view, ent_video_sel("frame"));
  ent_video_drop_at = (ent_video_point){x, frame.size.y - y};
  ent_video_drop_board = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSPasteboard"),
                                                        ent_video_sel("pasteboardWithUniqueName"));
  ((long (*)(id, SEL))objc_msgSend)(ent_video_drop_board, ent_video_sel("clearContents"));
  id url = ((id(*)(id, SEL, id))objc_msgSend)(
      (id)objc_getClass("NSURL"), ent_video_sel("fileURLWithPath:"),
      ent_video_string((const char *)path->bytes, path->length));
  id urls = ((id(*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSArray"),
                                               ent_video_sel("arrayWithObject:"), url);
  ((signed char (*)(id, SEL, id))objc_msgSend)(ent_video_drop_board,
                                               ent_video_sel("writeObjects:"), urls);
  id sender = ((id(*)(id, SEL))objc_msgSend)(
      ((id(*)(id, SEL))objc_msgSend)((id)drag, ent_video_sel("alloc")), ent_video_sel("init"));
  ((signed char (*)(id, SEL, id))objc_msgSend)(view, ent_video_sel("performDragOperation:"),
                                               sender);
}

// The wheel turned (lines; up is positive), over the window's view as a
// trackpad turns it. (Handed to the view itself, as an event posted
// would go where the real pointer is; and within the window's next
// polling of its events, after raylib has set the wheel back to 0 for
// that frame: a moment later, from the run loop.)
#include <dlfcn.h>
static void ent_video_wheel_go(id self, SEL c, id event) {
  (void)self, (void)c;
  id window = (id)GetWindowHandle();
  id view = ((id(*)(id, SEL))objc_msgSend)(window, ent_video_sel("contentView"));
  ((void (*)(id, SEL, id))objc_msgSend)(view, ent_video_sel("scrollWheel:"), event);
}
void ent_video_post_wheel(float lines) {
  if (!GetWindowHandle())
    return;
  typedef void *(*make_fn)(void *, int, unsigned, int, ...);
  typedef void (*release_fn)(const void *);
  static make_fn make;
  static release_fn release;
  static id wheel;
  if (!make) {
    make = (make_fn)dlsym(RTLD_DEFAULT, "CGEventCreateScrollWheelEvent");
    release = (release_fn)dlsym(RTLD_DEFAULT, "CFRelease");
    Class k = objc_allocateClassPair(objc_getClass("NSObject"), "EntVideoWheel", 0);
    class_addMethod(k, ent_video_sel("go:"), (IMP)ent_video_wheel_go, "v@:@");
    objc_registerClassPair(k);
    wheel = ((id(*)(id, SEL))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)((id)k, ent_video_sel("alloc")), ent_video_sel("init"));
  }
  if (!make)
    return;
  // (In pixels, as a trackpad's: GLFW takes a tenth of them.)
  void *cg = make(NULL, 0, 1, (int)(lines * 40.0f));
  id event = ((id(*)(id, SEL, void *))objc_msgSend)((id)objc_getClass("NSEvent"),
                                                   ent_video_sel("eventWithCGEvent:"), cg);
  ((void (*)(id, SEL, SEL, id, double))objc_msgSend)(
      wheel, ent_video_sel("performSelector:withObject:afterDelay:"), ent_video_sel("go:"),
      event, 0.0);
  if (release)
    release(cg);
}

// The program's context menu, answered by the script: the video shows
// its own picture of the menu (the system's is not in the window's
// picture), and the item the script chose is the answer (-1: none).
static int32_t ent_video_menu_answer = -1;
void ent_video_menu_answer_set(int32_t item) { ent_video_menu_answer = item; }
int32_t ent_video_video_menu(const ent_text *items) {
  (void)items;
  int32_t item = ent_video_menu_answer;
  ent_video_menu_answer = -1;
  return item;
}

double ent_video_clock(void) { return GetTime(); }

// The pictures, to ffmpeg: started with the first, at the size it has
// (later ones of another size give the last again); at `seconds` on the
// timeline, as many frames as bring it there, all this picture.
static FILE *ent_video_pipe;
static int ent_video_width, ent_video_height;
static long ent_video_frames;

void ent_video_capture(const ent_text *file, int32_t fps, float seconds) {
  long due = (long)(seconds * fps) + 1;
  if (ent_video_pipe && ent_video_frames >= due)
    return;
  rlDrawRenderBatchActive();
  Image picture = LoadImageFromScreen();
  if (!ent_video_pipe) {
    char command[1024];
    ent_video_width = picture.width;
    ent_video_height = picture.height;
    snprintf(command, sizeof command,
             "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
             "-vf scale=1920:-2:flags=lanczos,format=yuv420p -c:v libx264 -preset veryfast "
             "-crf 14 '%.*s'",
             picture.width, picture.height, fps, (int)file->length,
             (const char *)file->bytes);
    ent_video_pipe = popen(command, "w");
    if (!ent_video_pipe) {
      UnloadImage(picture);
      return;
    }
  }
  static unsigned char *last;
  size_t size = (size_t)ent_video_width * ent_video_height * 4;
  if (!last)
    last = calloc(1, size);
  if (picture.width == ent_video_width && picture.height == ent_video_height &&
      picture.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    memcpy(last, picture.data, size);
  UnloadImage(picture);
  for (; ent_video_frames < due; ++ent_video_frames)
    fwrite(last, 1, size, ent_video_pipe);
}

void ent_video_capture_end(void) {
  if (ent_video_pipe)
    pclose(ent_video_pipe);
  ent_video_pipe = NULL;
}

void ent_video_say(const ent_text *t) {
  printf("%.*s\n", (int)t->length, (const char *)t->bytes);
  fflush(stdout);
}
