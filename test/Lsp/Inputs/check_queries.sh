#!/bin/sh
# check_queries.sh <grammar directory> <source file>: every query of the
# grammar is one of the grammar as it is. Without tree-sitter, nothing.
command -v tree-sitter >/dev/null 2>&1 || exit 0
# (The source by its whole path: the queries are asked from the grammar's
# directory.)
case "$2" in /*) source=$2 ;; *) source=$(pwd)/$2 ;; esac
cd "$1" || exit 1
for query in queries/*.scm; do
  if ! tree-sitter query "$query" "$source" >/dev/null 2>"${TMPDIR:-/tmp}/ent_query.$$"; then
    echo "$query:" >&2
    cat "${TMPDIR:-/tmp}/ent_query.$$" >&2
    rm -f "${TMPDIR:-/tmp}/ent_query.$$"
    exit 1
  fi
done
rm -f "${TMPDIR:-/tmp}/ent_query.$$"
