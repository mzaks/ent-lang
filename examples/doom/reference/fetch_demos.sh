#!/bin/sh
# More demos to play the port against the reference with: the records
# of the first episode's levels from the Doom Speed Demos Archive
# (dsdarchive.com), into build/doom/demos. They are their players'
# recordings and are not part of this repository; check/demos.sh plays
# them. Needs curl, unzip and python3.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
into=$root/build/doom/demos
mkdir -p "$into"
for map in 1 2 3 4 5 6 7 8 9; do
  for category in "UV Speed" "UV Max" "UV Tyson" "Pacifist" "NoMo" "NoMo 100S" "Stroller"; do
    name=e1m${map}_$(echo "$category" | tr 'A-Z ' 'a-z_')
    [ -f "$into/$name.lmp" ] && continue
    url=$(curl -s -m 30 -G "https://dsdarchive.com/api/demos/records" \
      --data-urlencode "wad=doom" --data-urlencode "level=E1M$map" \
      --data-urlencode "category=$category" |
      python3 -I -c 'import json,sys
try: print(json.load(sys.stdin)["file"])
except Exception: pass')
    [ -z "$url" ] && continue
    rm -rf "$into/tmp" && mkdir -p "$into/tmp"
    curl -s -L -m 60 -o "$into/tmp/demo.zip" "$url" || continue
    unzip -q -o "$into/tmp/demo.zip" -d "$into/tmp" 2> /dev/null || continue
    chmod -R u+w "$into/tmp"
    lump=$(find "$into/tmp" -iname '*.lmp' | head -1)
    [ -n "$lump" ] && cp "$lump" "$into/$name.lmp" && echo "$name: $url"
  done
done
# And runs through the whole episode, which go through the screens
# between the levels: ep1_uv_max through all nine, ep1_nomo through
# eight. (ep1_uv_speed was recorded with another WAD's first level and
# never leaves it here; the port and the reference still agree on it.)
for category in "UV Speed" "UV Max" "NoMo"; do
  name=ep1_$(echo "$category" | tr 'A-Z ' 'a-z_')
  [ -f "$into/$name.lmp" ] && continue
  url=$(curl -s -m 30 -G "https://dsdarchive.com/api/demos/records" \
    --data-urlencode "wad=doom" --data-urlencode "level=Episode 1" \
    --data-urlencode "category=$category" |
    python3 -I -c 'import json,sys
try: print(json.load(sys.stdin)["file"])
except Exception: pass')
  [ -z "$url" ] && continue
  rm -rf "$into/tmp" && mkdir -p "$into/tmp"
  curl -s -L -m 60 -o "$into/tmp/demo.zip" "$url" || continue
  unzip -q -o "$into/tmp/demo.zip" -d "$into/tmp" 2> /dev/null || continue
  chmod -R u+w "$into/tmp"
  lump=$(find "$into/tmp" -iname '*.lmp' | head -1)
  [ -n "$lump" ] && cp "$lump" "$into/$name.lmp" && echo "$name: $url"
done
rm -rf "$into/tmp"
