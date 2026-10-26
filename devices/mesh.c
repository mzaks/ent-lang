// The mesh device (devices/mesh.ent): meshes of blocks made with raylib,
// kept on the GPU and drawn into the scene (devices/scene.c). Compiled
// with the program, against the declarations generated for it
// (ent_extern.h); it never sees the world.

#include "ent_extern.h"
#include "ent_files.h"

#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdlib.h>
#include <string.h>

// The scene's eye (devices/scene.c).
Camera3D ent_scene_camera(void);

#define ENT_MESH_SLOTS 4096

// A slot: its faces that hide what is behind them and those that do not
// (each a mesh of two triangles a face, without indices: no limit of
// 65,536 corners), and the box they are in.
struct ent_mesh_slot {
  Mesh solid, clear;
  bool has_solid, has_clear;
  Vector3 lo, hi;
  int32_t faces;
};
static struct ent_mesh_slot ent_mesh_slots[ENT_MESH_SLOTS];

static Texture2D ent_mesh_atlas;
static int32_t ent_mesh_per_row = 1;
static Shader ent_mesh_shader;
static Material ent_mesh_material;
static bool ent_mesh_ready;
static int ent_mesh_eye_at, ent_mesh_fog_at, ent_mesh_far_at, ent_mesh_day_at,
    ent_mesh_tile_at, ent_mesh_inset_at;
static float ent_mesh_day[3] = {1, 1, 1};
static int32_t ent_mesh_drawn, ent_mesh_faces_seen;

// Fog by how far a point is from the eye; texels that are clear left out.
static const char *ent_mesh_vertex =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec2 vertexTexCoord2;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "out vec2 uv;\n"
    "out vec2 origin;\n"
    "out vec4 tint;\n"
    "out vec3 at;\n"
    "void main() {\n"
    "  uv = vertexTexCoord;\n"
    "  origin = vertexTexCoord2;\n"
    "  tint = vertexColor;\n"
    "  at = vertexPosition;\n"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *ent_mesh_fragment =
    "#version 330\n"
    "in vec2 uv;\n"
    "in vec2 origin;\n"
    "in vec4 tint;\n"
    "in vec3 at;\n"
    "uniform sampler2D texture0;\n"
    "uniform float tile;\n"
    "uniform float inset;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 eye;\n"
    "uniform vec3 fog;\n"
    "uniform float far;\n"
    "uniform vec3 day;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "  // (A face of many blocks: its tile again for each, `uv` in blocks.)\n"
    "  vec2 at_tile = origin + inset + fract(uv) * (tile - 2.0 * inset);\n"
    "  vec4 c = texture(texture0, at_tile) * colDiffuse;\n"
    "  if (c.a < 0.1) discard;\n"
    "  // (The sky's light, by the day; a lamp's, warm; the brighter.)\n"
    "  vec3 lit = max(tint.r * day, tint.g * vec3(1.0, 0.84, 0.6));\n"
    "  c.rgb *= max(lit, vec3(0.02));\n"
    "  float f = clamp((distance(at, eye) - far * 0.55) / (far * 0.45), 0.0, 1.0);\n"
    "  finalColor = vec4(mix(c.rgb, fog, f * f), c.a);\n"
    "}\n";

static void ent_mesh_steps(void);

// (The shader and the material, once there is a window.)
static void ent_mesh_start(void) {
  if (ent_mesh_ready)
    return;
  ent_mesh_shader = LoadShaderFromMemory(ent_mesh_vertex, ent_mesh_fragment);
  ent_mesh_eye_at = GetShaderLocation(ent_mesh_shader, "eye");
  ent_mesh_fog_at = GetShaderLocation(ent_mesh_shader, "fog");
  ent_mesh_far_at = GetShaderLocation(ent_mesh_shader, "far");
  ent_mesh_day_at = GetShaderLocation(ent_mesh_shader, "day");
  ent_mesh_tile_at = GetShaderLocation(ent_mesh_shader, "tile");
  ent_mesh_inset_at = GetShaderLocation(ent_mesh_shader, "inset");
  ent_mesh_material = LoadMaterialDefault();
  ent_mesh_steps();
  ent_mesh_material.shader = ent_mesh_shader;
  ent_mesh_ready = true;
}

static void ent_mesh_use_atlas(Image image, int32_t per_row) {
  ent_mesh_start();
  if (ent_mesh_atlas.id != 0)
    UnloadTexture(ent_mesh_atlas);
  ent_mesh_atlas = LoadTextureFromImage(image);
  // (Texels as they are, blocky, near and far.)
  SetTextureFilter(ent_mesh_atlas, TEXTURE_FILTER_POINT);
  ent_mesh_per_row = per_row > 0 ? per_row : 1;
  ent_mesh_material.maps[MATERIAL_MAP_DIFFUSE].texture = ent_mesh_atlas;
}

