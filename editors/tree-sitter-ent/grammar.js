// The syntax of ent-lang (docs/syntax.md) for tree-sitter: what editors
// colour and fold by. It follows lib/Ent/Import/Parser.cpp, but takes more
// than the compiler does: what is wrong is the compiler's to say.

const PREC = {
  or: 1,
  and: 2,
  equal: 3,
  compare: 4,
  add: 5,
  multiply: 6,
  unary: 7,
  cast: 8,
  postfix: 9,
};

const list = (rule) => seq(rule, repeat(seq(',', rule)), optional(','));

module.exports = grammar({
  name: 'ent',

  extras: ($) => [/\s/, $.comment],
  word: ($) => $.identifier,
  // `capacity` after a declaration is its own (`... capacity 64`) or
  // starts the next one (`capacity Name 64`): what follows it tells.
  conflicts: ($) => [[$.tag], [$.component], [$.relation], [$.archetype]],

  rules: {
    source_file: ($) => repeat($._declaration),

    comment: (_) => token(seq('//', /[^\n]*/)),

    _declaration: ($) =>
      choice(
        $.import,
        $.component,
        $.tag,
        $.unique,
        $.buffer,
        $.enum,
        $.table,
        $.prefab,
        $.relation,
        $.archetype,
        $.system,
        $.extern_system,
        $.function,
        $.extern_function,
        $.schedule,
        $.world,
        $.main,
        $.default_capacity,
        $.capacity_of,
        $.asset,
      ),

    import: ($) => seq('import', field('module', $.identifier)),

    // asset "name.ttf": a file the program needs when it runs.
    // asset face = "name.ttf": by a name; with a `*` in the file's name,
    // the name is a list of the files there are.
    asset: ($) =>
      seq('asset', optional(seq(field('name', $.identifier), '=')),
          field('file', $.string)),

    default_capacity: ($) => seq('default_capacity', $.integer),
    capacity_of: ($) => seq('capacity', field('of', $._name), $.integer),

    capacity: ($) => seq('capacity', $.integer),

    fields: ($) => seq('{', optional(list($.field)), '}'),
    field: ($) => seq(field('name', $.identifier), ':', field('type', $._type)),

    component: ($) =>
      seq('component', field('name', $.identifier), $.fields,
          optional('apart'), optional($.capacity)),

    tag: ($) =>
      seq('tag', field('name', $.identifier), optional('apart'),
          optional($.capacity)),

    // buffer Name { field: type, .. } capacity N
    buffer: ($) =>
      seq('buffer', field('name', $.identifier), $.fields, $.capacity),

    unique: ($) =>
      seq('unique', field('name', $.identifier),
          choice($.fields, seq(':', field('type', $._type)))),

    enum: ($) =>
      seq('enum', field('name', $.identifier), '{',
          optional(list($.enum_case)), '}'),

    // table name { field: type, .. } = [ { field: value, .. }, .. ]
    // table name: type = [ value, .. ]
    // table name[Enum] .. = [ Case: row or value, .. ]
    table: ($) =>
      seq('table', field('name', $.identifier),
          optional(seq('[', field('key', $._name), ']')),
          choice($.fields, seq(':', field('type', $._type))), '=', '[',
          optional(list(choice($.row, $._expression, $.keyed_row))), ']'),
    row: ($) => seq('{', optional(list($.field_init)), '}'),
    keyed_row: ($) =>
      seq(field('case', $.identifier), ':', choice($.row, $._expression)),

    // prefab name(parameter: type, ..) { what a spawn lists }
    prefab: ($) =>
      seq('prefab', field('name', $.identifier), $.parameters,
          $.spawn_list),
    enum_case: ($) => $.identifier,

    // relation Name { fields } ..., or with what its ends have:
    // relation (Source)-[Name { fields }]->(Target) ...
    relation: ($) =>
      seq('relation',
          choice(seq(field('name', $.identifier), optional($.fields)),
                 seq($.relation_end, '-', '[', field('name', $.identifier),
                     optional($.fields), ']', '-', '>', $.relation_end)),
          optional(seq('tree', optional('sorted'),
                       optional(seq('ordered', 'by', $._name, '.',
                                    $.identifier)))),
          optional($.capacity)),

    archetype: ($) =>
      seq('archetype', field('name', $.identifier), '{',
          optional(list(seq(optional('optional'), $._name))), '}',
          optional($.capacity)),

    parameters: ($) => seq('(', optional(list($.parameter)), ')'),
    parameter: ($) =>
      seq(field('name', $.identifier), ':', field('type', $._type)),

    access: ($) =>
      repeat1(choice(seq('reads', list($._name)), seq('writes', list($._name)))),

    system: ($) =>
      seq('system', field('name', $.identifier), $.parameters,
          optional($.access), field('body', $.block)),

    extern_system: ($) =>
      prec.right(seq('extern', 'system', field('name', $.identifier),
                     $.parameters, optional($.access), optional(';'))),

    result: ($) =>
      seq('->', choice($._type, seq('(', list($._type), ')'))),

    function: ($) =>
      seq(choice('fn', 'proc'), field('name', $.identifier), $.parameters,
          optional($.result), field('body', $.block)),

    extern_function: ($) =>
      prec.right(seq('extern', choice('fn', 'proc'),
                     field('name', $.identifier), $.parameters,
                     optional($.result), optional(';'))),

    schedule: ($) =>
      seq('schedule', field('name', $.identifier), $.parameters,
          optional($.run_if), field('body', $.schedule_block)),
    schedule_block: ($) => seq('{', repeat($.run), '}'),
    run: ($) =>
      prec.right(seq(field('system', $._name), $.arguments,
                     optional($.run_if), optional(';'))),
    run_if: ($) => seq('run_if', $._expression),

    world: ($) => seq('world', field('body', $.block)),

    main: ($) => seq('main', field('body', $.main_block)),
    main_block: ($) => seq('{', repeat(choice($.run, $.loop)), '}'),
    loop: ($) =>
      prec.right(seq('loop', $.main_block,
                     optional(seq('until', $._expression)))),

    // Types

    _type: ($) =>
      choice($.primitive_type, $.text_type, $.callable_type, $.rows_type,
             $.row_type,
             $._name),
    // fn(t: f32) -> f32, proc(i32): a fn or proc of that shape, as a value.
    callable_type: ($) =>
      prec.right(seq(choice('fn', 'proc'), '(',
                     optional(list(choice(
                         seq(field('name', $.identifier), ':', $._type),
                         $._type))),
                     ')', optional(seq('->', $._type)))),
    primitive_type: (_) =>
      choice('f32', 'f64', 'bool', 'i1', 'i8', 'i16', 'i32', 'i64', 'index',
             'entity'),
    // rows { field: type, .. }, rows type, rows[Enum] ..: a table of
    // that shape, as a value.
    rows_type: ($) =>
      prec.right(seq('rows',
                     choice(seq('of', field('table', $._name)),
                            seq(optional(seq('[', field('key', $._name), ']')),
                                choice($.fields, $._type))))),
    // row { x: f32, y: f32 }: a row of one of the tables with such rows.
    // rows of road, row of road: with rows as those of that table.
    row_type: ($) =>
      seq('row', choice($.fields, seq('of', field('table', $._name)))),
    // text: of any length; text[N]: up to N bytes.
    text_type: ($) =>
      prec.right(seq('text', optional(seq('[', $.integer, ']')))),

    // Statements

    block: ($) =>
      seq('{', repeat(seq($._statement, optional(';'))), '}'),

    _statement: ($) =>
      choice(
        $.let,
        $.var,
        $.counted_for,
        // (As a statement rather than as a value that is dropped.)
        prec(1, $.query_for),
        $.until_loop,
        $.while_loop,
        $.connect,
        $.assignment,
        $._expression,
      ),

    _pattern: ($) =>
      choice(field('name', $.identifier),
             seq('(', list(field('name', $.identifier)), ')')),

    // (The value may be a `for` over entities: how many its body ran
    // for.)
    let: ($) =>
      seq('let', $._pattern, '=', field('value', $._expression)),

    var: ($) =>
      seq('var', $._pattern, optional(seq(':', field('type', $._type))), '=',
          field('value', $._expression)),

    relation_end: ($) => seq('(', optional($._name), ')'),

    // (Or `for case in Enum { }`: every case of an enum; `for row in
    // table { }`: every row of a table, or of the one a value holds.)
    counted_for: ($) =>
      seq('for',
          optional(seq(field('number', $.identifier), ',')),
          field('counter', $.identifier), 'in',
          choice(seq(field('from', $._expression), '..',
                     field('to', $._expression)),
                 field('over', $._expression)),
          field('body', $.block)),

    // loop { ... } until done: the statements, until the condition holds
    // after them.
    // while going { ... }
    while_loop: ($) =>
      seq('while', field('condition', $._expression), field('body', $.block)),
    until_loop: ($) =>
      seq('loop', field('body', $.block), 'until', $._expression),
    // for e, a: A, b: mut B ... { }: the entity's own. With arrows, the
    // head is patterns: for (a: A)-[R]->(outer: B), optional
    // (prev: A)~[R]~>(a) ... { }. Inside a `for`, a pattern is a loop
    // over the visited entity's edges.
    query_for: ($) =>
      seq('for',
          optional(choice(
              list(choice($.binding, field('entity', $.identifier))),
              list($.pattern))),
          repeat(choice($.with, $.without)),
          optional($.cascade),
          optional($.where),
          optional($.on),
          field('body', $.block)),
    // (`optional`: a component the entity may be without.)
    // (`old`, in a `for` inside a `for`: as it was before the outer one.)
    binding: ($) =>
      seq(optional('optional'), field('name', $.identifier), ':',
          optional(choice('mut', 'old')), field('component', $._name)),
    // (name), (e, a: A), (: C.f), (expr): an entity and what is bound of
    // it; in `connect`, an entity by any expression.
    node: ($) =>
      seq('(',
          optional(choice(
              list(choice($.binding, $._expression)),
              seq(':', field('component', $._name),
                  optional(seq('.', field('field', $.identifier))))),
          ),
          ')'),
    // -[R]-> and <-[R]-, an edge; ~[R]~> and <~[R]~, to the sibling
    // after. In the brackets: [mut] [name:] Relation [*] [{ fields }].
    arrow: ($) =>
      choice(seq('-', $._edge, '-', '>'), seq('<', '-', $._edge, '-'),
             seq('~', $._edge, '~', '>'), seq('<', '~', $._edge, '~')),
    _edge: ($) =>
      seq('[', optional('mut'),
          optional(seq(field('edge', $.identifier), ':')),
          field('relation', $._name), optional('*'),
          optional(seq('{', optional(list($.field_init)), '}')), ']'),
    pattern: ($) =>
      prec.right(
          seq(optional('optional'), $.node, repeat(seq($.arrow, $.node)))),
    connect: ($) => seq('connect', $.pattern),
    with: ($) => seq('with', list(choice($._name, $.any))),
    any: ($) => seq('any', '(', list($._name), ')'),
    without: ($) => seq('without', list($._name)),
    // top down R, bottom up R: along a tree; with bfs or dfs, in exactly
    // that order.
    cascade: ($) =>
      seq(choice(seq('top', 'down'), seq('bottom', 'up')),
          optional(choice('bfs', 'dfs')), field('along', $._name)),
    where: ($) => seq('where', $._expression),
    on: ($) => seq('on', list($.trigger)),
    // changed Component.field, changed binding.field, and of a child:
    // changed (: C.f)-[R]->(name)
    trigger: ($) =>
      seq(choice('changed', 'added', 'removed'),
          choice(seq($._name, optional(seq('.', $.identifier))),
                 seq($.node, $.arrow, $.node)),
          optional(seq('log', $.integer))),

    if_let: ($) =>
      seq('let', field('name', $.identifier), '=',
          field('value', $._expression),
          repeat(seq(',', 'let', field('name', $.identifier), '=',
                     field('value', $._expression)))),

    assignment: ($) =>
      prec.right(seq(field('target', $._expression),
                     field('operator',
                           choice('=', '+=', '-=', '*=', '/=', seq('min', '='),
                                  seq('max', '='))),
                     // (A buffer is given a row, or none.)
                     field('value',
                           choice($._expression, $.row, $.no_rows)))),
    no_rows: ($) => seq('[', ']'),

    // Expressions

    _expression: ($) =>
      choice(
        $.integer,
        $.float,
        $.boolean,
        $.string,
        $.character,
        $._name,
        $.field_expression,
        $.call,
        $.index,
        $.unary,
        $.binary,
        $.cast,
        $.if_expression,
        $.spawn,
        $.tuple,
        $.parenthesized,
        // (A `for` over entities, where a statement's value is worked
        // out: how many its body ran for.)
        $.query_for,
      ),

    _name: ($) => choice($.identifier, $.qualified_name),
    qualified_name: ($) =>
      seq(field('module', $.identifier), '::', field('name', $.identifier)),

    field_expression: ($) =>
      prec(PREC.postfix,
           seq(field('value', $._expression), '.',
               field('field', $.identifier))),

    arguments: ($) =>
      seq('(', optional(list(choice($._expression, $.component_init))), ')'),
    call: ($) =>
      prec(PREC.postfix,
           seq(field('function', $._expression), $.arguments)),

    index: ($) =>
      prec(PREC.postfix,
           seq(field('value', $._expression), '[', $._expression, ']')),

    unary: ($) =>
      prec(PREC.unary, seq(choice('-', '!'), $._expression)),

    binary: ($) =>
      choice(
        ...[
          ['||', PREC.or],
          ['&&', PREC.and],
          ['==', PREC.equal],
          ['!=', PREC.equal],
          ['<', PREC.compare],
          ['<=', PREC.compare],
          ['>', PREC.compare],
          ['>=', PREC.compare],
          ['+', PREC.add],
          ['-', PREC.add],
          ['*', PREC.multiply],
          ['/', PREC.multiply],
          ['%', PREC.multiply],
        ].map(([operator, precedence]) =>
          prec.left(precedence,
                    seq(field('left', $._expression),
                        field('operator', operator),
                        field('right', $._expression))))),

    cast: ($) =>
      prec.left(PREC.cast,
                seq(field('value', $._expression), 'as',
                    field('type', $._type))),

    // One `if` for both: the statement, and the one that gives a value,
    // whose branches end with it. Which it is, is the compiler's to say.
    if_expression: ($) =>
      prec.right(seq('if', choice($._expression, $.if_let), $.block,
                     optional(seq('else', choice($.block, $.if_expression))))),

    // In a spawn a component without fields is its name alone; so it is as
    // an argument (`e.add(Shield)`), where it is an expression like any.
    // (In its list also a prefab with what it is given, and an `if`
    // that picks, which needs no comma after it.)
    spawn: ($) => seq('spawn', $.spawn_list),
    spawn_list: ($) =>
      seq('{', repeat(seq($._spawn_entry, optional(','))), '}'),
    _spawn_entry: ($) =>
      choice($.component_init, field('component', $._name), $.prefab_use,
             $.spawn_if),
    prefab_use: ($) => seq(field('prefab', $._name), $.arguments),
    spawn_if: ($) =>
      prec.right(seq('if', $._expression, $.spawn_list,
                     optional(seq('else', choice($.spawn_list, $.spawn_if))))),
    component_init: ($) =>
      seq(field('component', $._name), '{', optional(list($.field_init)), '}'),
    field_init: ($) =>
      seq(field('name', $.identifier), ':', field('value', $._expression)),

    tuple: ($) =>
      seq('(', $._expression, repeat1(seq(',', $._expression)), ')'),
    parenthesized: ($) => seq('(', $._expression, ')'),

    // Words

    identifier: (_) => /[A-Za-z_][A-Za-z0-9_]*/,
    integer: (_) => token(choice(/[0-9]+/, /0[xX][0-9a-fA-F]+/)),
    float: (_) =>
      token(choice(/[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/, /[0-9]+[eE][+-]?[0-9]+/)),
    boolean: (_) => choice('true', 'false'),
    character: (_) => token(seq("'", choice(/[^'\\\n]/, /\\x[0-9a-fA-F]{2}/, /\\./), "'")),

    string: ($) =>
      seq('"',
          repeat(choice($.string_content, $.escape, $.interpolation)),
          token.immediate('"')),
    string_content: (_) => token.immediate(prec(1, /[^"\\{}\n]+/)),
    escape: (_) => token.immediate(choice(/\\x[0-9a-fA-F]{2}/, /\\./)),
    interpolation: ($) => seq(token.immediate('{'), $._expression, '}'),
  },
});
