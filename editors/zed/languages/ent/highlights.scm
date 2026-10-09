; Colours for ent-lang. Later patterns win over earlier ones.

(comment) @comment

(integer) @number
(float) @number
(boolean) @boolean
(character) @string
(string) @string
(escape) @string.escape
(interpolation
  "{" @punctuation.special
  "}" @punctuation.special) @embedded

(identifier) @variable

; What a declaration names.
(component name: (identifier) @type)
(tag name: (identifier) @type)
(unique name: (identifier) @type)
(enum name: (identifier) @type)
(relation name: (identifier) @type)
(archetype name: (identifier) @type)
(enum_case (identifier) @constant)
(table name: (identifier) @constant)
(asset name: (identifier) @constant)
(table key: (identifier) @type)
(keyed_row case: (identifier) @constant)
; (for case in Enum: an enum by its capital; a table is a value.)
(counted_for over: (identifier) @type (#match? @type "^[A-Z]"))
(prefab name: (identifier) @function)
(prefab_use prefab: (identifier) @function)
(import module: (identifier) @namespace)
(qualified_name module: (identifier) @namespace)

(system name: (identifier) @function)
(extern_system name: (identifier) @function)
(function name: (identifier) @function)
(extern_function name: (identifier) @function)
(schedule name: (identifier) @function)

; Types where a type is expected.
(primitive_type) @type.builtin
(text_type "text" @type.builtin)
(rows_type "rows" @type.builtin)
(row_type "row" @type.builtin)
(rows_type "of" @keyword)
(row_type "of" @keyword)
(rows_type table: (identifier) @constant)
(row_type table: (identifier) @constant)
(rows_type key: (identifier) @type)
(field type: (identifier) @type)
(parameter type: (identifier) @type)
(result (identifier) @type)
(cast type: (identifier) @type)
(var type: (identifier) @type)
(unique type: (identifier) @type)

; Components, where they are named.
(binding component: (identifier) @type)
(component_init component: (identifier) @type)
(spawn_list component: (identifier) @type)
(with (identifier) @type)
(any (identifier) @type)
(without (identifier) @type)
(trigger (identifier) @type)
(archetype (identifier) @type)
(access (identifier) @type)

(field name: (identifier) @property)
(field_init name: (identifier) @property)
(field_expression field: (identifier) @property)
(parameter name: (identifier) @variable.parameter)

; A name that starts with a capital stands for something declared: a
; unique, a component, an enum.
((identifier) @type
  (#match? @type "^[A-Z]"))
; `Way.Left`: a case of an enum, or a field of a unique.
(field_expression
  value: (identifier) @type
  field: (identifier) @constant
  (#match? @type "^[A-Z]")
  (#match? @constant "^[A-Z]"))

; Calls.
(call function: (identifier) @function)
(call function: (qualified_name name: (identifier) @function))
(call function: (field_expression field: (identifier) @function.method))
(run system: (identifier) @function)
(run system: (qualified_name name: (identifier) @function))
((call function: (identifier) @function.builtin)
  (#any-of? @function.builtin "min" "max" "len"))

[
  "import" "asset" "component" "tag" "unique" "enum" "table" "prefab" "relation"
  "archetype"
  "system" "extern" "fn" "proc" "schedule" "world" "main"
  "default_capacity" "capacity" "optional" "apart" "tree"
  "sorted" "ordered" "by"
  "reads" "writes" "let" "var" "mut" "old" "spawn" "as"
  "with" "without" "any" "where" "on" "changed" "added" "removed" "log"
  "top" "down" "bottom" "up" "bfs" "dfs" "connect"
  "run_if"
] @keyword

[ "if" "else" "for" "in" "loop" "until" "while" ] @keyword.control

[
  "=" "+=" "-=" "*=" "/=" "+" "-" "*" "/" "%" "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!" "->" ".." "::"
] @operator

[ "(" ")" "[" "]" "{" "}" ] @punctuation.bracket
[ "," ":" ";" "." ] @punctuation.delimiter
