#!/bin/sh
# The cultures the dish is calibrated against: spontaneous activity of
# dense cortical cultures on 60-electrode arrays, half an hour each, one
# recording per culture at the age nearest 28 days in vitro (25 to 31),
# 29 cultures, 62 MB; and the stimulus-evoked recordings of the same day,
# where there is one (21 cultures, 70 MB: the spikes and the stims). From
# the Potter lab's archive:
#
#   Wagenaar DA, Pine J, Potter SM (2006). An extremely rich repertoire of
#   bursting patterns during the development of cortical cultures. BMC
#   Neuroscience 7:11. https://doi.org/10.1186/1471-2202-7-11
#   https://potterlab.bme.gatech.edu/development-data/
#
# The archive asks that work using the data cite that article. Into
# build/culture/raw and build/culture/stim/raw (or under the folder given).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=${1:-$here/../../../build/culture}
to=$top/raw
stim=$top/stim/raw
mkdir -p "$to" "$stim"
base=https://potterlab.bme.gatech.edu/development-data/simple-text/daily
while read -r name; do
  [ -s "$to/$name.spk.txt.bz2" ] || curl -sSf -o "$to/$name.spk.txt.bz2" "$base/spont/dense/$name.spk.txt.bz2"
  for kind in spk tri; do
    [ -s "$stim/$name.$kind.txt.bz2" ] && continue
    # (Not every culture has a stimulus-evoked recording that day.)
    curl -sf -o "$stim/$name.$kind.txt.bz2" "$base/stim/dense/$name.$kind.txt.bz2" || rm -f "$stim/$name.$kind.txt.bz2"
  done
done <<'LIST'
1-1-25
1-2-25
1-3-25
1-4-25
1-5-25
2-1-28
2-2-26
2-3-28
2-4-28
2-5-28
2-6-28
3-1-27
3-2-27
3-3-27
3-4-26
3-5-26
3-6-26
4-1-27
4-2-27
5-1-27
5-2-27
5-3-27
6-1-28
6-3-28
7-1-31
7-2-31
8-1-28
8-2-28
8-3-28
LIST
echo "$(ls "$to" | wc -l) spontaneous recordings in $to, $(ls "$stim" | grep -c spk) evoked in $stim"
