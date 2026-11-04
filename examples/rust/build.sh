#!/bin/sh
# Builds the crate (Cargo.toml) into the archive tools/ent asks for
# (ENT_OUTPUT), in build/rust, and says in ENT_LINK what Rust's standard
# library needs linked. build.rs reads ENT_INCLUDE.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cargo build --release --quiet --manifest-path "$root/examples/rust/Cargo.toml" \
  --target-dir "$root/build/rust"
cp "$root/build/rust/release/librustdev.a" "$ENT_OUTPUT"
case $(uname) in
  Darwin) echo "-lSystem -lc -lm" > "$ENT_LINK" ;;
  *) echo "-lgcc_s -lutil -lrt -lpthread -lm -ldl -lc" > "$ENT_LINK" ;;
esac
