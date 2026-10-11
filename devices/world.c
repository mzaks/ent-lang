// The world device (devices/world.ent). It is the one device that is
// given the world: saving and loading is what the compiler wrote for
// this program's world into its header.

#include "ent_world.h"

#ifdef ENT_WORLD_CANNOT_SAVE
#error "this program's world cannot be saved yet: it holds a text of any length that is not transient (see ENT_WORLD_CANNOT_SAVE in ent_world.h)"
#endif

static const char *ent_world_file(ent_world *world, char *name) {
  const ent_text126 *file = ent_world_Saved_file(world);
  memcpy(name, file->bytes, file->length);
  name[file->length] = 0;
  return name;
}

void ent_world_save_world(ent_world *world) {
  char name[127];
  *ent_world_Saved_ok(world) =
      ent_world_save(world, ent_world_file(world, name));
}

void ent_world_load_world(ent_world *world) {
  char name[127];
  // (`Saved` is transient: the name and this answer are not the file's.)
  *ent_world_Saved_ok(world) =
      ent_world_load(world, ent_world_file(world, name));
}