void ent_mesh_make_atlas(int32_t count, const int32_t *color, int32_t width,
                         int32_t height, int32_t per_row) {
  if (width <= 0 || height <= 0 || count < width * height)
    return;
  Image image = GenImageColor(width, height, BLANK);
  Color *pixels = image.data;
  for (int32_t i = 0; i < width * height; i++) {
    uint32_t c = (uint32_t)color[i];
    unsigned char alpha = (unsigned char)(c >> 24);
    pixels[i] = (Color){(unsigned char)(c >> 16), (unsigned char)(c >> 8),
                        (unsigned char)c, alpha ? alpha : 255};
    // (0x01RRGGBB and the like: clear; 0 itself is black.)
    if (alpha == 1)
      pixels[i].a = 0;
  }
  ent_mesh_use_atlas(image, per_row);
  UnloadImage(image);
}

void ent_mesh_load_atlas(const ent_text126 *file, int32_t per_row) {
  char named[sizeof(file->bytes) + 1];
  memcpy(named, file->bytes, file->length);
  named[file->length] = 0;
  struct ent_file held;
  const char *kind = GetFileExtension(named);
  if (!kind || !ent_file_open(named, &held))
    return;
  Image image = LoadImageFromMemory(kind, held.bytes, (int)held.size);
  ent_file_close(&held);
  if (!image.data)
    return;
  ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  ent_mesh_use_atlas(image, per_row);
  UnloadImage(image);
}

// The corners of each side's face, of a block at the origin, as seen
// from outside going round against the clock (raylib's front), from the
// bottom left of its tile: East +x, West -x, Top +y, Bottom -y, South +z,
// North -z (the order of `Side`).
static const float ent_mesh_corners[6][4][3] = {
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}};
// (Lit from above, and a little from one side, as the scene's boxes.)
static const float ent_mesh_shade[6] = {0.90f, 0.70f, 1.0f, 0.45f, 0.82f, 0.62f};
// How bright a step of light is (0 to 15): each a fifth less than the
// one above it, as eyes see it.
static float ent_mesh_bright[16];
static void ent_mesh_steps(void) {
  for (int l = 0; l < 16; l++)
    ent_mesh_bright[l] = powf(0.8f, (float)(15 - l));
}
// How much light a corner keeps, by how hidden it is (0 to 3).
static const float ent_mesh_hidden[4] = {1.0f, 0.78f, 0.6f, 0.44f};
// The two triangles of a face, by its corners: cut from the first corner
// to the third, or (where the second and fourth are the lighter, so that
// the shade runs along the cut and not across it) from the second to
// the fourth.
static const int ent_mesh_order[2][6] = {{0, 1, 2, 0, 2, 3}, {1, 2, 3, 1, 3, 0}};

static void ent_mesh_free(Mesh *m, bool *has) {
  if (*has)
    UnloadMesh(*m);
  *m = (Mesh){0};
  *has = false;
}

