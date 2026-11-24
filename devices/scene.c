// The scene device (devices/scene.ent), drawn with raylib into the
// window's frame. Compiled with the program, against the declarations
// generated for it (ent_extern.h); it never sees the world.

#include "ent_extern.h"

#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

// 0xRRGGBB, opaque; or 0xAARRGGBB (as the window device has them).
static Color ent_scene_color(int32_t rgb) {
  unsigned char alpha = (unsigned char)((uint32_t)rgb >> 24);
  return (Color){(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8),
                 (unsigned char)rgb, alpha ? alpha : 255};
}
static Color ent_scene_shaded(Color c, float part) {
  return (Color){(unsigned char)(c.r * part), (unsigned char)(c.g * part),
                 (unsigned char)(c.b * part), c.a};
}

// The window device's way of drawing, put aside while the scene is
// drawn (devices/window.c).
void ent_window_aside(void);
void ent_window_again(void);
float ent_window_pixels(void);

// The eye of the scene now (and of the last one, for the mouse and for
// where its points are).
static Camera3D ent_scene_eye;
static bool ent_scene_hit;
static Vector3 ent_scene_ground;

// A scene in a part of the window (`begin_scene_in`): that part in the
// picture's pixels, from its top left; else all of it.
static bool ent_scene_part;
static float ent_scene_px, ent_scene_py, ent_scene_pw, ent_scene_ph;

void ent_scene_begin_scene(float eye_x, float eye_y, float eye_z, float at_x,
                           float at_y, float at_z, float fov, bool ortho) {
  ent_scene_part = false;
  ent_scene_eye = (Camera3D){.position = {eye_x, eye_y, eye_z},
                             .target = {at_x, at_y, at_z},
                             .up = {0, 1, 0},
                             .fovy = fov,
                             .projection = ortho ? CAMERA_ORTHOGRAPHIC
                                                 : CAMERA_PERSPECTIVE};
  // (The mouse on the ground: where its ray meets y = 0. In the
  // picture's pixels, as the scene is drawn.)
  Vector2 mouse = GetMousePosition();
  Vector2 dpi = GetWindowScaleDPI();
  mouse.x *= dpi.x;
  mouse.y *= dpi.y;
  Ray ray = GetScreenToWorldRayEx(mouse, ent_scene_eye, GetRenderWidth(),
                                  GetRenderHeight());
  ent_scene_hit = false;
  if (fabsf(ray.direction.y) > 1e-6f) {
    float t = -ray.position.y / ray.direction.y;
    if (t > 0) {
      ent_scene_hit = true;
      ent_scene_ground =
          Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    }
  }
  ent_window_aside();
  BeginMode3D(ent_scene_eye);
  rlDisableBackfaceCulling();
}

void ent_scene_begin_scene_in(float x, float y, float w, float h,
                              int32_t back, float eye_x, float eye_y,
                              float eye_z, float at_x, float at_y, float at_z,
                              float fov, bool ortho) {
  float px = ent_window_pixels();
  ent_scene_px = x * px;
  ent_scene_py = y * px;
  ent_scene_pw = w * px;
  ent_scene_ph = h * px;
  if (ent_scene_pw < 1)
    ent_scene_pw = 1;
  if (ent_scene_ph < 1)
    ent_scene_ph = 1;
  ent_scene_part = true;
  ent_scene_eye = (Camera3D){.position = {eye_x, eye_y, eye_z},
                             .target = {at_x, at_y, at_z},
                             .up = {0, 1, 0},
                             .fovy = fov,
                             .projection = ortho ? CAMERA_ORTHOGRAPHIC
                                                 : CAMERA_PERSPECTIVE};
  // (The mouse on the ground, as seen in that part.)
  Vector2 mouse = GetMousePosition();
  Vector2 dpi = GetWindowScaleDPI();
  mouse.x = mouse.x * dpi.x - ent_scene_px;
  mouse.y = mouse.y * dpi.y - ent_scene_py;
  Ray ray = GetScreenToWorldRayEx(mouse, ent_scene_eye, (int)ent_scene_pw,
                                  (int)ent_scene_ph);
  ent_scene_hit = false;
  if (fabsf(ray.direction.y) > 1e-6f) {
    float t = -ray.position.y / ray.direction.y;
    if (t > 0) {
      ent_scene_hit = true;
      ent_scene_ground =
          Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    }
  }
  ent_window_aside();
  // (Only that part: its colour and depth cleared, the eye's picture
  // fitted to it.)
  int gx = (int)ent_scene_px;
  int gy = GetRenderHeight() - (int)(ent_scene_py + ent_scene_ph);
  int gw = (int)ent_scene_pw, gh = (int)ent_scene_ph;
  rlDrawRenderBatchActive();
  rlEnableScissorTest();
  rlScissor(gx, gy, gw, gh);
  Color c = ent_scene_color(back);
  rlClearColor(c.r, c.g, c.b, c.a);
  rlClearScreenBuffers();
  rlViewport(gx, gy, gw, gh);
  rlMatrixMode(RL_PROJECTION);
  rlPushMatrix();
  rlLoadIdentity();
  double aspect = (double)gw / (double)gh;
  if (!ortho) {
    double top = RL_CULL_DISTANCE_NEAR * tan(fov * 0.5 * DEG2RAD);
    double right = top * aspect;
    rlFrustum(-right, right, -top, top, RL_CULL_DISTANCE_NEAR,
              RL_CULL_DISTANCE_FAR);
  } else {
    double top = fov / 2.0;
    double right = top * aspect;
    rlOrtho(-right, right, -top, top, RL_CULL_DISTANCE_NEAR,
            RL_CULL_DISTANCE_FAR);
  }
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  Matrix view = MatrixLookAt(ent_scene_eye.position, ent_scene_eye.target,
                             ent_scene_eye.up);
  rlMultMatrixf(MatrixToFloat(view));
  rlEnableDepthTest();
  rlDisableBackfaceCulling();
}

