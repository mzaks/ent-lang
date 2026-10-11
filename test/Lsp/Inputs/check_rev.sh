#!/bin/sh
# check_rev.sh <editors directory>: the commit that Zed's extension.toml
# names for the grammar has the grammar as it is here. Zed builds the
# grammar from that commit but takes the queries from here, and drops
# all of them if they name what that grammar has not. Where this is no
# clone, or the commit is not in it, nothing.
command -v git >/dev/null 2>&1 || exit 0
cd "$1" || exit 1
git rev-parse --git-dir >/dev/null 2>&1 || exit 0
rev=$(sed -n 's/^rev = "\(.*\)"$/\1/p' zed/extension.toml)
[ -n "$rev" ] || { echo "zed/extension.toml names no rev" >&2; exit 1; }
git cat-file -e "$rev^{commit}" 2>/dev/null || exit 0
if ! git diff --quiet "$rev" -- tree-sitter-ent; then
  echo "editors/zed/extension.toml: rev $rev has another grammar than editors/tree-sitter-ent;" >&2
  echo "name the commit that last changed it (and push that commit)" >&2
  exit 1
fi