// A mesh of those faces that are clear or not, as asked.
static bool ent_mesh_build(Mesh *m, int32_t count, const int32_t *x,
                           const int32_t *y, const int32_t *z,
                           const ent_mesh_Side *side, const int32_t *tile,
                           const int32_t *light, const int32_t *glow,
                           const int32_t *hidden, const int32_t *wide,
                           const int32_t *high, const bool *clear,
                           bool want) {
  int32_t n = 0;
  for (int32_t i = 0; i < count; i++)
    if (clear[i] == want && side[i] < 6)
      n++;
  if (n == 0)
    return false;
  *m = (Mesh){0};
  m->vertexCount = n * 6;
  m->triangleCount = n * 2;
  m->vertices = MemAlloc(sizeof(float) * 3 * (size_t)m->vertexCount);
  m->texcoords = MemAlloc(sizeof(float) * 2 * (size_t)m->vertexCount);
  m->texcoords2 = MemAlloc(sizeof(float) * 2 * (size_t)m->vertexCount);
  m->colors = MemAlloc(4 * (size_t)m->vertexCount);
  float per = (float)ent_mesh_per_row;
  // (Where on its tile each corner is, in blocks: the shader takes the
  // tile again for every block a face spans, from the tile's corner.)
  static const float corner_uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  int32_t v = 0;
  for (int32_t i = 0; i < count; i++) {
    if (clear[i] != want || side[i] >= 6)
      continue;
    int s = side[i];
    float u0 = (float)(tile[i] % ent_mesh_per_row) / per;
    float v0 = (float)(tile[i] / ent_mesh_per_row) / per;
    // (How far the face goes on each axis: one block along the way it
    // looks, `wide` along the first other axis, `high` the second. Its
    // tile's two ways: along the axes its first corners go.)
    float size[3] = {1, 1, 1};
    int normal = s / 2;
    int first = normal == 0 ? 1 : 0, second = normal == 2 ? 1 : 2;
    size[first] = (float)(wide[i] > 0 ? wide[i] : 1);
    size[second] = (float)(high[i] > 0 ? high[i] : 1);
    int axis_u = 0, axis_v = 0;
    for (int a = 0; a < 3; a++) {
      if (ent_mesh_corners[s][0][a] != ent_mesh_corners[s][1][a])
        axis_u = a;
      if (ent_mesh_corners[s][1][a] != ent_mesh_corners[s][2][a])
        axis_v = a;
    }
    int h[4];
    for (int c = 0; c < 4; c++)
      h[c] = (hidden[i] >> (2 * c)) & 3;
    const int *order = ent_mesh_order[h[0] + h[2] > h[1] + h[3]];
    for (int k = 0; k < 6; k++) {
      int c = order[k];
      // (In the colour: red the sky's light, green the lamps'.)
      float shade = 255.0f * ent_mesh_shade[s] * ent_mesh_hidden[h[c]];
      unsigned char sky =
          (unsigned char)(shade * ent_mesh_bright[(light[i] >> (4 * c)) & 15]);
      unsigned char lamp =
          (unsigned char)(shade * ent_mesh_bright[(glow[i] >> (4 * c)) & 15] *
                          (((glow[i] >> (4 * c)) & 15) > 0));
      m->vertices[v * 3 + 0] = (float)x[i] + ent_mesh_corners[s][c][0] * size[0];
      m->vertices[v * 3 + 1] = (float)y[i] + ent_mesh_corners[s][c][1] * size[1];
      m->vertices[v * 3 + 2] = (float)z[i] + ent_mesh_corners[s][c][2] * size[2];
      m->texcoords[v * 2 + 0] = corner_uv[c][0] * size[axis_u];
      m->texcoords[v * 2 + 1] = corner_uv[c][1] * size[axis_v];
      m->texcoords2[v * 2 + 0] = u0;
      m->texcoords2[v * 2 + 1] = v0;
      m->colors[v * 4 + 0] = sky;
      m->colors[v * 4 + 1] = lamp;
      m->colors[v * 4 + 2] = 0;
      m->colors[v * 4 + 3] = 255;
      v++;
    }
  }
  UploadMesh(m, false);
  // (On the GPU now: the copies here are not needed.)
  MemFree(m->vertices);
  MemFree(m->texcoords);
  MemFree(m->texcoords2);
  MemFree(m->colors);
  m->vertices = NULL;
  m->texcoords = NULL;
  m->texcoords2 = NULL;
  m->colors = NULL;
  return true;
}

void ent_mesh_set_mesh(int32_t slot, int32_t count, const int32_t *x,
                       const int32_t *y, const int32_t *z,
                       const ent_mesh_Side *side, const int32_t *tile,
                       const int32_t *light, const int32_t *glow,
                       const int32_t *hidden, const int32_t *wide,
                       const int32_t *high, const bool *clear) {
  if (slot < 0 || slot >= ENT_MESH_SLOTS)
    return;
  ent_mesh_start();
  struct ent_mesh_slot *s = &ent_mesh_slots[slot];
  ent_mesh_free(&s->solid, &s->has_solid);
  ent_mesh_free(&s->clear, &s->has_clear);
  s->faces = count;
  if (count <= 0)
    return;
  s->lo = (Vector3){(float)x[0], (float)y[0], (float)z[0]};
  s->hi = s->lo;
  for (int32_t i = 1; i < count; i++) {
    s->lo = Vector3Min(s->lo, (Vector3){(float)x[i], (float)y[i], (float)z[i]});
    s->hi = Vector3Max(s->hi, (Vector3){(float)x[i], (float)y[i], (float)z[i]});
  }
  s->hi = Vector3Add(s->hi, (Vector3){1, 1, 1});
  s->has_solid = ent_mesh_build(&s->solid, count, x, y, z, side, tile, light, glow, hidden, wide, high, clear, false);
  s->has_clear = ent_mesh_build(&s->clear, count, x, y, z, side, tile, light, glow, hidden, wide, high, clear, true);
}

