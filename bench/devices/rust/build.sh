#!/bin/sh
# Builds the crate into the archive tools/ent asks for (ENT_OUTPUT), for
# the processor ENT_CPU names (-march=native: target-cpu=native), in a
# folder of build/ for each, and says in ENT_LINK what Rust's standard
# library needs linked.
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cpu=$(echo " $ENT_CPU" | sed -n 's/.* -m\(arch\|cpu\)=\([^ ]*\).*/\2/p')
target="$root/build/bench-devices-rust${cpu:+-$cpu}"
RUSTFLAGS="${cpu:+-C target-cpu=$cpu} $RUSTFLAGS" \
  cargo build --release --quiet --manifest-path Cargo.toml \
  --target-dir "$target"
cp "$target/release/libdev.a" "$ENT_OUTPUT"
case $(uname) in
  Darwin) echo "-lSystem -lc -lm" > "$ENT_LINK" ;;
  *) echo "-lgcc_s -lutil -lrt -lpthread -lm -ldl -lc" > "$ENT_LINK" ;;
esac
