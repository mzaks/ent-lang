#!/bin/sh
# What the port writes to the sound card's chip for the tunes of the WAD,
# against what the reference writes, value for value with their times
# (after reference/fetch.sh):
#   check/music.sh                 every tune, six minutes of each
#   check/music.sh D_E1M1 60       one tune, for so many seconds
#   SOUND=1 check/music.sh D_E1M1 60   and what the two chips make of it:
#       music.reference.raw and music.port.raw there, held against each
#       other in numbers (check/music_sound.py; the port's chip is not
#       the reference's, so the samples are not the same)
# What was written is in build/doom/run as opl.reference and opl.port.
# Needs the compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for
# tools/ent).
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run
tunes=${1:-D_E1M1 D_E1M2 D_E1M3 D_E1M4 D_E1M5 D_E1M6 D_E1M7 D_E1M8 D_E1M9 D_INTER D_INTRO D_INTROA D_VICTOR}
seconds=${2:-360}
cp "$doom/wad/doom1.wad" "$run/doom1.wad"
# (The sound card's music, as loud as the game starts with.)
printf 'snd_musicdevice 3\nmusic_volume 8\n' > "$run/music.cfg"
failed=0
for tune in $tunes; do
  (cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    DOOM_MUSIC=$tune DOOM_MUSIC_SECONDS=$seconds DOOM_OPL="$run/opl.reference" \
    "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
      -config "$run/music.cfg" > /dev/null 2>&1)
  sed "s/D_E1M1/$tune/; s/60 \* MUSIC_RATE/$seconds * MUSIC_RATE/" \
    "$here/music_check.ent" > "$run/music_check.ent"
  (cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
    run -I "$here/.." "$run/music_check.ent") > "$run/opl.port"
  printf '%s: ' "$tune"
  python3 "$here/music.py" "$run/opl.reference" "$run/opl.port" "$seconds" || failed=1
  if [ -n "$SOUND" ]; then
    (cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      DOOM_MUSIC=$tune DOOM_MUSIC_SECONDS=$seconds \
      DOOM_OPL_SAMPLES="$run/music.reference.raw" \
      "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
        -config "$run/music.cfg" > /dev/null 2>&1)
    sed "s/D_E1M1/$tune/; s/60 \* MUSIC_RATE/$seconds * MUSIC_RATE/" \
      "$here/music_render.ent" > "$run/music_render.ent"
    rm -f "$run/music.port.raw"
    (cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
      run -I "$here/.." "$run/music_render.ent")
    python3 "$here/music_sound.py" "$run/music.reference.raw" "$run/music.port.raw"
  fi
done
exit $failed