// The eye, for the mesh device (devices/mesh.c).
Camera3D ent_scene_camera(void) { return ent_scene_eye; }

void ent_scene_end_scene(void) {
  EndMode3D();
  rlEnableBackfaceCulling();
  if (ent_scene_part) {
    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
    rlDisableScissorTest();
  }
  ent_window_again();
}

static void ent_scene_quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d,
                           Color col) {
  rlColor4ub(col.r, col.g, col.b, col.a);
  rlVertex3f(a.x, a.y, a.z);
  rlVertex3f(b.x, b.y, b.z);
  rlVertex3f(c.x, c.y, c.z);
  rlVertex3f(d.x, d.y, d.z);
}

void ent_scene_draw_boxes(int32_t count, const float *x, const float *y,
                          const float *z, const float *w, const float *h,
                          const float *d, const int32_t *color,
                          const int32_t *edge) {
  for (int32_t i = 0; i < count; i++) {
    Color c = ent_scene_color(color[i]);
    float x0 = x[i], y0 = y[i], z0 = z[i];
    float x1 = x0 + w[i], y1 = y0 + h[i], z1 = z0 + d[i];
    Vector3 p000 = {x0, y0, z0}, p100 = {x1, y0, z0}, p010 = {x0, y1, z0},
            p110 = {x1, y1, z0}, p001 = {x0, y0, z1}, p101 = {x1, y0, z1},
            p011 = {x0, y1, z1}, p111 = {x1, y1, z1};
    rlCheckRenderBatchLimit(24);
    rlBegin(RL_QUADS);
    // (Lit from above, and a little from one side: the top brightest.)
    ent_scene_quad(p010, p011, p111, p110, c);
    ent_scene_quad(p000, p100, p101, p001, ent_scene_shaded(c, 0.45f));
    ent_scene_quad(p000, p010, p110, p100, ent_scene_shaded(c, 0.62f));
    ent_scene_quad(p001, p101, p111, p011, ent_scene_shaded(c, 0.82f));
    ent_scene_quad(p000, p001, p011, p010, ent_scene_shaded(c, 0.70f));
    ent_scene_quad(p100, p110, p111, p101, ent_scene_shaded(c, 0.90f));
    rlEnd();
    if (edge[i]) {
      Color e = ent_scene_color(edge[i]);
      Vector3 corners[8] = {p000, p100, p110, p010, p001, p101, p111, p011};
      static const int lines[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                       {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                       {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      rlCheckRenderBatchLimit(24);
      rlBegin(RL_LINES);
      rlColor4ub(e.r, e.g, e.b, e.a);
      for (int k = 0; k < 12; k++) {
        Vector3 a = corners[lines[k][0]], b = corners[lines[k][1]];
        rlVertex3f(a.x, a.y, a.z);
        rlVertex3f(b.x, b.y, b.z);
      }
      rlEnd();
    }
  }
}

void ent_scene_draw_rays(int32_t count, const float *x0, const float *y0,
                         const float *z0, const float *x1, const float *y1,
                         const float *z1, const int32_t *color) {
  for (int32_t i = 0; i < count; i++)
    DrawLine3D((Vector3){x0[i], y0[i], z0[i]}, (Vector3){x1[i], y1[i], z1[i]},
               ent_scene_color(color[i]));
}

void ent_scene_draw_balls(int32_t count, const float *x, const float *y,
                          const float *z, const float *r, const int32_t *color,
                          const bool *wire) {
  for (int32_t i = 0; i < count; i++) {
    Vector3 at = {x[i], y[i], z[i]};
    if (wire[i])
      DrawSphereWires(at, r[i], 8, 16, ent_scene_color(color[i]));
    else
      DrawSphereEx(at, r[i], 8, 12, ent_scene_color(color[i]));
  }
}

bool ent_scene_ground_hit(void) { return ent_scene_hit; }
float ent_scene_ground_x(void) { return ent_scene_ground.x; }
float ent_scene_ground_z(void) { return ent_scene_ground.z; }

// (In the window's points: the picture's pixels, so many to a point.)
static Vector2 ent_scene_on_screen(float x, float y, float z) {
  if (ent_scene_part) {
    Vector2 v = GetWorldToScreenEx((Vector3){x, y, z}, ent_scene_eye,
                                   (int)ent_scene_pw, (int)ent_scene_ph);
    return (Vector2){v.x + ent_scene_px, v.y + ent_scene_py};
  }
  return GetWorldToScreenEx((Vector3){x, y, z}, ent_scene_eye,
                            GetRenderWidth(), GetRenderHeight());
}
float ent_scene_screen_x(float x, float y, float z) {
  return ent_scene_on_screen(x, y, z).x / ent_window_pixels();
}
float ent_scene_screen_y(float x, float y, float z) {
  return ent_scene_on_screen(x, y, z).y / ent_window_pixels();
}
bool ent_scene_in_view(float x, float y, float z) {
  if (ent_scene_eye.projection == CAMERA_ORTHOGRAPHIC)
    return true;
  Vector3 ahead = Vector3Subtract(ent_scene_eye.target, ent_scene_eye.position);
  Vector3 to = Vector3Subtract((Vector3){x, y, z}, ent_scene_eye.position);
  return Vector3DotProduct(ahead, to) > 0;
}
