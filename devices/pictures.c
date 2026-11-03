// The C of the module `pictures` (devices/pictures.ent).
#include "ent_extern.h"
#include "ent_files.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// (Static: raylib, linked in with the window, has stb_image's functions
// under the same names.)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// The first `width` times `height` colours as bytes, red, green, blue.
static unsigned char *ent_pictures_rgb(int32_t count, const int32_t *color,
                                       int32_t width, int32_t height) {
  if (width <= 0 || height <= 0 || (int64_t)width * height > count)
    return 0;
  unsigned char *rgb = malloc((size_t)width * (size_t)height * 3);
  if (!rgb)
    return 0;
  for (int64_t i = 0; i < (int64_t)width * height; i++) {
    rgb[3 * i] = (unsigned char)(color[i] >> 16);
    rgb[3 * i + 1] = (unsigned char)(color[i] >> 8);
    rgb[3 * i + 2] = (unsigned char)color[i];
  }
  return rgb;
}

static void ent_pictures_path(char *path, const ent_text126 *name) {
  memcpy(path, name->bytes, name->length);
  path[name->length] = 0;
}

int32_t ent_pictures_read_picture(const ent_text126 *name, int32_t *count,
                                  int32_t capacity, int32_t *color) {
  char path[127];
  struct ent_file file;
  ent_pictures_path(path, name);
  *count = 0;
  if (!ent_file_open(path, &file))
    return -1;
  int width = 0, height = 0, channels = 0;
  unsigned char *rgb = 0;
  if (file.size <= INT32_MAX)
    rgb = stbi_load_from_memory(file.bytes, (int)file.size, &width, &height,
                                &channels, 3);
  ent_file_close(&file);
  if (!rgb)
    return -1;
  if ((int64_t)width * height > capacity) {
    stbi_image_free(rgb);
    return -2;
  }
  int32_t n = width * height;
  for (int32_t i = 0; i < n; i++)
    color[i] = (int32_t)rgb[3 * i] << 16 | (int32_t)rgb[3 * i + 1] << 8 |
               (int32_t)rgb[3 * i + 2];
  stbi_image_free(rgb);
  *count = n;
  return width;
}

bool ent_pictures_write_png(const ent_text126 *name, int32_t count,
                            const int32_t *color, int32_t width,
                            int32_t height) {
  char path[127];
  unsigned char *rgb = ent_pictures_rgb(count, color, width, height);
  if (!rgb)
    return false;
  ent_pictures_path(path, name);
  int written = stbi_write_png(path, width, height, 3, rgb, width * 3);
  free(rgb);
  return written != 0;
}

bool ent_pictures_write_jpg(const ent_text126 *name, int32_t count,
                            const int32_t *color, int32_t width,
                            int32_t height, int32_t quality) {
  char path[127];
  unsigned char *rgb = ent_pictures_rgb(count, color, width, height);
  if (!rgb)
    return false;
  ent_pictures_path(path, name);
  int q = quality < 1 ? 1 : quality > 100 ? 100 : quality;
  int written = stbi_write_jpg(path, width, height, 3, rgb, q);
  free(rgb);
  return written != 0;
}