void ent_mesh_drop_mesh(int32_t slot) {
  if (slot < 0 || slot >= ENT_MESH_SLOTS)
    return;
  ent_mesh_free(&ent_mesh_slots[slot].solid, &ent_mesh_slots[slot].has_solid);
  ent_mesh_free(&ent_mesh_slots[slot].clear, &ent_mesh_slots[slot].has_clear);
  ent_mesh_slots[slot].faces = 0;
}

void ent_mesh_drop_meshes(void) {
  for (int32_t i = 0; i < ENT_MESH_SLOTS; i++)
    ent_mesh_drop_mesh(i);
}

// Whether a slot's box may be seen: not wholly behind the eye, nor
// wholly beyond `far`.
static bool ent_mesh_seen(const struct ent_mesh_slot *s, Camera3D eye,
                          Vector3 ahead, float far) {
  Vector3 mid = Vector3Scale(Vector3Add(s->lo, s->hi), 0.5f);
  float reach = Vector3Distance(s->lo, s->hi) * 0.5f;
  Vector3 to = Vector3Subtract(mid, eye.position);
  float d = Vector3Length(to);
  if (d - reach > far)
    return false;
  if (eye.projection == CAMERA_ORTHOGRAPHIC)
    return true;
  return Vector3DotProduct(to, ahead) > -reach;
}

void ent_mesh_draw_meshes(float far, int32_t fog) {
  ent_mesh_drawn = 0;
  ent_mesh_faces_seen = 0;
  if (!ent_mesh_ready)
    return;
  Camera3D eye = ent_scene_camera();
  Vector3 ahead = Vector3Normalize(Vector3Subtract(eye.target, eye.position));
  float fog_rgb[3] = {((fog >> 16) & 255) / 255.0f, ((fog >> 8) & 255) / 255.0f,
                      (fog & 255) / 255.0f};
  float eye_at[3] = {eye.position.x, eye.position.y, eye.position.z};
  SetShaderValue(ent_mesh_shader, ent_mesh_eye_at, eye_at, SHADER_UNIFORM_VEC3);
  SetShaderValue(ent_mesh_shader, ent_mesh_fog_at, fog_rgb, SHADER_UNIFORM_VEC3);
  SetShaderValue(ent_mesh_shader, ent_mesh_far_at, &far, SHADER_UNIFORM_FLOAT);
  SetShaderValue(ent_mesh_shader, ent_mesh_day_at, ent_mesh_day, SHADER_UNIFORM_VEC3);
  // (A tile's size on the atlas, and half a texel in from its edges, so
  // that its neighbours do not bleed in.)
  float tile = 1.0f / (float)ent_mesh_per_row;
  float inset = ent_mesh_atlas.width > 0 ? 0.5f / ent_mesh_atlas.width : 0.0f;
  SetShaderValue(ent_mesh_shader, ent_mesh_tile_at, &tile, SHADER_UNIFORM_FLOAT);
  SetShaderValue(ent_mesh_shader, ent_mesh_inset_at, &inset, SHADER_UNIFORM_FLOAT);
  // (Faces only from their front; the scene draws its boxes from both.)
  rlDrawRenderBatchActive();
  rlEnableBackfaceCulling();
  for (int32_t i = 0; i < ENT_MESH_SLOTS; i++) {
    struct ent_mesh_slot *s = &ent_mesh_slots[i];
    if (s->has_solid && ent_mesh_seen(s, eye, ahead, far)) {
      DrawMesh(s->solid, ent_mesh_material, MatrixIdentity());
      ent_mesh_drawn++;
      ent_mesh_faces_seen += s->solid.triangleCount / 2;
    }
  }
  // (Then what lets them show through, over them.)
  for (int32_t i = 0; i < ENT_MESH_SLOTS; i++) {
    struct ent_mesh_slot *s = &ent_mesh_slots[i];
    if (s->has_clear && ent_mesh_seen(s, eye, ahead, far)) {
      DrawMesh(s->clear, ent_mesh_material, MatrixIdentity());
      ent_mesh_faces_seen += s->clear.triangleCount / 2;
    }
  }
  rlDisableBackfaceCulling();
}

void ent_mesh_mesh_daylight(float part, int32_t tint) {
  float p = part < 0 ? 0 : part > 1 ? 1 : part;
  ent_mesh_day[0] = p * (float)((tint >> 16) & 255) / 255.0f;
  ent_mesh_day[1] = p * (float)((tint >> 8) & 255) / 255.0f;
  ent_mesh_day[2] = p * (float)(tint & 255) / 255.0f;
}

int32_t ent_mesh_meshes_drawn(void) { return ent_mesh_drawn; }
int32_t ent_mesh_faces_drawn(void) { return ent_mesh_faces_seen; }
