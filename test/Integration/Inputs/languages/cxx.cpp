// C++ against the generated headers, which are `extern "C"`: both of them
// compile, and the functions link under their C names.
#include "ent_extern.h"
#include "ent_world.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

int32_t ent_cxx_square(int32_t n) { return n * n; }

void ent_cxx_shout(const ent_text30 *line) {
  std::string text(line->bytes, line->length);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  std::cout << text << std::endl;
}
