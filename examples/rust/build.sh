#!/bin/sh
# Builds the crate (Cargo.toml) into the archive tools/ent asks for
# (ENT_OUTPUT), in build/rust, and says in ENT_LINK what Rust's standard
# library needs linked. build.rs reads ENT_INCLUDE. Made for the processor
# ENT_CPU names (-march=native: target-cpu=native), as the program is, in a
# folder of its own for each, so that one does not undo the other.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cpu=$(echo " $ENT_CPU" | sed -n 's/.* -m\(arch\|cpu\)=\([^ ]*\).*/\2/p')
target="$root/build/rust${cpu:+-$cpu}"
RUSTFLAGS="${cpu:+-C target-cpu=$cpu} $RUSTFLAGS" \
  cargo build --release --quiet \
  --manifest-path "$root/examples/rust/Cargo.toml" --target-dir "$target"
cp "$target/release/librustdev.a" "$ENT_OUTPUT"
case $(uname) in
  Darwin) echo "-lSystem -lc -lm" > "$ENT_LINK" ;;
  *) echo "-lgcc_s -lutil -lrt -lpthread -lm -ldl -lc" > "$ENT_LINK" ;;
esac
