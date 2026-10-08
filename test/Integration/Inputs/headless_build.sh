#!/bin/sh
# Builds the window device without a window: headless_window.c, and for
# every proc and fn the device declares that it does not define, one that
# does nothing and gives nought, written from the generated declarations.
grep -E '^[A-Za-z_0-9]+ \*?ent_window_[a-z_0-9]+\(' "$ENT_INCLUDE/ent_extern.h" |
while read -r line; do
  name=$(echo "$line" | sed -E 's/^[A-Za-z_0-9]+ \*?(ent_window_[a-z_0-9]+)\(.*/\1/')
  grep -q "$name(" headless_window.c && continue
  case "$line" in
  void*) echo "${line%;} {}" ;;
  *) echo "${line%;} { return 0; }" ;;
  esac
done > headless_rest.c
printf '#include "headless_window.c"\n#include "headless_rest.c"\n' > headless_all.c
"${ENT_CC:-cc}" -O2 -I "$ENT_INCLUDE" -I . -c headless_all.c -o "$ENT_OUTPUT"
