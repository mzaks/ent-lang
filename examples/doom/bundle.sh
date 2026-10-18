#!/bin/sh
# Doom as one program that runs on any Mac with Apple's chips, from macOS
# 11 on, with nothing installed: the shareware WAD in it (ent build
# --embed) and raylib linked into it, built here from its source for that
# macOS (Homebrew's is for the macOS it was built on). What it uses
# besides is the system's own: Cocoa, OpenGL, CoreAudio and the like.
# Needs git, curl, cmake and what ent needs. Everything goes to
# build/doom, which is not committed.
#
#   examples/doom/bundle.sh          writes build/doom/bundle/out/doom
#
# Saved games go where it is started, as with `ent run`.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
doom=$root/build/doom
bundle=$doom/bundle
wad_sha1=5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
wad_url=https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad
# The raylib whose header window.c and sound.c are compiled against.
raylib=6.0
export MACOSX_DEPLOYMENT_TARGET=11.0

mkdir -p "$doom/wad" "$bundle"

if [ ! -f "$doom/wad/doom1.wad" ]; then
  curl -sL -o "$doom/wad/doom1.wad" "$wad_url"
fi
echo "$wad_sha1  $doom/wad/doom1.wad" | shasum -c -

if [ ! -f "$bundle/raylib-build/raylib/libraylib.a" ]; then
  rm -rf "$bundle/raylib-src"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$raylib" \
    https://github.com/raysan5/raylib.git "$bundle/raylib-src"
  cmake -S "$bundle/raylib-src" -B "$bundle/raylib-build" -Wno-dev \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_C_COMPILER=/usr/bin/cc > /dev/null
  cmake --build "$bundle/raylib-build" -j 8 > /dev/null
fi

# The library alone in a folder that is looked in first: the linker takes
# it there, not Homebrew's libraylib.dylib.
rm -rf "$bundle/lib" "$bundle/src"
mkdir -p "$bundle/lib" "$bundle/src" "$bundle/out"
cp "$bundle/raylib-build/raylib/libraylib.a" "$bundle/lib/"

# The program as doom.ent is, with a module more: it declares the WAD, to
# be put into the program, and links the frameworks raylib uses.
sed 's/^import world$/import world\
import bundle/' "$here/doom.ent" > "$bundle/src/doom.ent"
grep -q '^import bundle$' "$bundle/src/doom.ent"
cp "$doom/wad/doom1.wad" "$bundle/src/"
cat > "$bundle/src/bundle.ent" <<'EOF'
// The WAD, in the program (ent build --embed).
asset "doom1.wad"
EOF
cat > "$bundle/src/bundle.link" <<'EOF'
# raylib, linked into the program: the system's frameworks it uses.
-framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
-framework CoreAudio -framework AudioToolbox -framework CoreFoundation
EOF

LIBRARY_PATH="$bundle/lib" "$root/tools/ent" build --embed --no-cache \
  -I "$here" "$bundle/src/doom.ent" -o "$bundle/out/doom"

# Nothing but the system's: no library from Homebrew or elsewhere.
if otool -L "$bundle/out/doom" | tail -n +2 |
   grep -v -e '^\s*/System/Library/' -e '^\s*/usr/lib/'; then
  echo "bundle.sh: the program uses a library that is not the system's" >&2
  exit 1
fi
echo "doom: $bundle/out/doom"
