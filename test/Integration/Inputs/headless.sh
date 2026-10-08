#!/bin/sh
# headless.sh <directory>: makes the directory one that `import window`
# finds the window device in without a window (headless_window.c): for
# `ent run -I <directory>`. The device is built by a command of its own
# (window.build), which knows what the device declares.
here=$(dirname "$0")
rm -rf "$1"
mkdir -p "$1"
cp "$here/../../../devices/window.ent" "$1/window.ent"
cp "$here/headless_window.c" "$here/headless_build.sh" "$1/"
echo 'sh ./headless_build.sh' > "$1/window.build"
