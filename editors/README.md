# ent-lang in an editor

Three things, which the editors share:

- `tools/ent-lsp`, the language server, built with the compiler as
  `build/bin/ent-lsp`. It runs the compiler's front end on a file as it
  changes and tells the editor
  - the errors and warnings of the importer, the verifier, scheduling and
    lowering, where they are;
  - the outline of a file;
  - where a name is declared, also in an imported module (go to
    definition);
  - on hovering over a name: its declaration with the comment above it,
    and for a system what it reads and writes, whether it acts on the
    outside, and what it waits for in a schedule and why.

  It knows what a program declares at its top level (components, uniques,
  enums, relations, archetypes, systems, fns, schedules), not yet locals,
  parameters, fields or the cases of an enum. It reports the first error
  of a file, as the compiler does, and while a file does not parse it
  answers from the last version that did. An imported module is read as it
  is on disk. Modules are looked for next to the importing file, in the
  directories given with `-I`, and in `devices/` (next to `build/`, or
  where `ENT_DEVICES` says).

- `editors/tree-sitter-ent`, the grammar for tree-sitter with its queries
  (colours, outline, brackets, indentation), for Zed and the other editors
  that use tree-sitter. `src/` is generated: after changing `grammar.js`,

  ```sh
  cd editors/tree-sitter-ent && tree-sitter generate
  for f in ../../examples/*.ent ../../devices/*.ent; do tree-sitter parse -q "$f"; done
  ```

  (the second line says nothing if every file parses).

- `editors/vscode/syntaxes/ent.tmLanguage.json`, the TextMate grammar for
  VS Code's colours.

Both grammars take more than the compiler does; what is wrong is for the
language server to say.

## VS Code

```sh
cd editors/vscode
npm install
ln -s "$PWD" ~/.vscode/extensions/ent-lang     # then restart VS Code
```

The extension starts `build/bin/ent-lsp` of a folder that is open, else
`ent-lsp` from the `PATH`; `ent.server.path` and `ent.server.arguments` in
the settings say otherwise. "ent-lang: Restart the language server" is in
the command palette.

## Zed

Zed builds the extension itself, for which it needs Rust installed with
[rustup](https://rustup.rs). In `editors/zed/extension.toml`, `rev` is the
commit of this repository whose grammar Zed fetches; to try a clone as it
is, point `repository` at it (`file:///path/to/ent-lang`) and `rev` at one
of its commits. Then, in Zed: "zed: install dev extension", and choose
`editors/zed`.

The extension starts the server the settings name, else `ent-lsp` from the
`PATH`, else `build/bin/ent-lsp` of the project:

```json
{ "lsp": { "ent-lsp": { "binary": { "path": "/path/to/build/bin/ent-lsp" } } } }
```

The queries in `editors/zed/languages/ent` are copies of those in
`editors/tree-sitter-ent/queries`; a test keeps them the same.
