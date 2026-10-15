/* C that names a fn of the program: it keeps its name. */
#include <stdint.h>
int32_t ent_step(int32_t x);
int32_t internal_c_step(int32_t x) { return ent_step(x); }
