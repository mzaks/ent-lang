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
        $.enum,
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
      ),

    import: ($) => seq('import', field('module', $.identifier)),

    default_capacity: ($) => seq('default_capacity', $.integer),
    capacity_of: ($) => seq('capacity', field('of', $._name), $.integer),

    capacity: ($) => seq('capacity', $.integer),

    fields: ($) => seq('{', optional(list($.field)), '}'),
    field: ($) => seq(field('name', $.identifier), ':', field('type', $._type)),

    component: ($) =>
      seq('component', field('name', $.identifier), $.fields,
          optional($.capacity)),

    tag: ($) => seq('tag', field('name', $.identifier), optional($.capacity)),

    unique: ($) =>
      seq('unique', field('name', $.identifier),
          choice($.fields, seq(':', field('type', $._type)))),

    enum: ($) =>
      seq('enum', field('name', $.identifier), '{',
          optional(list($.enum_case)), '}'),
    enum_case: ($) => $.identifier,

    relation: ($) =>
      seq('relation', field('name', $.identifier), optional($.fields),
          optional(seq('from', $._name)), optional(seq('to', $._name)),
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
      seq('fn', field('name', $.identifier), $.parameters, $.result,
          field('body', $.block)),

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

    _type: ($) => choice($.primitive_type, $.text_type, $._name),
    primitive_type: (_) =>
      choice('f32', 'f64', 'bool', 'i1', 'i8', 'i16', 'i32', 'i64', 'index',
             'entity'),
    text_type: ($) => seq('text', '[', $.integer, ']'),

    // Statements

    block: ($) =>
      seq('{', repeat(seq($._statement, optional(';'))), '}'),

    _statement: ($) =>
      choice(
        $.let,
        $.var,
        $.counted_for,
        $.query_for,
        $.edges_for,
        $.assignment,
        $._expression,
      ),

    _pattern: ($) =>
      choice(field('name', $.identifier),
             seq('(', list(field('name', $.identifier)), ')')),

    let: ($) => seq('let', $._pattern, '=', field('value', $._expression)),

    var: ($) =>
      seq('var', $._pattern, optional(seq(':', field('type', $._type))), '=',
          field('value', $._expression)),

    counted_for: ($) =>
      seq('for', field('counter', $.identifier), 'in',
          field('from', $._expression), '..', field('to', $._expression),
          field('body', $.block)),

    query_for: ($) =>
      seq('for',
          optional(list(choice($.binding, field('entity', $.identifier)))),
          repeat(choice($.with, $.without)),
          optional($.cascade),
          optional($.where),
          optional($.on),
          field('body', $.block)),
    binding: ($) =>
      seq(optional('optional'), field('name', $.identifier), ':',
          optional('mut'), field('component', $._name),
          optional(seq(choice('up', 'before'), field('via', $._name)))),
    with: ($) => seq('with', list(choice($._name, $.any))),
    any: ($) => seq('any', '(', list($._name), ')'),
    without: ($) => seq('without', list($._name)),
    cascade: ($) =>
      seq('cascade', field('along', $._name),
          optional(seq('leaves', 'first'))),
    where: ($) => seq('where', $._expression),
    on: ($) => seq('on', list($.trigger)),
    trigger: ($) =>
      seq(choice('changed', 'added', 'removed'), $._name,
          optional(seq('.', $.identifier)),
          optional(seq('up', field('via', $._name))),
          optional(seq('log', $.integer))),

    edges_for: ($) =>
      seq('for', optional('mut'), field('edge', $.identifier), ',',
          field('other', $.identifier), 'in', field('entity', $.identifier),
          '.', field('direction', choice('out', 'in')), '(', $._name, ')',
          field('body', $.block)),

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
                     field('value', $._expression))),

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
    spawn: ($) =>
      seq('spawn', '{',
          optional(list(choice($.component_init, field('component', $._name)))),
          '}'),
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
