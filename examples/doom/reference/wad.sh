#!/bin/sh
# Puts the shareware WAD (Doom v1.9) in build/doom/wad/doom1.wad, if it is
# not there: from id Software's own shareware release, doom19s.zip, as
# the idgames archive keeps it (idstuff/doom), from the first of its
# mirrors that has it. The two parts in that zip, put together, are a zip
# with DOOM1.WAD in it. Both the release and the WAD are checked against
# their SHA-1s. Needs curl and unzip. fetch.sh and ../bundle.sh run it.
#
#   examples/doom/reference/wad.sh
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
wad=$root/build/doom/wad
zip_sha1=8d0fbbbeba5ecb692a99f97e55dfb5365cfe5b77
wad_sha1=5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
mirrors="https://youfailit.net/pub/idgames
https://ftpmirror1.infania.net/pub/idgames"

if command -v sha1sum > /dev/null; then sha1="sha1sum"; else sha1="shasum"; fi

mkdir -p "$wad"
if [ ! -f "$wad/doom1.wad" ]; then
  work=$wad/release
  rm -rf "$work" && mkdir -p "$work"
  for mirror in $mirrors; do
    if curl -sfL -o "$work/doom19s.zip" "$mirror/idstuff/doom/doom19s.zip" &&
       echo "$zip_sha1  $work/doom19s.zip" | $sha1 -c - > /dev/null 2>&1; then
      break
    fi
    rm -f "$work/doom19s.zip"
  done
  if [ ! -f "$work/doom19s.zip" ]; then
    echo "wad.sh: no mirror gave doom19s.zip" >&2
    exit 1
  fi
  unzip -q -o "$work/doom19s.zip" DOOMS_19.1 DOOMS_19.2 -d "$work"
  cat "$work/DOOMS_19.1" "$work/DOOMS_19.2" > "$work/dooms19.zip"
  unzip -q -o -p "$work/dooms19.zip" DOOM1.WAD > "$wad/doom1.wad.part"
  mv "$wad/doom1.wad.part" "$wad/doom1.wad"
  rm -rf "$work"
fi
echo "$wad_sha1  $wad/doom1.wad" | $sha1 -c -
