//===- Parser.cpp - ent-lang source to the ent dialect --------------------===//
//
// A recursive-descent parser for `.ent` files that builds `ent` IR as it
// goes. Declarations and statements become ops directly; expressions are
// parsed into a small tree first, so that a literal can take the type of
// the operand it meets (`x * 2` with `x: f32` makes `2` an f32).
//
//===----------------------------------------------------------------------===//

#include "Ent/Import.h"
#include "Lexer.h"

#include "Ent/EntDialect.h"
#include "Ent/EntOps.h"
#include "Ent/World.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/SourceMgr.h"

#include <functional>
#include <limits>

#include <memory>
#include <optional>

using namespace mlir;
using namespace mlir::ent;

static llvm::cl::list<std::string> importDirs(
    "I", llvm::cl::desc("Directory to search for imported ent-lang modules, "
                        "after the importing file's own"),
    llvm::cl::value_desc("dir"), llvm::cl::Prefix);

namespace {

/// One `.ent` file of a program. The file the compiler is given is the
/// root; its declarations keep their names. Those of an imported module
/// `m` are named `m.Name` in the IR.
struct SourceModule {
  std::string name;
  std::string prefix;
  /// Everything it declares, by the name written in its source.
  llvm::StringSet<> names;
  /// The modules it imports itself; their names are in scope in it.
  SmallVector<SourceModule *> imports;
  /// Still being parsed: importing it again is a cycle.
  bool loading = true;
  /// The schedule that runs its `world` block, if it has one.
  std::string worldSchedule;
  llvm::StringSet<> reported;
};

/// What the declarations of one kind (components, uniques, ...) are known
/// by. Keys are IR symbol names; a name as written in the source is
/// resolved against the module being parsed first.
template <typename T>
class Declared {
public:
  using iterator = typename llvm::StringMap<T>::iterator;
  explicit Declared(std::function<std::string(StringRef)> resolve)
      : resolve(std::move(resolve)) {}
  iterator begin() { return map.begin(); }
  iterator end() { return map.end(); }
  iterator find(StringRef name) { return map.find(resolve(name)); }
  size_t count(StringRef name) { return map.count(resolve(name)); }
  T &operator[](StringRef name) { return map[resolve(name)]; }
  bool try_emplace(StringRef name) {
    return map.try_emplace(resolve(name)).second;
  }
  /// The declaration with this IR symbol name, as it is.
  T &ofSymbol(StringRef symbol) { return map[symbol]; }

private:
  std::function<std::string(StringRef)> resolve;
  llvm::StringMap<T> map;
};

/// Of two whole-number types (not a bool's), the one of more bits; null
/// if either is something else.
static Type widerInteger(Type a, Type b) {
  auto x = dyn_cast_or_null<IntegerType>(a);
  auto y = dyn_cast_or_null<IntegerType>(b);
  if (!x || !y || x.getWidth() == 1 || y.getWidth() == 1)
    return {};
  // (One with a sign and one without are not computed with together.)
  if (x.isUnsigned() != y.isUnsigned())
    return {};
  return x.getWidth() >= y.getWidth() ? a : b;
}

/// The operator an assignment computes with (`+` for `+=`); Eof for `=`
/// and for what is no assignment.
static Token::Kind computedBy(Token::Kind assign) {
  switch (assign) {
  case Token::PlusAssign:
    return Token::Plus;
  case Token::MinusAssign:
    return Token::Minus;
  case Token::StarAssign:
    return Token::Star;
  case Token::SlashAssign:
    return Token::Slash;
  case Token::PercentAssign:
    return Token::Percent;
  case Token::AmpAssign:
    return Token::Amp;
  case Token::PipeAssign:
    return Token::Pipe;
  case Token::CaretAssign:
    return Token::Caret;
  case Token::ShiftLeftAssign:
    return Token::ShiftLeft;
  case Token::ShiftRightAssign:
    return Token::ShiftRight;
  default:
    return Token::Eof;
  }
}

/// A whole number without a sign: u8, u16, u32 or u64.
static bool isUnsigned(Type type) {
  auto whole = dyn_cast_or_null<IntegerType>(type);
  return whole && whole.isUnsigned();
}

/// A whole number, with a sign or without (not a bool).
static bool isWhole(Type type) {
  auto whole = dyn_cast_or_null<IntegerType>(type);
  return whole && whole.getWidth() != 1;
}

/// What a whole number is computed with: one without a sign as its bits,
/// an integer of as many (the operations say that it has none).
static Type bitsType(Type type) {
  if (!isUnsigned(type))
    return type;
  return IntegerType::get(type.getContext(),
                          cast<IntegerType>(type).getWidth());
}

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

/// `Position { x: 1.0 }` in a spawn or an add.
struct ComponentInit {
  std::string component;
  llvm::SMLoc loc;
  SmallVector<std::pair<std::string, ExprPtr>> fields;
};

/// What a `spawn` lists, or a prefab: a component with its values, a
/// prefab with what it is given, or an `if` that says which of two lists
/// it is.
struct SpawnEntry {
  enum Kind { Init, Prefab, If };
  Kind kind = Init;
  llvm::SMLoc loc;
  ComponentInit init;
  std::string prefab;
  std::vector<ExprPtr> args;
  ExprPtr condition;
  std::vector<SpawnEntry> then, otherwise;
};

/// `let a = value`, or `let (a, b) = values` for what a fn gives.
struct Let {
  SmallVector<std::string> names;
  ExprPtr value;
  /// Written with parentheses: takes several values apart.
  bool several = false;
};

/// A branch of an if-expression: `{ let a = ...; value }`.
struct Branch {
  SmallVector<Let> lets;
  ExprPtr value;
};

struct Expr {
  enum Kind {
    Int,
    Float,
    Bool,
    Name,   // a local, a parameter, the entity, or a unique's shorthand
    Field,  // binding.field or Unique.field
    Unary,  // - !
    Binary, // arithmetic, comparison, logic
    Call,   // min(a, b), max(a, b)
    Cast,   // e as T
    If,     // if c { a } else { b }
    Spawn,  // spawn { A { .. }, B { .. } }
    Has,    // e.has(C): name is the entity, field the component
    InSet,  // e in Name: name is the set, the operand the entity
    String, // "text": name holds its bytes
    Index,  // text[i]
    Format, // {value} inside a string: the value as text
    Tuple,  // (a, b): the values a fn gives
    Row,    // table[i].field: name the table, field the field (or none)
    Of,     // value.field: a field of the row a value is
    Given,  // a value that is there already (`given`)
    Const,  // a `const` by its name: name is its IR symbol
  };
  Kind kind;
  llvm::SMLoc loc;
  mlir::Value given;
  int64_t intValue = 0;
  double floatValue = 0;
  bool boolValue = false;
  /// For an Int: written as a character ('a'), so it is a byte (i8).
  bool isByte = false;
  /// For an Int: written in hex (0xff), so it is the bits of its type.
  bool isHex = false;
  std::string name, field;
  Token::Kind op = Token::Eof;
  SmallVector<ExprPtr> operands;
  Type castType;
  std::unique_ptr<Branch> thenBranch, elseBranch;
  std::vector<SpawnEntry> entries;
};

/// A record declared in the source: a component or a unique.
struct Record {
  /// For a relation: declared a `tree`.
  bool tree = false;
  /// `ordered by`: its entities have siblings before them.
  bool ordered = false;
  /// For a relation: the components its sources and its targets have, as
  /// it names them (empty where it does not).
  std::string ends[2];
  SmallVector<std::pair<std::string, Type>> fields;
  /// For a unique declared as `unique Score: i64`: its one field is
  /// `value` and the bare name stands for it.
  bool shorthand = false;
  /// For a component: the fields an index keeps in it (an entity's row
  /// in the index), which a spawn does not give.
  SmallVector<std::string> hidden;

  Type fieldType(StringRef name) const {
    for (auto &[field, type] : fields)
      if (field == name)
        return type;
    return {};
  }
};

struct SourceModule;
/// A named list of what a `spawn` lists, with parameters: written out
/// where a spawn (or another prefab) names it.
struct Prefab {
  SmallVector<std::pair<std::string, Type>> params;
  std::vector<SpawnEntry> entries;
  /// The module it is declared in, whose names its entries use.
  SourceModule *home = nullptr;
};

/// A function over values: the program's own (`fn`) or one implemented in
/// C (`extern fn`, `extern proc`).
struct Function {
  SmallVector<Type> params;
  /// What it gives back: nothing, a value, or several.
  SmallVector<Type> results;
  /// The value it gives back, if that is one; else null.
  Type result;
  bool proc = false;
  /// Implemented in C.
  bool isExtern = false;
  /// For each parameter: the component it is given, or nothing. (Such a
  /// parameter is of the type of a buffer's, by the component's name.)
  SmallVector<std::string> components;
  /// Said, with its body yet to come (`proc name(..)` with none): its op,
  /// and where it was said, until the body is there.
  Operation *later = nullptr;
  llvm::SMLoc laterAt;
};

/// The fns or procs of one shape (what they take and give) as values: a
/// field can hold one, and what holds one can be called. The program is
/// closed, so a value is the number of the function among those of the
/// shape that are used as values anywhere (0: none), kept as an enum of
/// them, and a call through one calls a fn that asks which it is and
/// calls that one.
struct Callable {
  /// The enum's IR symbol (no module's: one shape, one type everywhere).
  std::string name;
  bool proc = false;
  SmallVector<Type> params;
  /// What it gives back, if anything.
  Type result;
  /// The IR symbols of the functions used as values, in their order:
  /// case `i + 1` of the enum.
  SmallVector<std::string> targets;
  /// Whether anything calls through such a value.
  bool called = false;
  Operation *op = nullptr;
};

/// The tables of one shape (the fields of their rows, and the enum they
/// have a row for each case of, if any) as values: a field or a
/// parameter can hold one, and what holds one is read like the table
/// itself. As with fns: a value is the number of the table among those
/// of the shape that are used as values anywhere (0: none, which has no
/// rows), kept as an enum of them, and a read through one asks a fn that
/// reads the one it is.
struct RowsShape {
  std::string name;
  SmallVector<std::pair<std::string, Type>> fields;
  /// The IR symbol of the enum its rows are for, or empty.
  std::string key;
  SmallVector<std::string> targets;
  /// The fields something reads through such a value, and whether
  /// something asks one for its number of rows.
  llvm::StringSet<> read;
  bool counted = false;
  Operation *op = nullptr;
};

/// The rows of the tables whose rows have such fields, as values: a
/// field or a parameter can hold one (`row { x: f32, y: f32 }`), and what
/// holds one is read by its fields. The rows of every table one is taken
/// of are kept in one table, one's after another's; a value is the
/// number of the row in it, counted from 1 (0: none, which reads as
/// nought).
struct RowKind {
  std::string name;
  SmallVector<std::pair<std::string, Type>> fields;
  /// The tables rows are taken of, and where each one's rows start.
  SmallVector<std::string> targets;
  SmallVector<unsigned> starts;
  unsigned total = 0;
  /// The fields something reads of such a value.
  llvm::StringSet<> read;
  /// The shapes of table values that rows are taken through.
  SmallVector<RowsShape *> through;
  Operation *op = nullptr;
};

/// What a name stands for in a body.
struct Variable {
  /// (Other: a component of the entity at the other end of an edge that
  /// a loop visits; `value` is that entity, and a field is looked up.)
  /// (Row: a row of a table, read by its fields; `value` is what the
  /// table is read by, and the table is `component` by its name, or the
  /// one that `which` holds.)
  /// (Window: rows of a buffer a `for`'s entity has, `w[i].field`; the
  /// buffer is `component` by its symbol, `value` the row its window
  /// starts at and `which` how many it has, both i32.)
  /// (Near: a component of an entity a `for ... near` visits, in a row
  /// of a grid index; `value` is the entity, `which` its row (an i32),
  /// and a field is read from the row, as seen from the place, or looked
  /// up: see Parser::nearRows.)
  enum Kind { Value, Ref, Entity, Other, Row, Window, Near };
  Kind kind;
  mlir::Value value;
  std::string component; // for Ref; for a Row, the table
  bool mut = false;      // for Ref
  /// For a Value: declared with `var`, so it can be assigned; `value` is
  /// what it holds where the parser is.
  bool isVar = false;
  mlir::Value which = {}; // for a Row of the table a value holds

  static Variable ofValue(mlir::Value value) {
    return {Value, value, std::string(), false};
  }
  static Variable ofVar(mlir::Value value) {
    return {Value, value, std::string(), false, true};
  }
  static Variable ofEntity(mlir::Value value) {
    return {Entity, value, std::string(), false};
  }
  static Variable ofRow(StringRef table, mlir::Value which, mlir::Value row) {
    return {Row, row, table.str(), false, false, which};
  }
};

/// What a `var` holds at some point: the variable, by its scope and name,
/// and its value there.
struct VarState {
  unsigned scope;
  std::string name;
  mlir::Value value;
};

class Parser {
public:
  Parser(llvm::SourceMgr &sourceMgr, MLIRContext *context,
         ArrayRef<std::string> directories)
      : sourceMgr(sourceMgr), context(context), directories(directories),
        lexer(
            sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID())->getBuffer()),
        builder(context),
        components([this](StringRef name) { return resolve(name); }),
        uniques([this](StringRef name) { return resolve(name); }),
        relations([this](StringRef name) { return resolve(name); }),
        systems([this](StringRef name) { return resolve(name); }),
        schedules([this](StringRef name) { return resolve(name); }),
        scheduleOps([this](StringRef name) { return resolve(name); }),
        functions([this](StringRef name) { return resolve(name); }),
        enums([this](StringRef name) { return resolve(name); }),
        tables([this](StringRef name) { return resolve(name); }),
        buffers([this](StringRef name) { return resolve(name); }),
        sets([this](StringRef name) { return resolve(name); }),
        assetFiles([this](StringRef name) { return resolve(name); }),
        constants([this](StringRef name) { return resolve(name); }),
        prefabs([this](StringRef name) { return resolve(name); }) {
    advance();
  }

  OwningOpRef<ModuleOp> parseModule();
  /// Where what the program is made of is written, if anyone asks.
  ImportedFiles *files = nullptr;

private:
  //===--------------------------------------------------------------===//
  // Tokens and diagnostics
  //===--------------------------------------------------------------===//

  void advance() {
    previousEnd = token.spelling.end();
    token = lexer.next();
  }
  /// Whether the current token is the first of its line: a bracket there
  /// starts something new, and is no call of what ends the line before.
  bool onNewLine() const {
    if (!previousEnd || !lexer.holds(previousEnd))
      return false;
    for (const char *at = previousEnd; at < token.loc.getPointer(); ++at)
      if (*at == '\n')
        return true;
    return false;
  }
  /// The token after the current one.
  bool atRunning();
  LogicalResult parseRunning(llvm::SMLoc at);
  Token peek() {
    Lexer ahead = lexer;
    return ahead.next();
  }
  /// Whether a statement on the buffer named here starts: `name = ..`,
  /// `name += ..`, or `name[i].field` with an assignment after it (and
  /// not a value read of it).
  bool atBufferStatement() {
    // (So with a component that was given: `name[entity].field = ..`.)
    if (!token.is(Token::Identifier) ||
        (!bufferNamed(token.spelling) && !componentGiven(token.spelling)))
      return false;
    Lexer ahead = lexer;
    Token next = ahead.next();
    if (next.is(Token::Assign) || next.is(Token::PlusAssign))
      return true;
    if (!next.is(Token::LBracket))
      return false;
    for (int brackets = 0; !next.is(Token::Eof); next = ahead.next()) {
      if (next.is(Token::LBracket))
        ++brackets;
      else if (next.is(Token::RBracket) && --brackets == 0)
        break;
    }
    if (!ahead.next().is(Token::Dot) || !ahead.next().is(Token::Identifier))
      return false;
    next = ahead.next();
    return next.is(Token::Assign) || next.isKeyword("min") ||
           next.isKeyword("max") ||
           (next.kind >= Token::PlusAssign &&
            next.kind <= Token::ShiftRightAssign);
  }
  /// Whether a counted `for` starts here: `name in`, or `number, name in`.
  bool atCountedFor() {
    if (!token.is(Token::Identifier))
      return false;
    Lexer ahead = lexer;
    Token next = ahead.next();
    if (next.isKeyword("in"))
      return true;
    if (!next.is(Token::Comma) || !ahead.next().is(Token::Identifier))
      return false;
    return ahead.next().isKeyword("in");
  }

  Location loc(llvm::SMLoc at) {
    unsigned buffer = sourceMgr.FindBufferContainingLoc(at);
    auto [line, column] = sourceMgr.getLineAndColumn(at, buffer);
    StringRef file = sourceMgr.getMemoryBuffer(buffer)->getBufferIdentifier();
    return FileLineColLoc::get(context, file, line, column);
  }

  LogicalResult error(llvm::SMLoc at, const Twine &message) {
    emitError(loc(at)) << message;
    hadError = true;
    return failure();
  }
  LogicalResult error(const Twine &message) {
    return error(token.loc, message);
  }

  LogicalResult expect(Token::Kind kind, StringRef what) {
    if (!token.is(kind))
      return error("expected " + what + ", found '" + token.spelling + "'");
    advance();
    return success();
  }
  LogicalResult expectKeyword(StringRef word) {
    if (!token.isKeyword(word))
      return error("expected '" + word + "', found '" + token.spelling + "'");
    advance();
    return success();
  }
  bool consumeIf(Token::Kind kind) {
    if (!token.is(kind))
      return false;
    advance();
    return true;
  }
  bool consumeKeyword(StringRef word) {
    if (!token.isKeyword(word))
      return false;
    advance();
    return true;
  }
  FailureOr<std::string> identifier(StringRef what) {
    if (!token.is(Token::Identifier))
      return error("expected " + what + ", found '" + token.spelling + "'");
    std::string name = token.spelling.str();
    advance();
    return name;
  }
  FailureOr<int64_t> integer(StringRef what) {
    int64_t value;
    if (!token.is(Token::Integer) || token.spelling.getAsInteger(10, value))
      return error("expected " + what + ", found '" + token.spelling + "'");
    advance();
    return value;
  }

  //===--------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------===//

  FailureOr<Type> parseType();
  LogicalResult parseFields(Record &record);
  /// An arrow between two nodes of a pattern, `-[ ... ]->` or `<-[ ... ]-`
  /// (and with `~` for `-`, from an entity to the sibling after it). The
  /// start is taken up to its `[`; what is in the brackets is the
  /// caller's.
  struct ArrowStart {
    bool reversed = false, sibling = false;
    llvm::SMLoc loc;
  };
  bool atArrow() {
    return token.is(Token::Minus) || token.is(Token::Tilde) ||
           (token.is(Token::Less) &&
            (peek().is(Token::Minus) || peek().is(Token::Tilde)));
  }
  FailureOr<ArrowStart> parseArrowStart() {
    ArrowStart arrow;
    arrow.loc = token.loc;
    arrow.reversed = consumeIf(Token::Less);
    if (consumeIf(Token::Tilde))
      arrow.sibling = true;
    else if (failed(expect(Token::Minus, "an arrow: '-[Relation]->'")))
      return failure();
    if (failed(expect(Token::LBracket, "'[' and a relation")))
      return failure();
    return arrow;
  }
  LogicalResult parseArrowEnd(const ArrowStart &arrow) {
    if (failed(expect(Token::RBracket, "']'")))
      return failure();
    Token::Kind shaft = arrow.sibling ? Token::Tilde : Token::Minus;
    StringRef text = arrow.sibling ? "~" : "-";
    if (failed(expect(shaft, ("'" + text + "'").str())))
      return failure();
    if (!arrow.reversed &&
        failed(expect(Token::Greater, ("'" + text + ">': the arrow's head")
                                          .str())))
      return failure();
    if (arrow.reversed && token.is(Token::Greater))
      return error("an arrow has one head: '<" + text.str() + "[R]" +
                   text.str() + "' or '" + text.str() + "[R]" + text.str() +
                   ">'");
    return success();
  }
  LogicalResult parseComponent(bool tag);
  LogicalResult parseBuffer();
  LogicalResult parseSet();
  LogicalResult parseSetStatement(const std::string &name, llvm::SMLoc at);
  LogicalResult parseIndex();
  LogicalResult finishIndexOrders();
  FailureOr<mlir::Value> emitIndexCall(const Expr &expr, StringRef symbol,
                                       mlir::Value given);
  LogicalResult parseBufferStatement(const std::string &name, llvm::SMLoc at);
  /// The buffer a name stands for where it is used: the buffer of that
  /// name, or the one a fn or proc was given under it (to change, or only
  /// to read).
  struct BufferUse {
    std::string symbol;
    bool mut = true;
    /// By a parameter: its value.
    mlir::Value given;
  };
  /// A hash index is read by `get` and by `where`, not by its rows (but
  /// in what it declares for itself).
  LogicalResult notHashRows(const BufferUse &held, StringRef name,
                            llvm::SMLoc at) {
    auto index = indexes.find(held.symbol);
    if (buildingIndex || index == indexes.end() || !index->second.hashed)
      return success();
    return error(at, "'" + name + "' is a hash index: it is read by '" + name +
                         ".get(key)' and by a 'for' whose 'where' "
                         "asks 'x.key == value', not by its rows");
  }
  std::optional<BufferUse> bufferNamed(StringRef name) {
    if (const Variable *variable = lookup(name)) {
      if (variable->kind == Variable::Value && variable->value &&
          !givenComponents.count(name))
        if (auto handed = dyn_cast<BufferType>(variable->value.getType()))
          return BufferUse{handed.getName().getValue().str(),
                           handed.getMut(), variable->value};
      return std::nullopt;
    }
    if (buffers.count(name))
      return BufferUse{resolve(name),
                       buildingIndex || !indexes.count(resolve(name)),
                       mlir::Value()};
    return std::nullopt;
  }
  /// The components the fn or proc that is being parsed was given, by
  /// the names of its parameters: it reads the entities that have one
  /// (`name[entity].field`, `for e, c in name { }`) and, given it `mut`,
  /// changes them and spawns.
  llvm::StringMap<std::string> givenComponents;
  struct ComponentUse {
    std::string component;
    bool mut = false;
    mlir::Value given;
  };
  std::optional<ComponentUse> componentGiven(StringRef name) {
    auto given = givenComponents.find(name);
    const Variable *variable = lookup(name);
    if (given == givenComponents.end() || !variable || !variable->value)
      return std::nullopt;
    auto handed = dyn_cast<BufferType>(variable->value.getType());
    if (!handed)
      return std::nullopt;
    return ComponentUse{given->second, handed.getMut(), variable->value};
  }
  /// Whether the proc that is being parsed was given `component` to
  /// change.
  bool givenToChange(StringRef component) {
    for (auto &entry : givenComponents)
      if (entry.second == component)
        if (auto use = componentGiven(entry.first()); use && use->mut)
          return true;
    return false;
  }
  LogicalResult parseGivenStatement(const std::string &name, llvm::SMLoc at);
  /// Whether a type being parsed may be a buffer: the parameter of an
  /// extern proc.
  bool handsBuffers = false;
  /// The component the type that was just parsed is, if it is one.
  std::string typedComponent;
  LogicalResult parseUnique();
  LogicalResult parseArchetype();
  LogicalResult parseSystem(bool isExtern, bool gpu = false);
  LogicalResult parseFunction(bool proc, bool isExtern);
  LogicalResult parseEnum();
  LogicalResult parseTable();
  LogicalResult parseConst();
  LogicalResult parsePrefab();
  FailureOr<std::vector<SpawnEntry>> parseSpawnEntries();
  FailureOr<SpawnEntry> parseSpawnIf();
  LogicalResult parsePrefabUse(SpawnEntry &entry);
  LogicalResult emitAddEntries(const std::vector<SpawnEntry> &entries,
                               llvm::SMLoc at);
  /// The components of the entity that is being spawned, with their
  /// values, in the order they were first listed.
  struct Spawned {
    SmallVector<std::pair<std::string, SmallVector<mlir::Value>>> parts;
  };
  FailureOr<mlir::Value>
  emitSpawnEntries(const std::vector<SpawnEntry> &entries, size_t from,
                   Spawned spawned, llvm::SMLoc at,
                   function_ref<FailureOr<mlir::Value>(Spawned)> rest);
  FailureOr<Attribute> constantOf(const Expr &expr, Type type);
  LogicalResult parseSchedule();
  LogicalResult parseMain();
  LogicalResult parseWorld();
  LogicalResult parseDeclarations();
  LogicalResult parseImport();
  LogicalResult parseMainBlock();
  FailureOr<ArrayAttr> parseNameList(StringRef what);

  //===--------------------------------------------------------------===//
  // Statements (emitted as they are parsed)
  //===--------------------------------------------------------------===//

  LogicalResult parseBlock(bool ownScope = true);
  LogicalResult parseStatements();
  std::string openBreak(Location where);
  mlir::Value breakFlag();
  mlir::Value continueFlag();
  LogicalResult parseLoopBody();
  void setVar(StringRef name, mlir::Value value);
  LogicalResult parseStatement();
  LogicalResult parseFor();
  LogicalResult parseCountedFor(llvm::SMLoc at);
  LogicalResult parseLoop(llvm::SMLoc at);
  LogicalResult parseWhile(llvm::SMLoc at);
  FailureOr<ExprPtr> parseStatementValue();
  FailureOr<ExprPtr> parseCountedQuery();
  LogicalResult parseIf();
  LogicalResult parseIfLet(llvm::SMLoc at);
  LogicalResult parseNameStatement();
  LogicalResult parseVar();
  bool ifIsStatement();
  SmallVector<VarState> captureVars();
  void restoreVars(ArrayRef<VarState> states);
  void mergeBranches(scf::IfOp branch, ArrayRef<VarState> before,
                     ArrayRef<VarState> thenVars, ArrayRef<VarState> elseVars);
  LogicalResult parseMethod(const std::string &entity, llvm::SMLoc at);
  LogicalResult emitAssignment(llvm::SMLoc at, Token::Kind op,
                               std::optional<StringRef> rule,
                               function_ref<Type()> targetType,
                               function_ref<mlir::Value()> load,
                               function_ref<LogicalResult(mlir::Value)> store,
                               const Expr &value);
  FailureOr<std::pair<Token::Kind, std::optional<StringRef>>>
  parseAssignOp();

  //===--------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------===//

  FailureOr<ExprPtr> parseExpr();
  FailureOr<ExprPtr> parseBinary(int precedence);
  FailureOr<ExprPtr> parseUnary();
  FailureOr<ExprPtr> parsePrimary();
  LogicalResult parseRelation();
  LogicalResult parseEdges(llvm::SMLoc at);
  LogicalResult parseEach(llvm::SMLoc at);
  LogicalResult parseNear(llvm::SMLoc at, const std::string &entity,
                          ArrayRef<std::pair<std::string, std::string>> bound);
  LogicalResult loopWithVars(Location where, mlir::Value lower,
                             mlir::Value upper,
                             function_ref<LogicalResult(mlir::Value)> body);
  LogicalResult ifWithVars(Location where, mlir::Value holds,
                           function_ref<LogicalResult()> body);
  LogicalResult whileWithVars(Location where,
                              function_ref<mlir::Value()> condition,
                              function_ref<LogicalResult()> body);
  /// In the body of a `for ... near`: the index and its row the entity is
  /// in, and where it is seen from the place (the image of each place's
  /// field nearest it, by the component's symbol and the field).
  struct NearRows {
    std::string index;
    mlir::Value row;
    SmallVector<std::tuple<std::string, std::string, mlir::Value>> seen;
  };
  std::optional<NearRows> nearRows;
  LogicalResult checkEaches(Operation *query);
  /// Inside the body of a `for` over entities that is in another.
  bool inEach = false;
  /// Inside the body of a `for` whose entities take turns (`in turn by`):
  /// what it does takes effect at once.
  bool inTurn = false;
  LogicalResult parseConnect(llvm::SMLoc at);
  LogicalResult emitCondition(const Expr &expr);
  FailureOr<std::unique_ptr<Branch>> parseBranch();
  FailureOr<ComponentInit> parseComponentInit();

  Type typeOf(const Expr &expr);
  Type defaultType(const Expr &expr);
  FailureOr<mlir::Value> emit(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitRaw(const Expr &expr, Type expected);
  FailureOr<ExprPtr> parseString();
  FailureOr<char> parseEscape(StringRef &rest, llvm::SMLoc at);

  // Text: a value of !ent.text<N> is one integer (see TextType), which
  // these compute with.
  Type textTypeOf(const Expr &expr);
  bool isAnyText(const Expr &expr);
  bool mayBeAssigned(const Expr &expr);
  LogicalResult keepsNoText(llvm::SMLoc at, StringRef component);
  /// Set while the value a fn gives back is emitted, where that is a
  /// text of any length.
  bool givesText = false;
  TextType textType(llvm::SMLoc at, unsigned capacity);
  mlir::Value integer(Location at, Type type, uint64_t value);
  /// A number without a sign as its bits, and bits as such a number.
  mlir::Value bits(Location at, mlir::Value value);
  mlir::Value fromBits(Location at, mlir::Value bits, Type type);
  /// A number without a sign where one says which row or byte: the same
  /// number as one with a sign.
  mlir::Value countable(Location at, mlir::Value value);
  mlir::Value indexNumber(Location at, mlir::Value index);
  mlir::Value asIndex(Location at, mlir::Value number, IndexOfType type);
  FailureOr<mlir::Value> emitRowNumber(const Expr &number,
                                       FlatSymbolRefAttr buffer,
                                       StringRef name);
  /// The row of the buffer that row `number` of `window` is (an i32), or
  /// -1, which the buffer has not, where the window has no such row: that
  /// is said, and the query's end stops the program.
  FailureOr<mlir::Value> windowRow(const Variable &window, const Expr &number);
  LogicalResult parseWindowStatement(const std::string &name,
                                     const Variable &window, llvm::SMLoc at);
  mlir::Value textBits(Location at, mlir::Value text);
  mlir::Value textFromBits(Location at, mlir::Value bits, TextType type);
  mlir::Value textConstant(Location at, StringRef bytes, TextType type);
  mlir::Value textResize(Location at, mlir::Value text, TextType to);
  mlir::Value textConcat(llvm::SMLoc at, mlir::Value a, mlir::Value b);
  mlir::Value textLength(Location at, mlir::Value text);
  mlir::Value textIndex(Location at, mlir::Value text, mlir::Value index);
  mlir::Value formatUnsigned(Location at, mlir::Value magnitude);
  FailureOr<mlir::Value> formatValue(llvm::SMLoc at, mlir::Value value);
  FailureOr<mlir::Value> emitBinary(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitIf(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitSpawn(const Expr &expr);
  FailureOr<mlir::Value> emitInvoke(const Expr &expr);
  FailureOr<Type> parseCallableType(bool proc);
  Callable *callableOf(Type type);
  Callable *callableNamed(StringRef name);
  Callable *callableThrough(const Expr &call);
  FailureOr<mlir::Value> emitFunctionValue(const Expr &expr,
                                           Callable &callable);
  FailureOr<SmallVector<mlir::Value>> emitCallThrough(const Expr &expr,
                                                      Callable &callable);
  LogicalResult finishCallables();
  void finishIndexes();
  FailureOr<Type> parseRowsType();
  RowsShape *rowsOf(Type type);
  FailureOr<mlir::Value> emitTableValue(const Expr &expr, RowsShape &shape);
  FailureOr<mlir::Value> emitRowThrough(const Expr &expr, RowsShape &shape,
                                        const Expr &base, const Expr &index,
                                        StringRef field);
  mlir::Value noughtOf(Location at, Type type);
  LogicalResult finishRows();
  std::string typeWords(Type type);
  LogicalResult mismatch(llvm::SMLoc at, Type value, Type target);
  LogicalResult finishRowKinds();
  FailureOr<Type> parseRowType();
  RowKind *rowOf(Type type);
  bool isRowSource(const Expr &expr);
  unsigned rowsStart(RowKind &kind, StringRef target);
  FailureOr<mlir::Value> emitRowValue(const Expr &expr, RowKind &kind);
  FailureOr<mlir::Value> readRowValue(llvm::SMLoc at, mlir::Value row,
                                      StringRef field);
  std::vector<std::unique_ptr<RowKind>> rowKinds;
  llvm::StringMap<RowKind *> rowKindByKey;
  llvm::StringMap<RowKind *> rowKindByName;
  LogicalResult parseAsset();
  /// (For who asks what the program is made of: a file by its whole
  /// path.)
  LogicalResult noteAsset(llvm::SMLoc at, StringRef name, StringRef path) {
    SmallString<256> whole;
    if (llvm::sys::fs::real_path(path, whole))
      whole = path;
    // (A program knows a file by its name: one name, one file.)
    auto [known, isNew] = assetPaths.try_emplace(name, whole.str().str());
    if (!isNew && known->second != whole)
      return error(at, "two files are the asset '" + name + "': '" +
                           known->second + "' and '" + whole + "'");
    if (files && isNew)
      files->assets.push_back({name.str(), whole.str().str()});
    return success();
  }
  llvm::StringMap<std::string> assetPaths;
  bool isWholeRow(const Expr &expr);
  FailureOr<Variable> emitRow(const Expr &expr);
  Expr fieldOfRow(const Variable &row, StringRef field, llvm::SMLoc at);
  std::vector<std::unique_ptr<RowsShape>> rowShapes;
  llvm::StringMap<RowsShape *> rowsByKey;
  llvm::StringMap<RowsShape *> rowsByName;
  FailureOr<SmallVector<mlir::Value>> emitCall(const Expr &expr);
  FailureOr<SmallVector<mlir::Value>>
  emitSeveral(const Expr &expr, ArrayRef<Type> expected);
  FailureOr<Let> parseLet(bool statement = false);
  LogicalResult emitLet(const Let &let, bool isVar);
  scf::IfOp giveFromBranches(scf::IfOp branch, ArrayRef<mlir::Value> thenValues,
                             ArrayRef<mlir::Value> elseValues);
  FailureOr<SmallVector<mlir::Value>> emitInitValues(const ComponentInit &init);
  mlir::Value combine(Location at, StringRef rule, mlir::Value a,
                      mlir::Value b);
  FailureOr<mlir::Value> arithmetic(Location at, Token::Kind op,
                                    mlir::Value a, mlir::Value b);

  //===--------------------------------------------------------------===//
  // Scopes
  //===--------------------------------------------------------------===//

  const Variable *lookup(StringRef name) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      auto it = scope->find(name);
      if (it != scope->end())
        return &it->second;
    }
    return nullptr;
  }
  void bind(StringRef name, Variable variable) {
    scopes.back()[name] = variable;
  }
  struct ScopeGuard {
    Parser &parser;
    explicit ScopeGuard(Parser &parser) : parser(parser) {
      parser.scopes.emplace_back();
    }
    ~ScopeGuard() { parser.scopes.pop_back(); }
  };

  FlatSymbolRefAttr symbol(StringRef name) {
    return FlatSymbolRefAttr::get(context, resolve(name));
  }

  //===--------------------------------------------------------------===//
  // Modules
  //===--------------------------------------------------------------===//

  /// The IR symbol a name written in the module being parsed stands for:
  /// its own declaration, else that of the one module it imports that
  /// declares the name; `m::Name` names module `m`'s. A name nothing
  /// declares resolves to an own name, which the caller will not find.
  std::string resolve(StringRef written) {
    auto report = [&](const Twine &message) {
      if (current->reported.insert(written).second)
        (void)error(message);
    };
    auto [qualifier, base] = written.split("::");
    if (base.empty() && !written.contains("::")) {
      if (current->names.contains(written))
        return current->prefix + written.str();
      SourceModule *found = nullptr;
      for (SourceModule *imported : current->imports) {
        if (!imported->names.contains(written))
          continue;
        if (found) {
          report("'" + written + "' is declared by the modules '" +
                 found->name + "' and '" + imported->name + "'; write '" +
                 found->name + "::" + written + "' or '" + imported->name +
                 "::" + written + "'");
          break;
        }
        found = imported;
      }
      return (found ? found->prefix : current->prefix) + written.str();
    }
    if (qualifier == current->name)
      return current->prefix + base.str();
    for (SourceModule *imported : current->imports)
      if (imported->name == qualifier)
        return imported->prefix + base.str();
    report("unknown module '" + qualifier + "'; import it first");
    return current->prefix + written.str();
  }

  /// Declare `name` in the module being parsed and return its IR symbol.
  StringAttr declareSymbol(llvm::SMLoc at, StringRef name) {
    if (name.contains("::"))
      (void)error(at, "a declaration's name cannot be qualified; it belongs to "
                      "the module that declares it");
    current->names.insert(name);
    return builder.getStringAttr(current->prefix + name);
  }

  llvm::SourceMgr &sourceMgr;
  MLIRContext *context;
  /// Where imported modules are looked for, after the importer's own
  /// directory and before those of `-I`.
  SmallVector<std::string> directories;
  Lexer lexer;
  /// Whether the declaration that is read now is marked `transient`.
  bool transientNext = false;
  Token token;
  /// Where the token before the current one ends.
  const char *previousEnd = nullptr;
  OpBuilder builder;
  ModuleOp module;

  Declared<Record> components;
  Declared<Record> uniques;
  Declared<Record> relations;

  /// The fields a ref's variable has: a component's, or for an edge a
  /// relation's.
  const Record &recordOf(const Variable &variable) {
    auto relation = relations.find(variable.component);
    if (relation != relations.end())
      return relation->second;
    return components[variable.component];
  }
  Declared<SmallVector<Type>> systems;
  Declared<SmallVector<Type>> schedules;
  /// The schedules themselves, for a schedule that runs one.
  Declared<Operation *> scheduleOps;
  Declared<Function> functions;
  /// The shapes of fns and procs that are types, by how they are written
  /// out (see callableKey) and by their enum's symbol.
  std::vector<std::unique_ptr<Callable>> callables;
  llvm::StringMap<Callable *> callablesByKey;
  llvm::StringMap<Callable *> callablesByName;
  /// Parsing the body of a proc: a function that may call procs.
  bool inProc = false;
  /// The cases of every enum, in the order they number them.
  Declared<SmallVector<std::string>> enums;
  /// Tables: their fields (one without a name for a plain list), and how
  /// many rows each has.
  Declared<Record> tables;
  /// The buffers, by their fields.
  Declared<Record> buffers;
  /// The sets (`set Name`): entities, each once, a bit for each.
  Declared<bool> sets;
  /// The indexes (`index Name of Component by field`), by the symbol of
  /// the buffer that holds their rows: the component that keeps each
  /// entity's row, the field it keeps it in, the type of the key, and the
  /// fn that halves the rows.
  struct Index {
    std::string component;
    std::string rowField;
    Type key;
    std::string find;
    /// Of a grid: the fields of where an entity is (the first
    /// component's, and the rows' of the same names), their type, whether
    /// the box wraps, the unique of the grid's sizes and the buffer of
    /// each cell's first row; and copied fields by their component and
    /// field, to their names in the rows.
    SmallVector<std::string> positions;
    Type positionType;
    bool wrap = false;
    std::string gridUnique, cells;
    /// A Morton index (`morton (x, y)`): how many bits a side its keys
    /// have, and how many entities a cell holds at most before it is split;
    /// `cells` its cells in preorder (`N_box` its box, as `gridUnique`).
    bool morton = false;
    int bits = 0, leaf = 0;
    SmallVector<std::tuple<std::string, std::string, std::string>> copied;
    /// The components of the entities it holds, by their symbols.
    SmallVector<std::string> of;
    /// A hash index (`hash`): the fn that finds an entity by its key
    /// (`Name.get(key)`), whether the key is a text (whose hash the rows
    /// keep, the entity's text compared), and the field it is of.
    bool hashed = false;
    bool textKey = false;
    std::string keyField;
  };
  llvm::StringMap<Index> indexes;
  /// Parsing what an index declares for itself, which fills it (and of
  /// which index: `index_duplicate` names it).
  bool buildingIndex = false;
  std::string buildingIndexName;
  /// The files that have a name (`asset name = "file"`): as the program
  /// writes them.
  Declared<std::string> assetFiles;
  /// A `const`: a value by a name, written out again wherever it is used.
  struct Constant {
    Type type;
    ExprPtr value;
  };
  Declared<Constant> constants;
  llvm::StringMap<unsigned> tableRows;
  /// Of a table with a row for each case of an enum: that enum's symbol.
  llvm::StringMap<std::string> tableKeys;
  Declared<Prefab> prefabs;
  /// Parsing the body of a system (or `world`) or of a fn: where functions
  /// are called.
  bool inSystem = false;
  /// Parsing the body of a fn, which only computes from its parameters.
  bool inFunction = false;
  /// The unique a `for` over entities that is being parsed counts into:
  /// one that is asked how many entities its body ran for.
  FlatSymbolRefAttr countsInto;
  /// Whether a `for` over entities may stand where a value does: in the
  /// value of a statement, which is worked out there and then.
  bool forValueAllowed = false;
  unsigned countedFors = 0;
  /// The system whose body is being parsed (not `world`).
  Operation *systemOp = nullptr;
  /// In a `loop` of a system, whose body is built before the loop is in
  /// the system.
  bool inSystemLoop = false;
  bool hasMain = false;
  bool hadError = false;
  /// Every module of the program, in the order their parsing finished: a
  /// module after the ones it imports, the root last.
  std::vector<std::unique_ptr<SourceModule>> modules;
  SmallVector<SourceModule *> finished;
  /// Modules by the path of their file.
  llvm::StringMap<SourceModule *> modulesByPath;
  SourceModule *root = nullptr;
  SourceModule *current = nullptr;
  /// The names a `world` block is declared under: a system holding its
  /// statements and a schedule running it, which `main` calls first and a
  /// C host calls as `ent_world_init`.
  static constexpr StringLiteral kWorldSystem = "world_setup";
  static constexpr StringLiteral kWorldSchedule = "world_init";
  SmallVector<llvm::StringMap<Variable>> scopes;
  /// Inside a `for`: the name of the visited entity, if bound.
  bool inQuery = false;
  bool inEdges = false;
  /// The loops `break` may leave, innermost last: the name of each one's
  /// flag, a var that `break` sets (see openBreak); and how many of them
  /// are outside the `for` over entities the parser is in, which `break`
  /// does not leave.
  SmallVector<std::string> breakFlags;
  size_t breakFloor = 0;
  unsigned breaks = 0;
  /// And the flag `continue` sets, of the body each turn of them runs
  /// (see parseLoopBody); as many as of `break` inside a body.
  SmallVector<std::string> continueFlags;
  /// The false each loop's `break` flag starts as, which its turns'
  /// `continue` flags start as too: by the name of the `break` flag.
  llvm::StringMap<mlir::Value> breakStarts;
  /// The number of scopes outside the `for` over entities (and the edge
  /// loop) being parsed: a `var` of one of them is not the loop's own.
  unsigned queryScopes = 0, edgesScopes = 0;
  std::string queryEntity;
  /// The names the entity a `for` visits goes by: its own, if it has one,
  /// and those of its components' bindings.
  llvm::StringSet<> queryNames;
};

} // namespace

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

OwningOpRef<ModuleOp> Parser::parseModule() {
  OwningOpRef<ModuleOp> owned = ModuleOp::create(loc(token.loc));
  module = *owned;
  builder.setInsertionPointToEnd(module.getBody());
  StringRef file = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID())
                       ->getBufferIdentifier();
  modules.push_back(std::make_unique<SourceModule>());
  root = current = modules.back().get();
  root->name = llvm::sys::path::stem(file).str();
  SmallString<256> path;
  if (!llvm::sys::fs::real_path(file, path))
    modulesByPath[path] = root;
  if (files)
    files->sources.push_back(path.empty() ? file.str() : path.str().str());
  if (failed(parseDeclarations()) || hadError)
    return nullptr;
  for (auto &entry : functions)
    if (entry.second.later) {
      emitError(loc(entry.second.laterAt))
          << "'" << entry.first() << "' is said here without a body, and "
             "given none further down ('extern' declares one implemented "
             "in C)";
      return nullptr;
    }
  root->loading = false;
  finished.push_back(root);
  if (failed(finishRowKinds()) || failed(finishRows()) ||
      failed(finishCallables()))
    return nullptr;
  if (failed(finishIndexOrders()))
    return nullptr;
  finishIndexes();

  // Every module's world is set up before `main` does anything else, a
  // module's after those it imports, wherever they were declared.
  for (MainOp main : module.getOps<MainOp>()) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&main.getBody().front());
    for (SourceModule *source : finished)
      if (!source->worldSchedule.empty())
        CallOp::create(builder, main.getLoc(),
                       FlatSymbolRefAttr::get(context, source->worldSchedule),
                       ValueRange{});
  }
  if (failed(verify(module)))
    return nullptr;
  return owned;
}

// The row numbers of buffers, now that every capacity is known: each
// stored in the bits its buffer's room needs, and the buffers they read
// marked to have room for a power of two of rows.
void Parser::finishIndexes() {
  llvm::DenseMap<Attribute, BufferOp> byName;
  for (BufferOp buffer : module.getOps<BufferOp>())
    byName[buffer.getSymNameAttr()] = buffer;
  llvm::SetVector<BufferOp> indexed;
  AttrTypeReplacer replacer;
  replacer.addReplacement([&](IndexOfType index) -> Type {
    BufferOp buffer = byName.lookup(index.getBuffer().getAttr());
    if (!buffer)
      return index;
    indexed.insert(buffer);
    return IndexOfType::get(
        context, index.getBuffer(),
        IndexOfType::bitsFor(llvm::PowerOf2Ceil(buffer.getCapacity())));
  });
  replacer.recursivelyReplaceElementsIn(module, /*replaceAttrs=*/true,
                                        /*replaceLocs=*/false,
                                        /*replaceTypes=*/true);
  for (BufferOp buffer : indexed)
    buffer->setAttr(WorldLayout::kIndexedAttr, builder.getUnitAttr());
}

// The declarations of one file, up to its end.
LogicalResult Parser::parseDeclarations() {
  while (!token.is(Token::Eof)) {
    LogicalResult result = success();
    // `transient unique`, `transient buffer`: not part of a saved world.
    transientNext = false;
    if (token.isKeyword("transient") &&
        (peek().isKeyword("unique") || peek().isKeyword("buffer"))) {
      advance();
      transientNext = true;
    } else if (token.isKeyword("transient") && peek().is(Token::Identifier)) {
      // transient Name: a unique or a buffer declared before, here or in
      // an imported module, is not part of a saved world either.
      advance();
      llvm::SMLoc nameAt = token.loc;
      FailureOr<std::string> name = identifier("a unique or a buffer");
      if (failed(name))
        return failure();
      Operation *declared =
          SymbolTable::lookupSymbolIn(module, symbol(*name).getAttr());
      if (!declared || !isa<ResourceOp, BufferOp>(declared))
        return error(nameAt, "'" + *name + "' is not a unique or a buffer "
                             "declared before: only those can be "
                             "'transient'");
      declared->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
      consumeIf(Token::Semicolon);
      continue;
    }
    // distribute Component by field: where its entities live on many
    // ranks (docs/ranks.md).
    if (token.isKeyword("distribute") && peek().is(Token::Identifier)) {
      advance();
      llvm::SMLoc nameAt = token.loc;
      FailureOr<std::string> name = identifier("a component");
      if (failed(name))
        return failure();
      if (!components.count(*name))
        return error(nameAt, "unknown component '" + *name + "'");
      if (!consumeKeyword("by"))
        return error("expected 'by' and a field: 'distribute " + *name +
                     " by field'");
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      Type type = components[*name].fieldType(*field);
      if (!type)
        return error(fieldAt, "component '" + *name + "' has no field '" +
                                  *field + "'");
      auto whole = dyn_cast<IntegerType>(type);
      if (!isa<EntityType>(type) && (!whole || whole.getWidth() == 1))
        return error(fieldAt, "an entity is placed by a whole number (its "
                              "range of them on each rank) or by an entity "
                              "(it lives where that one does); '" + *field +
                                  "' is neither");
      Operation *declared =
          SymbolTable::lookupSymbolIn(module, symbol(*name).getAttr());
      if (declared->hasAttr(WorldLayout::kDistributeAttr))
        return error(nameAt, "'" + *name + "' is distributed already");
      declared->setAttr(WorldLayout::kDistributeAttr,
                        builder.getStringAttr(*field));
      consumeIf(Token::Semicolon);
      continue;
    }
    if (consumeKeyword("component"))
      result = parseComponent(/*tag=*/false);
    else if (consumeKeyword("tag"))
      result = parseComponent(/*tag=*/true);
    else if (consumeKeyword("unique"))
      result = parseUnique();
    else if (token.isKeyword("buffer") && peek().is(Token::Identifier)) {
      advance();
      result = parseBuffer();
    } else if (token.isKeyword("set") && peek().is(Token::Identifier)) {
      advance();
      result = parseSet();
    } else if (token.isKeyword("index") && peek().is(Token::Identifier)) {
      advance();
      result = parseIndex();
    } else if (consumeKeyword("relation"))
      result = parseRelation();
    else if (consumeKeyword("archetype"))
      result = parseArchetype();
    else if (consumeKeyword("system"))
      result = parseSystem(/*isExtern=*/false);
    else if (token.isKeyword("gpu") && peek().isKeyword("system")) {
      advance();
      advance();
      result = parseSystem(/*isExtern=*/false, /*gpu=*/true);
    } else if (token.isKeyword("gpu") && peek().isKeyword("extern"))
      result = error("an extern system is implemented in C, on the CPU; "
                     "only a system with a body can be 'gpu'");
    else if (consumeKeyword("extern")) {
      if (consumeKeyword("fn"))
        result = parseFunction(/*proc=*/false, /*isExtern=*/true);
      else if (consumeKeyword("proc"))
        result = parseFunction(/*proc=*/true, /*isExtern=*/true);
      else if (consumeKeyword("system"))
        result = parseSystem(/*isExtern=*/true);
      else
        result = error("expected 'fn', 'proc' or 'system' after 'extern', "
                       "found '" + token.spelling + "'");
    }
    else if (consumeKeyword("fn"))
      result = parseFunction(/*proc=*/false, /*isExtern=*/false);
    else if (consumeKeyword("proc"))
      result = parseFunction(/*proc=*/true, /*isExtern=*/false);
    else if (consumeKeyword("enum"))
      result = parseEnum();
    else if (token.isKeyword("const") && peek().is(Token::Identifier))
      result = parseConst();
    else if (token.isKeyword("table") && peek().is(Token::Identifier))
      result = parseTable();
    else if (consumeKeyword("schedule"))
      result = parseSchedule();
    else if (token.isKeyword("import"))
      result = parseImport();
    else if (token.isKeyword("asset") &&
             (peek().is(Token::String) || peek().is(Token::Identifier)))
      result = parseAsset();
    else if (token.isKeyword("main"))
      result = current == root
                   ? parseMain()
                   : error("'main' belongs to the program's own file, not to "
                           "a module it imports");
    else if (token.isKeyword("world"))
      result = parseWorld();
    else if (token.isKeyword("default_capacity")) {
      if (current != root)
        return error("'default_capacity' belongs to the program's own file, "
                     "not to a module it imports");
      advance();
      FailureOr<int64_t> capacity = integer("a capacity");
      if (failed(capacity))
        return failure();
      module->setAttr("ent.default_capacity",
                      builder.getI64IntegerAttr(*capacity));
    } else if (token.isKeyword("capacity")) {
      // capacity Name N: another capacity for a component, a relation or
      // an archetype declared before, here or in an imported module. The
      // last one said holds, so the importing file's over the module's.
      advance();
      llvm::SMLoc nameAt = token.loc;
      FailureOr<std::string> name = identifier("a component, a relation or "
                                               "an archetype");
      if (failed(name))
        return failure();
      FailureOr<int64_t> capacity = integer("a capacity");
      if (failed(capacity))
        return failure();
      if (*capacity <= 0)
        return error(nameAt, "a capacity is at least 1");
      Operation *declared =
          SymbolTable::lookupSymbolIn(module,
                                      symbol(*name).getAttr());
      if (!declared ||
          !isa<ComponentOp, RelationOp, ArchetypeOp, BufferOp>(declared))
        return error(nameAt, "'" + *name + "' is no component, relation, "
                             "archetype or buffer declared before this: "
                             "'capacity' gives one of those another "
                             "capacity");
      declared->setAttr("capacity", builder.getI64IntegerAttr(*capacity));
    } else if (token.isKeyword("prefab")) {
      result = parsePrefab();
    } else if (token.isKeyword("device")) {
      result = error("'" + token.spelling + "' is not supported yet");
    } else {
      result = error("expected a declaration (import, component, tag, "
                     "unique, enum, relation, archetype, system, fn, "
                     "schedule, world, main), found '" +
                     token.spelling + "'");
    }
    if (failed(result))
      return failure();
  }
  return success();
}

// import name: the declarations of name.ent, found next to the importing
// file or in an -I directory.
/// Whether `name` is what `pattern` says, in which a `*` is any letters.
static bool matchesPattern(StringRef name, StringRef pattern) {
  size_t star = pattern.find('*');
  if (star == StringRef::npos)
    return name == pattern;
  if (!name.starts_with(pattern.take_front(star)))
    return false;
  name = name.drop_front(star);
  pattern = pattern.drop_front(star + 1);
  for (size_t skipped = 0; skipped <= name.size(); ++skipped)
    if (matchesPattern(name.drop_front(skipped), pattern))
      return true;
  return false;
}

/// Whether `a` comes before `b`, numbers in them by how much they are:
/// walk2.png before walk10.png.
static bool namedBefore(StringRef a, StringRef b) {
  while (!a.empty() && !b.empty()) {
    if (llvm::isDigit(a[0]) && llvm::isDigit(b[0])) {
      StringRef x = a.take_while(llvm::isDigit);
      StringRef y = b.take_while(llvm::isDigit);
      StringRef m = x.ltrim('0'), n = y.ltrim('0');
      if (m.size() != n.size())
        return m.size() < n.size();
      if (m != n)
        return m < n;
      a = a.drop_front(x.size());
      b = b.drop_front(y.size());
      continue;
    }
    if (a[0] != b[0])
      return a[0] < b[0];
    a = a.drop_front();
    b = b.drop_front();
  }
  return a.size() < b.size();
}

// asset "file", asset name = "file": a file the program needs when it
// runs (a font, a picture, a sound), next to the file that says so. It
// is there when the program is built, and `ent build` takes it along;
// the program names it as it is written here, or by the name it is
// given. With a `*` in the file's name ("walk/*.png") it is every file
// there is of that name, and the name a list of them, in their order.
LogicalResult Parser::parseAsset() {
  advance();
  llvm::SMLoc nameAt = token.loc;
  std::string given;
  if (token.is(Token::Identifier)) {
    given = token.spelling;
    advance();
    if (failed(expect(Token::Assign, "'=' and the file's name")))
      return failure();
    if (!token.is(Token::String))
      return error("expected the file's name, as a text");
  }
  llvm::SMLoc at = token.loc;
  std::string name =
      StringRef(token.spelling).drop_front().drop_back().str();
  advance();
  consumeIf(Token::Semicolon);
  StringRef written = name;
  if (written.empty() || llvm::sys::path::is_absolute(written) ||
      written.contains('\\') || written.contains(':') ||
      written.contains('{') ||
      llvm::is_contained(llvm::split(written, '/'), "..") ||
      llvm::is_contained(llvm::split(written, '/'), "") ||
      llvm::is_contained(llvm::split(written, '/'), "."))
    return error(at, "an asset is named by its path from the folder of the "
                     "file that declares it ('fonts/name.ttf'), without "
                     "'..'");
  StringRef declaring =
      sourceMgr.getMemoryBuffer(sourceMgr.FindBufferContainingLoc(at))
          ->getBufferIdentifier();
  SmallString<256> path(llvm::sys::path::parent_path(declaring));
  llvm::sys::path::append(path, written);

  // One file.
  if (!written.contains('*')) {
    if (!llvm::sys::fs::is_regular_file(path))
      return error(at, "there is no file '" + name + "' next to '" +
                           llvm::sys::path::filename(declaring) + "'");
    if (failed(noteAsset(at, name, path)))
      return failure();
    if (!given.empty()) {
      declareSymbol(nameAt, given);
      assetFiles[given] = name;
    }
    return success();
  }

  // Every file of that name: in their order, numbers by how much they
  // are.
  StringRef folder = llvm::sys::path::parent_path(written);
  StringRef pattern = llvm::sys::path::filename(written);
  if (folder.contains('*'))
    return error(at, "the '*' is in the file's name, not in its folder's "
                     "('walk/*.png')");
  std::vector<std::string> files;
  std::error_code problem;
  // (Next to a file that is named without a folder: where the compiler
  // is run.)
  StringRef inside = llvm::sys::path::parent_path(path);
  for (llvm::sys::fs::directory_iterator
           entry(inside.empty() ? "." : inside, problem),
       end;
       !problem && entry != end; entry.increment(problem)) {
    StringRef file = llvm::sys::path::filename(entry->path());
    if (file.starts_with(".") || !matchesPattern(file, pattern) ||
        !llvm::sys::fs::is_regular_file(entry->path()))
      continue;
    files.push_back(folder.empty() ? file.str()
                                   : (folder + "/" + file).str());
  }
  if (files.empty())
    return error(at, "there is no file '" + name + "' next to '" +
                         llvm::sys::path::filename(declaring) + "'");
  llvm::sort(files, [](const std::string &a, const std::string &b) {
    return namedBefore(a, b);
  });
  for (const std::string &file : files) {
    SmallString<256> where(llvm::sys::path::parent_path(declaring));
    llvm::sys::path::append(where, file);
    if (failed(noteAsset(at, file, where)))
      return failure();
  }
  if (given.empty())
    return success();
  // The list: a table of the files' names.
  unsigned longest = 0;
  for (const std::string &file : files)
    longest = std::max<unsigned>(longest, file.size());
  if (longest > TextType::kMaxCapacity)
    return error(at, "a file's name here has at most " +
                         Twine(unsigned(TextType::kMaxCapacity)) + " bytes");
  auto text = TextType::get(context, longest);
  IntegerType storage = text.getStorageType();
  SmallVector<Attribute> values;
  for (const std::string &file : files) {
    APInt bits(storage.getWidth(), file.size());
    for (auto [index, byte] : llvm::enumerate(file))
      bits.insertBits(static_cast<unsigned char>(byte), 16 + 8 * index, 8);
    values.push_back(IntegerAttr::get(storage, bits));
  }
  Record record;
  record.fields.push_back({"", text});
  StringAttr symbolName = declareSymbol(nameAt, given);
  OperationState state(loc(nameAt), TableOp::getOperationName());
  state.addAttribute("sym_name", symbolName);
  state.addAttribute("field_names", builder.getStrArrayAttr({""}));
  state.addAttribute("field_types", builder.getTypeArrayAttr({text}));
  state.addAttribute("values",
                     builder.getArrayAttr({builder.getArrayAttr(values)}));
  builder.create(state);
  tableRows[symbolName.getValue()] = files.size();
  tables[given] = std::move(record);
  return success();
}

LogicalResult Parser::parseImport() {
  llvm::SMLoc at = token.loc;
  advance();
  FailureOr<std::string> name = identifier("a module name");
  if (failed(name))
    return failure();
  consumeIf(Token::Semicolon);
  if (StringRef(*name).contains("::"))
    return error(at, "expected a module name");

  SmallVector<std::string> dirs;
  StringRef importer =
      sourceMgr.getMemoryBuffer(sourceMgr.FindBufferContainingLoc(at))
          ->getBufferIdentifier();
  dirs.push_back(llvm::sys::path::parent_path(importer).str());
  llvm::append_range(dirs, directories);
  llvm::append_range(dirs, importDirs);
  SmallString<256> path;
  bool found = false;
  for (auto [index, dir] : llvm::enumerate(dirs)) {
    SmallString<256> candidate(dir);
    llvm::sys::path::append(candidate, *name + ".ent");
    SmallString<256> real;
    if (llvm::sys::fs::real_path(candidate, real) ||
        !llvm::sys::fs::is_regular_file(real))
      continue;
    if (!found) {
      found = true;
      path = real;
      // (Only a file next to the importing one can stand in for a
      // module unasked: the other folders were named, in their order.)
      if (index != 0)
        break;
      continue;
    }
    if (real != path) {
      emitWarning(loc(at))
          << "'" << *name << ".ent' next to this file is imported, not the "
          << "module '" << *name << "' in '" << dir
          << "'; give the file another name to import that one";
      break;
    }
  }
  if (!found)
    return error(at, "cannot find module '" + *name + "': no '" + *name +
                         ".ent' next to the importing file or in an -I "
                         "directory");

  auto known = modulesByPath.find(path);
  if (known != modulesByPath.end()) {
    if (known->second->loading)
      return error(at, "module '" + *name + "' is imported while it is "
                           "being imported itself; imports cannot form a "
                           "cycle");
    if (!llvm::is_contained(current->imports, known->second))
      current->imports.push_back(known->second);
    return success();
  }
  // Names are told apart by their module's name, so that is unique.
  for (const std::unique_ptr<SourceModule> &other : modules)
    if (other->name == *name)
      return error(at, "another module named '" + *name +
                           "' is part of the program already");

  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return error(at, "cannot read '" + path + "'");
  StringRef text = (*buffer)->getBuffer();
  sourceMgr.AddNewSourceBuffer(std::move(*buffer), at);
  if (files)
    files->sources.push_back(path.str().str());

  modules.push_back(std::make_unique<SourceModule>());
  SourceModule *imported = modules.back().get();
  imported->name = *name;
  imported->prefix = *name + ".";
  modulesByPath[path] = imported;

  // Parse the file where the import stands, then go on with this one.
  Lexer savedLexer = lexer;
  Token savedToken = token;
  const char *savedEnd = previousEnd;
  SourceModule *importing = current;
  lexer = Lexer(text);
  current = imported;
  advance();
  LogicalResult result = parseDeclarations();
  lexer = savedLexer;
  token = savedToken;
  previousEnd = savedEnd;
  current = importing;
  if (failed(result))
    return failure();
  imported->loading = false;
  finished.push_back(imported);
  current->imports.push_back(imported);
  return success();
}

FailureOr<Type> Parser::parseType() {
  llvm::SMLoc at = token.loc;
  // fn(T, ...) -> U, proc(T, ...): one of the fns or procs of that shape.
  if ((token.isKeyword("fn") || token.isKeyword("proc")) &&
      peek().is(Token::LParen)) {
    bool proc = token.isKeyword("proc");
    advance();
    return parseCallableType(proc);
  }
  // rows { field: type, .. }, rows type, rows[Enum] ..: one of the tables
  // of that shape.
  if (token.isKeyword("rows") &&
      (peek().is(Token::LBrace) || peek().is(Token::LBracket) ||
       peek().is(Token::Identifier))) {
    advance();
    return parseRowsType();
  }
  // row { field: type, .. }: a row of one of the tables with such rows.
  if (token.isKeyword("row") &&
      (peek().is(Token::LBrace) || peek().isKeyword("of"))) {
    advance();
    return parseRowType();
  }
  // index of Name: a row number of that buffer.
  if (token.isKeyword("index") && peek().isKeyword("of")) {
    advance();
    advance();
    llvm::SMLoc bufferAt = token.loc;
    FailureOr<std::string> buffer = identifier("a buffer");
    if (failed(buffer))
      return failure();
    if (!buffers.count(*buffer))
      return error(bufferAt, "'index of' names a buffer declared before; '" +
                                 *buffer + "' is none");
    return Type(IndexOfType::get(context, symbol(*buffer)));
  }
  FailureOr<std::string> name = identifier("a type");
  if (failed(name))
    return failure();
  Type type = llvm::StringSwitch<Type>(*name)
                  .Case("f32", builder.getF32Type())
                  .Case("f64", builder.getF64Type())
                  .Case("bool", builder.getI1Type())
                  .Case("i1", builder.getI1Type())
                  .Case("i8", builder.getIntegerType(8))
                  .Case("i16", builder.getIntegerType(16))
                  .Case("i32", builder.getI32Type())
                  .Case("i64", builder.getI64Type())
                  .Case("u8", builder.getIntegerType(8, /*isSigned=*/false))
                  .Case("u16", builder.getIntegerType(16, /*isSigned=*/false))
                  .Case("u32", builder.getIntegerType(32, /*isSigned=*/false))
                  .Case("u64", builder.getIntegerType(64, /*isSigned=*/false))
                  .Case("index", builder.getIndexType())
                  .Case("entity", EntityType::get(context))
                  .Default(Type());
  if (*name == "text") {
    // text: of any length, held by the world; the module `text` keeps
    // the bytes.
    if (!token.is(Token::LBracket)) {
      if (current->name != "text" &&
          llvm::none_of(current->imports, [](SourceModule *module) {
            return module->name == "text";
          }))
        return error(at, "a text of any length is kept by the module "
                         "'text': 'import text' (or give it a capacity, "
                         "'text[N]')");
      return Type(StringType::get(context));
    }
    // text[N]: up to N bytes, stored inline.
    advance();
    FailureOr<int64_t> capacity = integer("a capacity in bytes");
    if (failed(capacity) || failed(expect(Token::RBracket, "']'")))
      return failure();
    if (*capacity < 1 || *capacity > TextType::kMaxCapacity)
      return error(at, "a text holds 1 to " +
                           Twine(unsigned(TextType::kMaxCapacity)) + " bytes");
    return Type(TextType::get(context, *capacity));
  }
  if (!type && enums.count(*name))
    return Type(EnumType::get(context, symbol(*name)));
  // A buffer, where an extern proc says what it is handed.
  if (!type && buffers.count(*name)) {
    if (!handsBuffers)
      return error(at, "'" + *name + "' is a buffer: a parameter of a fn "
                       "or proc may be one, and nothing else");
    // Buffer[]: rows of it, a window or all it has, by their number.
    if (token.is(Token::LBracket) && peek().is(Token::RBracket)) {
      advance();
      advance();
      return Type(RowsType::get(context, symbol(*name), false));
    }
    return Type(BufferType::get(context, symbol(*name)));
  }
  // A component, where a fn or proc says it is given the entities that
  // have it.
  if (!type && components.count(*name) && handsBuffers) {
    typedComponent = *name;
    return Type(BufferType::get(context, symbol(*name)));
  }
  if (!type)
    return error(at, "unknown type '" + *name +
                         "'; expected f32, f64, bool, i8, i16, i32, i64, "
                         "u8, u16, u32, u64, index, entity, text, text[N], "
                         "an enum or 'index of Buffer'");
  return type;
}

/// How a type is written in the name of a shape's enum.
static std::string shapeWord(Type type) {
  if (type.isInteger(1))
    return "bool";
  if (auto text = dyn_cast<TextType>(type))
    return ("text" + Twine(text.getCapacity())).str();
  if (isa<EntityType>(type))
    return "entity";
  if (isa<StringType>(type))
    return "text";
  std::string word;
  llvm::raw_string_ostream os(word);
  if (auto named = dyn_cast<EnumType>(type))
    os << named.getName().getValue();
  else if (auto index = dyn_cast<IndexOfType>(type))
    os << "index_of_" << index.getBuffer().getValue();
  else
    os << type;
  for (char &c : word)
    if (!llvm::isAlnum(c))
      c = '_';
  return word;
}

// (name: T, ...) [-> U], after `fn` or `proc`: the type of the fns or
// procs that take and give those.
FailureOr<Type> Parser::parseCallableType(bool proc) {
  llvm::SMLoc at = token.loc;
  if (failed(expect(Token::LParen, "'('")))
    return failure();
  auto callable = std::make_unique<Callable>();
  callable->proc = proc;
  while (!token.is(Token::RParen)) {
    // (A parameter may be named, for the reader.)
    if (token.is(Token::Identifier) && peek().is(Token::Colon)) {
      advance();
      advance();
    }
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    callable->params.push_back(*type);
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RParen, "')'")))
    return failure();
  if (token.is(Token::Minus) && peek().is(Token::Greater)) {
    advance();
    advance();
    llvm::SMLoc resultAt = token.loc;
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    if (!type->isIntOrFloat() && !isa<EnumType, TextType, StringType>(*type))
      return error(resultAt, "a fn or proc that is a value gives a number, "
                             "a bool, an enum or a text, or nothing");
    callable->result = *type;
  } else if (!proc) {
    return error(at, "a fn gives a value back ('-> type')");
  }
  std::string key = proc ? "proc" : "fn";
  std::string name = proc ? "proc_of" : "fn_of";
  for (Type type : callable->params) {
    llvm::raw_string_ostream(key) << " " << type;
    name += "_" + shapeWord(type);
  }
  if (callable->result) {
    llvm::raw_string_ostream(key) << " -> " << callable->result;
    name += "_to_" + shapeWord(callable->result);
  }
  auto known = callablesByKey.find(key);
  if (known != callablesByKey.end())
    return Type(EnumType::get(
        context, FlatSymbolRefAttr::get(context, known->second->name)));
  // (Two shapes whose names come out the same are told apart.)
  while (callablesByName.count(name) ||
         SymbolTable::lookupSymbolIn(module, name))
    name += "_";
  callable->name = name;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(module.getBody());
    callable->op = EnumOp::create(builder, loc(at),
                                  builder.getStringAttr(name),
                                  builder.getStrArrayAttr({"none"}));
  }
  enums.ofSymbol(name) = {"none"};
  Callable *made = callable.get();
  callablesByKey[key] = made;
  callablesByName[name] = made;
  callables.push_back(std::move(callable));
  return Type(EnumType::get(context, FlatSymbolRefAttr::get(context, name)));
}

// (`rows` is read.) [ '[' Enum ']' ] ( '{' field: type, .. '}' | type )
FailureOr<Type> Parser::parseRowsType() {
  llvm::SMLoc at = token.loc;
  auto shape = std::make_unique<RowsShape>();
  // rows of table: the tables with rows as that one's.
  if (token.isKeyword("of") && peek().is(Token::Identifier)) {
    advance();
    llvm::SMLoc tableAt = token.loc;
    FailureOr<std::string> table = identifier("a table");
    if (failed(table))
      return failure();
    if (!tables.count(*table))
      return error(tableAt, "'" + *table + "' is no table");
    shape->fields = tables[*table].fields;
    auto keyed = tableKeys.find(resolve(*table));
    if (keyed != tableKeys.end())
      shape->key = keyed->second;
  } else if (consumeIf(Token::LBracket)) {
    llvm::SMLoc keyAt = token.loc;
    FailureOr<std::string> named = identifier("an enum");
    if (failed(named) || failed(expect(Token::RBracket, "']'")))
      return failure();
    if (!enums.count(*named))
      return error(keyAt, "'" + *named + "' is no enum");
    shape->key = resolve(*named);
  }
  if (!shape->fields.empty()) {
    // (Said by a table.)
  } else if (token.is(Token::LBrace)) {
    Record record;
    if (failed(parseFields(record)))
      return failure();
    shape->fields = record.fields;
  } else {
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    shape->fields.push_back({std::string(), *type});
  }
  if (shape->fields.empty())
    return error(at, "a table's rows have at least one field");
  std::string key = "rows " + shape->key;
  std::string name = "rows_of";
  for (auto &[field, type] : shape->fields) {
    llvm::raw_string_ostream(key) << " " << field << ": " << type;
    name += "_" + (field.empty() ? std::string() : field + "_") +
            shapeWord(type);
  }
  if (!shape->key.empty()) {
    name += "_by_";
    for (char c : shape->key)
      name += llvm::isAlnum(c) ? c : '_';
  }
  auto known = rowsByKey.find(key);
  if (known != rowsByKey.end())
    return Type(EnumType::get(
        context, FlatSymbolRefAttr::get(context, known->second->name)));
  // (Two shapes whose names come out the same are told apart.)
  while (rowsByName.count(name) || SymbolTable::lookupSymbolIn(module, name))
    name += "_";
  shape->name = name;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(module.getBody());
    shape->op = EnumOp::create(builder, loc(at), builder.getStringAttr(name),
                               builder.getStrArrayAttr({"none"}));
  }
  enums.ofSymbol(name) = {"none"};
  RowsShape *made = shape.get();
  rowsByKey[key] = made;
  rowsByName[name] = made;
  rowShapes.push_back(std::move(shape));
  return Type(EnumType::get(context, FlatSymbolRefAttr::get(context, name)));
}

/// The shape a type stands for, if it is the type of tables.
RowsShape *Parser::rowsOf(Type type) {
  auto named = dyn_cast_or_null<EnumType>(type);
  if (!named)
    return nullptr;
  auto known = rowsByName.find(named.getName().getValue());
  return known == rowsByName.end() ? nullptr : known->second;
}

/// A table by its name, where one of its shape is expected: its number
/// among those of the shape that are used so.
FailureOr<mlir::Value> Parser::emitTableValue(const Expr &expr,
                                              RowsShape &shape) {
  const Record &table = tables[expr.name];
  std::string target = symbol(expr.name).getValue().str();
  auto keyed = tableKeys.find(target);
  std::string key = keyed == tableKeys.end() ? std::string() : keyed->second;
  if (table.fields != shape.fields || key != shape.key)
    return error(expr.loc, "table '" + expr.name + "' has not the rows of "
                           "the tables expected here");
  auto *known = llvm::find(shape.targets, target);
  unsigned number = known - shape.targets.begin() + 1;
  if (known == shape.targets.end()) {
    if (number >= EnumType::kMaxCases)
      return error(expr.loc, "more than " +
                                 Twine(unsigned(EnumType::kMaxCases) - 1) +
                                 " tables of one shape are used as values");
    shape.targets.push_back(target);
    std::string label = target;
    for (char &c : label)
      if (!llvm::isAlnum(c))
        c = '_';
    SmallVector<std::string> &labels = enums.ofSymbol(shape.name);
    labels.push_back(label);
    SmallVector<StringRef> all(labels.begin(), labels.end());
    shape.op->setAttr("cases", builder.getStrArrayAttr(all));
  }
  auto type =
      EnumType::get(context, FlatSymbolRefAttr::get(context, shape.name));
  Location at = loc(expr.loc);
  return UnrealizedConversionCastOp::create(
             builder, at, type, integer(at, type.getStorageType(), number))
      .getResult(0);
}

/// Nought of a type: 0, false, the first case, a text without bytes.
mlir::Value Parser::noughtOf(Location at, Type type) {
  if (auto named = dyn_cast<EnumType>(type))
    return UnrealizedConversionCastOp::create(
               builder, at, named, integer(at, named.getStorageType(), 0))
        .getResult(0);
  if (auto text = dyn_cast<TextType>(type))
    return textConstant(at, "", text);
  if (isa<StringType>(type))
    return TextConstantOp::create(builder, at, type, "").getResult();
  if (isUnsigned(type))
    return integer(at, type, 0);
  return arith::ConstantOp::create(builder, at,
                                   cast<TypedAttr>(builder.getZeroAttr(type)));
}

/// What row `index` of the table that `base` holds has in `field`: of
/// the fn that asks which table it is and reads that one (see
/// finishRows).
FailureOr<mlir::Value> Parser::emitRowThrough(const Expr &expr,
                                              RowsShape &shape,
                                              const Expr &base,
                                              const Expr &index,
                                              StringRef field) {
  Type type;
  for (auto &[name, fieldType] : shape.fields)
    if (name == field)
      type = fieldType;
  if (!type)
    return error(expr.loc,
                 field.empty() ? Twine("a row of this table has fields: "
                                       "'table[i].field'")
                               : "the rows of this table have no field '" +
                                     field + "'");
  auto held =
      EnumType::get(context, FlatSymbolRefAttr::get(context, shape.name));
  FailureOr<mlir::Value> which = emit(base, held);
  if (failed(which))
    return failure();
  Location at = loc(expr.loc);
  mlir::Value row;
  if (!shape.key.empty()) {
    auto named =
        EnumType::get(context, FlatSymbolRefAttr::get(context, shape.key));
    FailureOr<mlir::Value> picked = emit(index, named);
    if (failed(picked))
      return failure();
    if (picked->getType() != named)
      return error(index.loc, "this table has a row for each case of an "
                              "enum, and is read by one, not by a number");
    row = arith::ExtUIOp::create(
        builder, at, builder.getI32Type(),
        UnrealizedConversionCastOp::create(builder, at,
                                           named.getStorageType(), *picked)
            .getResult(0));
  } else {
    FailureOr<mlir::Value> counted = emit(index, builder.getI32Type());
    if (failed(counted))
      return failure();
    if (!counted->getType().isInteger(32))
      return error(index.loc, "a row's number is an i32 here (or less)");
    row = *counted;
  }
  shape.read.insert(field);
  auto invoke = InvokeOp::create(
      builder, at, type,
      FlatSymbolRefAttr::get(context,
                             ("row_" + shape.name + "__" + field).str()),
      ValueRange{*which, row}, UnitAttr());
  return invoke->getResult(0);
}

/// When everything is parsed, and every table that is used as a value is
/// known: for each shape, a table of all the rows of those tables, one
/// table's after another's, and two lists that say for each of them where
/// its rows start in it and how many they are (none has none). The fn
/// that reads a field through such a value reads those: no asking which
/// table it is. And the fn that says how many rows one has.
LogicalResult Parser::finishRows() {
  for (auto &shape : rowShapes) {
    if (shape->read.empty() && !shape->counted)
      continue;
    Location at = shape->op->getLoc();
    auto type =
        EnumType::get(context, FlatSymbolRefAttr::get(context, shape->name));
    Type i32 = builder.getI32Type();
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(module.getBody());

    SmallVector<Attribute> starts{builder.getI32IntegerAttr(0)};
    SmallVector<Attribute> counts{builder.getI32IntegerAttr(0)};
    SmallVector<SmallVector<Attribute>> columns(shape->fields.size());
    for (const std::string &target : shape->targets) {
      auto table = cast<TableOp>(SymbolTable::lookupSymbolIn(module, target));
      starts.push_back(builder.getI32IntegerAttr(columns[0].size()));
      counts.push_back(builder.getI32IntegerAttr(table.getNumRows()));
      for (auto [column, values] : llvm::zip(columns, table.getValues()))
        llvm::append_range(column, cast<ArrayAttr>(values));
    }
    auto declare = [&](const Twine &name, ArrayRef<StringRef> names,
                       ArrayRef<Type> types, ArrayRef<Attribute> values) {
      OperationState state(at, TableOp::getOperationName());
      state.addAttribute("sym_name", builder.getStringAttr(name));
      state.addAttribute("field_names", builder.getStrArrayAttr(names));
      state.addAttribute("field_types", builder.getTypeArrayAttr(types));
      state.addAttribute("values", builder.getArrayAttr(values));
      builder.create(state);
    };
    std::string all = "rows_" + shape->name + "__all";
    std::string start = "rows_" + shape->name + "__start";
    std::string count = "rows_" + shape->name + "__count";
    declare(count, {""}, {i32}, {builder.getArrayAttr(counts)});
    if (!shape->read.empty() && !columns[0].empty()) {
      SmallVector<StringRef> names;
      SmallVector<Type> types;
      SmallVector<Attribute> values;
      for (auto [field, column] : llvm::zip(shape->fields, columns)) {
        names.push_back(field.first);
        types.push_back(field.second);
        values.push_back(builder.getArrayAttr(column));
      }
      declare(all, names, types, values);
      declare(start, {""}, {i32}, {builder.getArrayAttr(starts)});
    }

    auto make = [&](const Twine &name, bool takesRow, Type result,
                    function_ref<mlir::Value(mlir::Value, mlir::Value)> of) {
      SmallVector<Type> params{type};
      if (takesRow)
        params.push_back(i32);
      OpBuilder::InsertionGuard inner(builder);
      auto op = FunctionOp::create(builder, at, builder.getStringAttr(name),
                                   builder.getTypeArrayAttr(params),
                                   builder.getTypeArrayAttr({result}),
                                   /*proc=*/false);
      auto *block = new Block();
      op.getBody().push_back(block);
      for (Type param : params)
        block->addArgument(param, at);
      builder.setInsertionPointToEnd(block);
      mlir::Value which = arith::ExtUIOp::create(
          builder, at, i32,
          UnrealizedConversionCastOp::create(builder, at,
                                             type.getStorageType(),
                                             block->getArgument(0))
              .getResult(0));
      mlir::Value row = takesRow ? block->getArgument(1) : mlir::Value();
      YieldOp::create(builder, at, ValueRange{of(which, row)});
    };
    auto listed = [&](StringRef list, mlir::Value which) -> mlir::Value {
      return TableAtOp::create(builder, at, i32,
                               FlatSymbolRefAttr::get(context, list),
                               builder.getStringAttr(""), which)
          .getResult();
    };
    for (auto &[field, fieldType] : shape->fields) {
      if (!shape->read.contains(field))
        continue;
      StringRef name = field;
      Type result = fieldType;
      make("row_" + shape->name + "__" + name, /*takesRow=*/true, result,
           [&](mlir::Value which, mlir::Value row) -> mlir::Value {
             // (No table of the shape is ever such a value.)
             if (columns[0].empty())
               return noughtOf(at, result);
             // A row the table has not is nought, not a row of the next.
             mlir::Value there = arith::CmpIOp::create(
                 builder, at, arith::CmpIPredicate::ult, row,
                 listed(count, which));
             auto branch = scf::IfOp::create(builder, at, result, there,
                                             /*withElseRegion=*/true);
             OpBuilder::InsertionGuard inner(builder);
             builder.setInsertionPointToStart(branch.thenBlock());
             mlir::Value read =
                 TableAtOp::create(
                     builder, at, result,
                     FlatSymbolRefAttr::get(context, all),
                     builder.getStringAttr(name),
                     arith::AddIOp::create(builder, at, listed(start, which),
                                           row))
                     .getResult();
             scf::YieldOp::create(builder, at, read);
             builder.setInsertionPointToStart(branch.elseBlock());
             scf::YieldOp::create(builder, at, noughtOf(at, result));
             return branch.getResult(0);
           });
    }
    if (shape->counted)
      make("rows_" + shape->name, /*takesRow=*/false, i32,
           [&](mlir::Value which, mlir::Value) -> mlir::Value {
             return listed(count, which);
           });
  }
  return success();
}

// (`row` is read.) '{' field: type, .. '}'
FailureOr<Type> Parser::parseRowType() {
  llvm::SMLoc at = token.loc;
  auto kind = std::make_unique<RowKind>();
  // row of table: a row with fields as that one's.
  if (consumeKeyword("of")) {
    llvm::SMLoc tableAt = token.loc;
    FailureOr<std::string> table = identifier("a table");
    if (failed(table))
      return failure();
    if (!tables.count(*table))
      return error(tableAt, "'" + *table + "' is no table");
    if (tables[*table].fieldType(""))
      return error(tableAt, "table '" + *table + "' is a plain list: a row "
                            "of it is its value");
    kind->fields = tables[*table].fields;
  } else {
    Record record;
    if (failed(parseFields(record)))
      return failure();
    kind->fields = record.fields;
  }
  if (kind->fields.empty())
    return error(at, "a table's rows have at least one field");
  std::string key = "row";
  std::string name = "row_of";
  for (auto &[field, type] : kind->fields) {
    llvm::raw_string_ostream(key) << " " << field << ": " << type;
    name += "_" + field + "_" + shapeWord(type);
  }
  auto typeOfKind = [&](StringRef symbol) {
    return Type(EnumType::get(context, FlatSymbolRefAttr::get(context, symbol),
                              32));
  };
  auto known = rowKindByKey.find(key);
  if (known != rowKindByKey.end())
    return typeOfKind(known->second->name);
  while (rowKindByName.count(name) || SymbolTable::lookupSymbolIn(module, name))
    name += "_";
  kind->name = name;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(module.getBody());
    kind->op = EnumOp::create(builder, loc(at), builder.getStringAttr(name),
                              builder.getStrArrayAttr({"none"}));
    // (More rows than a byte tells apart.)
    kind->op->setAttr("bits", builder.getI32IntegerAttr(32));
  }
  enums.ofSymbol(name) = {"none"};
  RowKind *made = kind.get();
  rowKindByKey[key] = made;
  rowKindByName[name] = made;
  rowKinds.push_back(std::move(kind));
  return typeOfKind(name);
}

/// The kind of row a type stands for, if it is the type of rows.
RowKind *Parser::rowOf(Type type) {
  auto named = dyn_cast_or_null<EnumType>(type);
  if (!named)
    return nullptr;
  auto known = rowKindByName.find(named.getName().getValue());
  return known == rowKindByName.end() ? nullptr : known->second;
}

/// Whether `expr` is a row that is not a value yet: `table[i]`,
/// `value[i]`, a name that stands for one, or `none`.
bool Parser::isRowSource(const Expr &expr) {
  if (expr.kind == Expr::Name) {
    const Variable *variable = lookup(expr.name);
    return variable ? variable->kind == Variable::Row : expr.name == "none";
  }
  return isWholeRow(expr);
}

/// Where the rows of the table `target` start among those of `kind`:
/// after those of the tables rows were taken of before.
unsigned Parser::rowsStart(RowKind &kind, StringRef target) {
  for (auto [known, start] : llvm::zip(kind.targets, kind.starts))
    if (known == target)
      return start;
  kind.targets.push_back(target.str());
  kind.starts.push_back(kind.total);
  kind.total += tableRows[target];
  return kind.starts.back();
}

/// A row (see isRowSource) as a value of `kind`: its number among all
/// the rows of the kind, from 1; none (0) where the table has no such
/// row.
FailureOr<mlir::Value> Parser::emitRowValue(const Expr &expr, RowKind &kind) {
  Location at = loc(expr.loc);
  Type i32 = builder.getI32Type();
  auto type =
      EnumType::get(context, FlatSymbolRefAttr::get(context, kind.name), 32);
  auto asRow = [&](mlir::Value number) {
    return UnrealizedConversionCastOp::create(builder, at, Type(type), number)
        .getResult(0);
  };
  // Which table, and which row of it: of a name that stands for a row,
  // or as it is written.
  std::string table;
  mlir::Value which, index;
  if (expr.kind == Expr::Name) {
    const Variable *variable = lookup(expr.name);
    if (!variable)
      return asRow(integer(at, i32, 0));
    table = variable->component;
    which = variable->which;
    index = variable->value;
  } else {
    FailureOr<Variable> row = emitRow(expr);
    if (failed(row))
      return failure();
    table = row->component;
    which = row->which;
    index = row->value;
  }
  // (The row's number as an i32: a case as its number.)
  if (auto named = dyn_cast<EnumType>(index.getType()))
    index = UnrealizedConversionCastOp::create(builder, at,
                                               named.getStorageType(), index)
                .getResult(0);
  unsigned width = cast<IntegerType>(index.getType()).getWidth();
  if (width < 32)
    index = arith::ExtUIOp::create(builder, at, i32, index);
  else if (width > 32)
    index = arith::TruncIOp::create(builder, at, i32, index);

  if (which) {
    // Of the table a value holds: asked of the fn that knows where the
    // rows of each such table are (see finishRowKinds).
    RowsShape *shape = rowsOf(which.getType());
    if (shape->fields != kind.fields)
      return error(expr.loc, "the rows of this table are not the rows "
                             "expected here");
    if (!llvm::is_contained(kind.through, shape))
      kind.through.push_back(shape);
    shape->counted = true;
    return InvokeOp::create(
               builder, at, Type(type),
               FlatSymbolRefAttr::get(context, "row_" + kind.name + "__of_" +
                                                   shape->name),
               ValueRange{which, index}, UnitAttr())
        ->getResult(0);
  }
  if (tables[table].fields != kind.fields)
    return error(expr.loc, "a row of table '" + table + "' is not the row "
                           "expected here");
  std::string target = resolve(table);
  unsigned start = rowsStart(kind, target);
  mlir::Value there = arith::CmpIOp::create(
      builder, at, arith::CmpIPredicate::ult, index,
      integer(at, i32, tableRows[target]));
  mlir::Value number = arith::AddIOp::create(builder, at, index,
                                             integer(at, i32, start + 1));
  mlir::Value picked = arith::SelectOp::create(builder, at, there, number,
                                               integer(at, i32, 0));
  return asRow(picked);
}

/// What the row that `row` is has in `field`: of the fn that reads the
/// table of all the rows of its kind (see finishRowKinds).
FailureOr<mlir::Value> Parser::readRowValue(llvm::SMLoc where, mlir::Value row,
                                            StringRef field) {
  RowKind *kind = rowOf(row.getType());
  Type type;
  for (auto &[name, fieldType] : kind->fields)
    if (name == field)
      type = fieldType;
  if (!type)
    return error(where, "this row has no field '" + field + "'");
  kind->read.insert(field);
  return InvokeOp::create(
             builder, loc(where), type,
             FlatSymbolRefAttr::get(context,
                                    ("row_" + kind->name + "__" + field).str()),
             ValueRange{row}, UnitAttr())
      ->getResult(0);
}

/// When everything is parsed: for each kind of row, the table of all the
/// rows of the tables rows are taken of; the fns that read a field of
/// such a value; and for each shape of table values rows are taken
/// through, the fn that makes of a table and a row's number in it the
/// row's number among all.
LogicalResult Parser::finishRowKinds() {
  for (auto &kind : rowKinds) {
    Location at = kind->op->getLoc();
    auto type =
        EnumType::get(context, FlatSymbolRefAttr::get(context, kind->name), 32);
    Type i32 = builder.getI32Type();
    // (Every table that may be the one a value holds.)
    for (RowsShape *shape : kind->through)
      for (const std::string &target : shape->targets)
        rowsStart(*kind, target);
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(module.getBody());
    auto declare = [&](const Twine &name, ArrayRef<StringRef> names,
                       ArrayRef<Type> types, ArrayRef<Attribute> values) {
      OperationState state(at, TableOp::getOperationName());
      state.addAttribute("sym_name", builder.getStringAttr(name));
      state.addAttribute("field_names", builder.getStrArrayAttr(names));
      state.addAttribute("field_types", builder.getTypeArrayAttr(types));
      state.addAttribute("values", builder.getArrayAttr(values));
      builder.create(state);
    };
    auto make = [&](const Twine &name, ArrayRef<Type> params, Type result,
                    function_ref<mlir::Value(Block &)> of) {
      OpBuilder::InsertionGuard inner(builder);
      auto op = FunctionOp::create(builder, at, builder.getStringAttr(name),
                                   builder.getTypeArrayAttr(params),
                                   builder.getTypeArrayAttr({result}),
                                   /*proc=*/false);
      auto *block = new Block();
      op.getBody().push_back(block);
      for (Type param : params)
        block->addArgument(param, at);
      builder.setInsertionPointToEnd(block);
      YieldOp::create(builder, at, ValueRange{of(*block)});
    };
    std::string all = "row_" + kind->name + "__all";
    if (kind->total) {
      SmallVector<SmallVector<Attribute>> columns(kind->fields.size());
      for (const std::string &target : kind->targets) {
        auto table =
            cast<TableOp>(SymbolTable::lookupSymbolIn(module, target));
        for (auto [column, values] : llvm::zip(columns, table.getValues()))
          llvm::append_range(column, cast<ArrayAttr>(values));
      }
      SmallVector<StringRef> names;
      SmallVector<Type> types;
      SmallVector<Attribute> values;
      for (auto [field, column] : llvm::zip(kind->fields, columns)) {
        names.push_back(field.first);
        types.push_back(field.second);
        values.push_back(builder.getArrayAttr(column));
      }
      declare(all, names, types, values);
    }
    for (auto &[field, fieldType] : kind->fields) {
      if (!kind->read.contains(field))
        continue;
      StringRef name = field;
      Type result = fieldType;
      make("row_" + kind->name + "__" + name, {type}, result,
           [&](Block &block) -> mlir::Value {
             // (No table has such a row: every one is none.)
             if (!kind->total)
               return noughtOf(at, result);
             // Row 0 is none: before the first, which reads as nought.
             mlir::Value number = UnrealizedConversionCastOp::create(
                                      builder, at, i32, block.getArgument(0))
                                      .getResult(0);
             return TableAtOp::create(
                        builder, at, result,
                        FlatSymbolRefAttr::get(context, all),
                        builder.getStringAttr(name),
                        arith::SubIOp::create(builder, at, number,
                                              integer(at, i32, 1)))
                 .getResult();
           });
    }
    for (RowsShape *shape : kind->through) {
      // For each table of the shape (none first), where its rows start.
      SmallVector<Attribute> starts{builder.getI32IntegerAttr(0)};
      for (const std::string &target : shape->targets)
        starts.push_back(builder.getI32IntegerAttr(rowsStart(*kind, target)));
      std::string start = "row_" + kind->name + "__in_" + shape->name;
      declare(start, {""}, {i32}, {builder.getArrayAttr(starts)});
      auto held =
          EnumType::get(context, FlatSymbolRefAttr::get(context, shape->name));
      make("row_" + kind->name + "__of_" + shape->name, {held, i32}, type,
           [&](Block &block) -> mlir::Value {
             mlir::Value which = arith::ExtUIOp::create(
                 builder, at, i32,
                 UnrealizedConversionCastOp::create(
                     builder, at, held.getStorageType(), block.getArgument(0))
                     .getResult(0));
             mlir::Value index = block.getArgument(1);
             auto listed = [&](StringRef list) -> mlir::Value {
               return TableAtOp::create(builder, at, i32,
                                        FlatSymbolRefAttr::get(context, list),
                                        builder.getStringAttr(""), which)
                   .getResult();
             };
             mlir::Value there = arith::CmpIOp::create(
                 builder, at, arith::CmpIPredicate::ult, index,
                 listed("rows_" + shape->name + "__count"));
             mlir::Value number = arith::AddIOp::create(
                 builder, at,
                 arith::AddIOp::create(builder, at, listed(start), index),
                 integer(at, i32, 1));
             mlir::Value row = arith::SelectOp::create(
                 builder, at, there, number, integer(at, i32, 0));
             return UnrealizedConversionCastOp::create(builder, at,
                                                       Type(type), row)
                 .getResult(0);
           });
    }
  }
  return success();
}

/// Whether `expr` is a whole row of a table whose rows have fields:
/// `table[i]`, or `value[i]` of the table a value holds.
bool Parser::isWholeRow(const Expr &expr) {
  if (expr.kind == Expr::Row && !expr.name.empty() && expr.field.empty())
    return !tables[expr.name].fieldType("");
  if (expr.kind == Expr::Index)
    if (RowsShape *shape = rowsOf(typeOf(*expr.operands[0])))
      return !shape->fields[0].first.empty();
  return false;
}

/// The row that `expr` is (see isWholeRow), for a name to stand for:
/// what says which row it is is worked out here, once; its fields are
/// read where they are asked for.
FailureOr<Variable> Parser::emitRow(const Expr &expr) {
  bool named = expr.kind == Expr::Row;
  const Expr &index = *expr.operands[named ? 0 : 1];
  mlir::Value which;
  if (!named) {
    FailureOr<mlir::Value> held =
        emit(*expr.operands[0], typeOf(*expr.operands[0]));
    if (failed(held))
      return failure();
    which = *held;
  }
  Type type = typeOf(index);
  if (!type)
    type = builder.getI32Type();
  FailureOr<mlir::Value> row = emit(index, type);
  if (failed(row))
    return failure();
  Variable variable = Variable::ofRow(named ? expr.name : "", which, *row);
  // (Whether the table is read by such a value is said here, where the
  // row is written, by reading its first field.)
  StringRef first = named ? StringRef(tables[expr.name].fields[0].first)
                          : StringRef(rowsOf(which.getType())->fields[0].first);
  Expr probe = fieldOfRow(variable, first, index.loc);
  OpBuilder::InsertionGuard guard(builder);
  FailureOr<mlir::Value> read = emit(probe, Type());
  if (failed(read))
    return failure();
  if (Operation *op = read->getDefiningOp(); op && op->use_empty())
    if (isa<TableAtOp>(op))
      op->erase();
  return variable;
}

/// `row.field`, as the read of the table it is: by the values that are
/// there already.
Expr Parser::fieldOfRow(const Variable &row, StringRef field,
                        llvm::SMLoc at) {
  auto given = [&](mlir::Value value) {
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Given;
    node->loc = at;
    node->given = value;
    return node;
  };
  Expr read;
  read.kind = Expr::Row;
  read.loc = at;
  read.name = row.component;
  read.field = field.str();
  read.operands.push_back(given(row.value));
  if (row.which)
    read.operands.push_back(given(row.which));
  return read;
}

/// The shape a type stands for, if it is the type of fns or procs.
Callable *Parser::callableOf(Type type) {
  auto named = dyn_cast_or_null<EnumType>(type);
  if (!named)
    return nullptr;
  auto known = callablesByName.find(named.getName().getValue());
  return known == callablesByName.end() ? nullptr : known->second;
}

/// The shape of what the name `name` holds, if that is a fn or proc.
Callable *Parser::callableNamed(StringRef name) {
  const Variable *variable = lookup(name);
  if (!variable || variable->kind != Variable::Value)
    return nullptr;
  return callableOf(variable->value.getType());
}

/// The shape of what a call is through, if it is through a name or a
/// field that holds a fn or proc (`paint(...)`, `c.draw(...)`).
Callable *Parser::callableThrough(const Expr &call) {
  if (call.field.empty())
    return callableNamed(call.name);
  Expr held;
  held.kind = Expr::Field;
  held.loc = call.loc;
  held.name = call.name;
  held.field = call.field;
  return callableOf(typeOf(held));
}

/// The name of a fn or proc where a value of the shape `callable` is
/// expected: that function, as one of the shape's.
FailureOr<mlir::Value> Parser::emitFunctionValue(const Expr &expr,
                                                 Callable &callable) {
  Function &function = functions[expr.name];
  StringRef kind = callable.proc ? "proc" : "fn";
  if (function.proc != callable.proc)
    return error(expr.loc, "'" + expr.name + "' is a " +
                               (function.proc ? "proc" : "fn") + ", and a " +
                               kind + " is expected here");
  if (TypeRange(function.params) != TypeRange(callable.params) ||
      function.results.size() != (callable.result ? 1u : 0u) ||
      (callable.result && function.result != callable.result))
    return error(expr.loc, "'" + expr.name + "' does not take and give "
                           "what the " + kind + " expected here does");
  std::string target = symbol(expr.name).getValue().str();
  auto *known = llvm::find(callable.targets, target);
  unsigned number = known - callable.targets.begin() + 1;
  if (known == callable.targets.end()) {
    if (number >= EnumType::kMaxCases)
      return error(expr.loc, "more than " +
                                 Twine(unsigned(EnumType::kMaxCases) - 1) +
                                 " fns or procs of one shape are used as "
                                 "values");
    callable.targets.push_back(target);
    std::string label = target;
    for (char &c : label)
      if (!llvm::isAlnum(c))
        c = '_';
    SmallVector<std::string> &labels = enums.ofSymbol(callable.name);
    labels.push_back(label);
    SmallVector<StringRef> all(labels.begin(), labels.end());
    callable.op->setAttr("cases", builder.getStrArrayAttr(all));
  }
  auto type =
      EnumType::get(context, FlatSymbolRefAttr::get(context, callable.name));
  Location at = loc(expr.loc);
  return UnrealizedConversionCastOp::create(
             builder, at, type, integer(at, type.getStorageType(), number))
      .getResult(0);
}

/// A call through a name that holds a fn or proc: of the fn that asks
/// which one it is and calls that (see finishCallables).
FailureOr<SmallVector<mlir::Value>>
Parser::emitCallThrough(const Expr &expr, Callable &callable) {
  if (!inSystem)
    return error(expr.loc, "'" + expr.name + "' can only be called in a "
                           "system, a fn or a proc");
  if (inFunction && callable.proc && !inProc)
    return error(expr.loc, "'" + expr.name + "' is a proc: it acts, and a "
                           "fn only computes");
  if (expr.operands.size() != callable.params.size())
    return error(expr.loc, "'" + expr.name + "' takes " +
                               Twine(callable.params.size()) +
                               " argument(s), not " +
                               Twine(expr.operands.size()));
  SmallVector<mlir::Value> args;
  if (expr.field.empty()) {
    args.push_back(lookup(expr.name)->value);
  } else {
    Expr held;
    held.kind = Expr::Field;
    held.loc = expr.loc;
    held.name = expr.name;
    held.field = expr.field;
    FailureOr<mlir::Value> value = emit(
        held, EnumType::get(context,
                            FlatSymbolRefAttr::get(context, callable.name)));
    if (failed(value))
      return failure();
    args.push_back(*value);
  }
  for (auto [operand, type] : llvm::zip(expr.operands, callable.params)) {
    FailureOr<mlir::Value> arg = emit(*operand, type);
    if (failed(arg))
      return failure();
    if (arg->getType() != type)
      return error(operand->loc, "argument has a different type than the "
                                 "parameter");
    args.push_back(*arg);
  }
  callable.called = true;
  SmallVector<Type> results;
  if (callable.result)
    results.push_back(callable.result);
  auto invoke = InvokeOp::create(
      builder, loc(expr.loc), results,
      FlatSymbolRefAttr::get(context, "call_" + callable.name), args,
      callable.proc ? builder.getUnitAttr() : UnitAttr());
  return SmallVector<mlir::Value>(invoke.getResults());
}

/// When everything is parsed, and every fn and proc that is used as a
/// value is known: for each shape something calls through, the function
/// that takes one of them and what it takes, and calls the one it is.
/// None does nothing, and gives nought.
LogicalResult Parser::finishCallables() {
  for (auto &callable : callables) {
    if (!callable->called)
      continue;
    Location at = callable->op->getLoc();
    auto type = EnumType::get(
        context, FlatSymbolRefAttr::get(context, callable->name));
    SmallVector<Type> params{type};
    params.append(callable->params.begin(), callable->params.end());
    SmallVector<Type> results;
    if (callable->result)
      results.push_back(callable->result);
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(module.getBody());
    auto op = FunctionOp::create(
        builder, at, builder.getStringAttr("call_" + callable->name),
        builder.getTypeArrayAttr(params), builder.getTypeArrayAttr(results),
        callable->proc);
    auto *block = new Block();
    op.getBody().push_back(block);
    for (Type param : params)
      block->addArgument(param, at);
    builder.setInsertionPointToEnd(block);
    mlir::Value which =
        UnrealizedConversionCastOp::create(builder, at, type.getStorageType(),
                                           block->getArgument(0))
            .getResult(0);
    ArrayRef<BlockArgument> given = block->getArguments().drop_front();
    // One after another: is it this one? Then that is called.
    std::function<mlir::Value(unsigned)> from = [&](unsigned index)
        -> mlir::Value {
      if (index == callable->targets.size()) {
        if (!callable->result)
          return {};
        if (auto named = dyn_cast<EnumType>(callable->result))
          return UnrealizedConversionCastOp::create(
                     builder, at, named,
                     integer(at, named.getStorageType(), 0))
              .getResult(0);
        if (auto text = dyn_cast<TextType>(callable->result))
          return textConstant(at, "", text);
        if (isa<StringType>(callable->result))
          return TextConstantOp::create(builder, at, callable->result, "")
              .getResult();
        return noughtOf(at, callable->result);
      }
      mlir::Value is = arith::CmpIOp::create(
          builder, at, arith::CmpIPredicate::eq, which,
          integer(at, type.getStorageType(), index + 1));
      auto branch = scf::IfOp::create(builder, at, results, is,
                                      /*withElseRegion=*/true);
      {
        OpBuilder::InsertionGuard inner(builder);
        builder.setInsertionPointToStart(branch.thenBlock());
        auto invoke = InvokeOp::create(
            builder, at, results,
            FlatSymbolRefAttr::get(context, callable->targets[index]),
            ValueRange(given),
            callable->proc ? builder.getUnitAttr() : UnitAttr());
        if (callable->result)
          scf::YieldOp::create(builder, at, invoke.getResults());
        builder.setInsertionPointToStart(branch.elseBlock());
        mlir::Value other = from(index + 1);
        if (callable->result)
          scf::YieldOp::create(builder, at, other);
      }
      return callable->result ? branch.getResult(0) : mlir::Value();
    };
    mlir::Value value = from(0);
    YieldOp::create(builder, at,
                    value ? ValueRange(value) : ValueRange());
  }
  return success();
}

// enum Name { First, Second, ... }
LogicalResult Parser::parseEnum() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("an enum's name");
  if (failed(name) || failed(expect(Token::LBrace, "'{'")))
    return failure();
  SmallVector<std::string> cases;
  SmallVector<StringRef> labels;
  while (!token.is(Token::RBrace)) {
    llvm::SMLoc caseAt = token.loc;
    FailureOr<std::string> label = identifier("a case");
    if (failed(label))
      return failure();
    if (llvm::is_contained(cases, *label))
      return error(caseAt, "enum '" + *name + "' has the case '" + *label +
                               "' twice");
    cases.push_back(*label);
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RBrace, "'}'")))
    return failure();
  if (cases.empty())
    return error(at, "an enum has at least one case");
  if (cases.size() > EnumType::kMaxCases)
    return error(at, "an enum has at most " +
                         Twine(unsigned(EnumType::kMaxCases)) + " cases");
  for (const std::string &label : cases)
    labels.push_back(label);
  EnumOp::create(builder, loc(at), declareSymbol(at, *name),
                 builder.getStrArrayAttr(labels));
  enums[*name] = std::move(cases);
  return success();
}

/// What `expr` is as a value of `type` that is known as it is written: a
/// number, a bool, a case of an enum, a text; as it is stored.
FailureOr<Attribute> Parser::constantOf(const Expr &expr, Type type) {
  const Expr *value = &expr;
  bool negative = false;
  if (expr.kind == Expr::Unary && expr.op == Token::Minus) {
    negative = true;
    value = expr.operands[0].get();
  }
  if (auto real = dyn_cast<FloatType>(type)) {
    if (value->kind != Expr::Int && value->kind != Expr::Float)
      return error(expr.loc, "expected a number as it is written");
    double number = value->kind == Expr::Int ? double(value->intValue)
                                             : value->floatValue;
    return Attribute(builder.getFloatAttr(real, negative ? -number : number));
  }
  if (type.isInteger(1)) {
    if (value->kind != Expr::Bool || negative)
      return error(expr.loc, "expected 'true' or 'false'");
    return Attribute(builder.getIntegerAttr(type, value->boolValue));
  }
  if (auto whole = dyn_cast<IntegerType>(type)) {
    if (value->kind != Expr::Int)
      return error(expr.loc, "expected a whole number as it is written");
    if (whole.isUnsigned()) {
      if (negative && value->intValue != 0)
        return error(expr.loc, "a u" + Twine(whole.getWidth()) +
                                   " is not less than nought");
      if (whole.getWidth() < 64 &&
          uint64_t(value->intValue) >> whole.getWidth() != 0)
        return error(expr.loc, "this number is more than a u" +
                                   Twine(whole.getWidth()) + " holds");
    }
    return Attribute(builder.getIntegerAttr(
        whole, APInt(whole.getWidth(),
                     uint64_t(negative ? -value->intValue : value->intValue),
                     /*isSigned=*/true, /*implicitTrunc=*/true)));
  }
  if (auto named = dyn_cast<EnumType>(type)) {
    StringRef symbol = named.getName().getValue();
    if (value->kind != Expr::Field || negative ||
        resolve(value->name) != symbol)
      return error(expr.loc, "expected a case of the enum");
    const SmallVector<std::string> &cases = enums.ofSymbol(symbol);
    auto *at = llvm::find(cases, value->field);
    if (at == cases.end())
      return error(expr.loc, "the enum has no case '" + value->field + "'");
    return Attribute(
        builder.getIntegerAttr(named.getStorageType(), at - cases.begin()));
  }
  if (auto text = dyn_cast<TextType>(type)) {
    if (value->kind != Expr::String || negative)
      return error(expr.loc, "expected a text as it is written");
    if (value->name.size() > text.getCapacity())
      return error(expr.loc, "this text has " + Twine(value->name.size()) +
                                 " bytes, but the field is a text[" +
                                 Twine(text.getCapacity()) + "]");
    IntegerType storage = text.getStorageType();
    APInt bits(storage.getWidth(), value->name.size());
    for (auto [index, byte] : llvm::enumerate(value->name))
      bits.insertBits(static_cast<unsigned char>(byte), 16 + 8 * index, 8);
    return Attribute(IntegerAttr::get(storage, bits));
  }
  if (isa<StringType>(type)) {
    if (value->kind != Expr::String || negative)
      return error(expr.loc, "expected a text as it is written");
    return Attribute(builder.getStringAttr(value->name));
  }
  return error(expr.loc, "a table holds numbers, bools, cases of enums and "
                         "texts");
}

// table name { field: type, ... } = [ { field: value, ... }, ... ]
// table name: type = [ value, ... ]
// table name[Enum] ... = [ Case: row or value, ... ]: one for each case.
/// Whether a const's value is made of what one may be: numbers and bools
/// as they are written, other consts, and what computes with them.
static bool isWrittenOut(const Expr &expr) {
  switch (expr.kind) {
  case Expr::Int:
  case Expr::Float:
  case Expr::Bool:
  case Expr::Const:
    return true;
  case Expr::Unary:
  case Expr::Binary:
    return llvm::all_of(expr.operands, [](const ExprPtr &operand) {
      return isWrittenOut(*operand);
    });
  case Expr::Cast:
    return (expr.castType.isIntOrFloat() || expr.castType.isIndex()) &&
           isWrittenOut(*expr.operands[0]);
  default:
    return false;
  }
}

// const NAME = value, const NAME: type = value
LogicalResult Parser::parseConst() {
  advance();
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a const's name");
  if (failed(name))
    return failure();
  if (current->names.contains(*name))
    return error(at, "'" + *name + "' is already declared");
  Type type;
  if (consumeIf(Token::Colon)) {
    llvm::SMLoc typeAt = token.loc;
    FailureOr<Type> written = parseType();
    if (failed(written))
      return failure();
    if (!written->isIntOrFloat() && !written->isIndex())
      return error(typeAt, "a const is a number or a bool");
    type = *written;
  }
  if (failed(expect(Token::Assign, "'=' and the const's value")))
    return failure();
  llvm::SMLoc valueAt = token.loc;
  FailureOr<ExprPtr> value = parseExpr();
  if (failed(value))
    return failure();
  if (!isWrittenOut(**value))
    return error(valueAt, "a const's value is written out: numbers and "
                          "bools, other consts, and what computes with "
                          "them ('1 << 16', 'ANG45 / 45')");
  if (!type)
    type = typeOf(**value);
  if (!type)
    type = defaultType(**value);
  // Written once here, to see that it is a value of its type; what it
  // makes is thrown away.
  {
    Block scratch;
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(&scratch);
    FailureOr<mlir::Value> made = emit(**value, type);
    if (failed(made))
      return failure();
    if (made->getType() != type)
      return mismatch(valueAt, made->getType(), type);
  }
  declareSymbol(at, *name);
  Constant &constant = constants[*name];
  constant.type = type;
  constant.value = std::move(*value);
  return success();
}

LogicalResult Parser::parseTable() {
  advance();
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a table's name");
  if (failed(name))
    return failure();
  // [Enum]: a row for every case of it, which says which row is read.
  std::string key;
  if (consumeIf(Token::LBracket)) {
    llvm::SMLoc keyAt = token.loc;
    FailureOr<std::string> named = identifier("an enum");
    if (failed(named) || failed(expect(Token::RBracket, "']'")))
      return failure();
    if (!enums.count(*named))
      return error(keyAt, "'" + *named + "' is no enum: a table has a row "
                          "for each case of one ('table " + *name +
                              "[Enum]'), or rows that are counted");
    key = *named;
  }
  // (The case each row is for, in the order the rows are written.)
  SmallVector<unsigned> order;
  Record record;
  bool plain = consumeIf(Token::Colon);
  if (plain) {
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    record.fields.push_back({std::string(), *type});
  } else if (failed(parseFields(record))) {
    return failure();
  }
  if (record.fields.empty())
    return error(at, "a table's rows have at least one field");
  for (auto &[field, type] : record.fields)
    if (!type.isIntOrFloat() && !isa<EnumType, TextType, StringType>(type))
      return error(at, "a table holds numbers, bools, cases of enums and "
                       "texts");
  if (failed(expect(Token::Assign, "'=' and the table's rows")) ||
      failed(expect(Token::LBracket, "'[' and the table's rows")))
    return failure();
  SmallVector<SmallVector<Attribute>> columns(record.fields.size());
  while (!token.is(Token::RBracket)) {
    llvm::SMLoc rowAt = token.loc;
    if (!key.empty()) {
      FailureOr<std::string> label = identifier("a case of '" + key + "'");
      if (failed(label) || failed(expect(Token::Colon, "':' and its row")))
        return failure();
      const SmallVector<std::string> &cases = enums[key];
      auto *found = llvm::find(cases, *label);
      if (found == cases.end())
        return error(rowAt, "'" + key + "' has no case '" + *label + "'");
      if (llvm::is_contained(order, unsigned(found - cases.begin())))
        return error(rowAt, "'" + *label + "' has its row already");
      order.push_back(found - cases.begin());
    }
    if (plain) {
      FailureOr<ExprPtr> value = parseExpr();
      if (failed(value))
        return failure();
      FailureOr<Attribute> stored =
          constantOf(**value, record.fields[0].second);
      if (failed(stored))
        return failure();
      columns[0].push_back(*stored);
    } else {
      // { field: value, ... }: every field, once.
      if (failed(expect(Token::LBrace, "'{' and a row")))
        return failure();
      SmallVector<Attribute> row(record.fields.size());
      while (!token.is(Token::RBrace)) {
        llvm::SMLoc fieldAt = token.loc;
        FailureOr<std::string> field = identifier("a field");
        if (failed(field) || failed(expect(Token::Colon, "':'")))
          return failure();
        unsigned place = 0;
        while (place < record.fields.size() &&
               record.fields[place].first != *field)
          ++place;
        if (place == record.fields.size())
          return error(fieldAt, "table '" + *name + "' has no field '" +
                                    *field + "'");
        if (row[place])
          return error(fieldAt, "'" + *field + "' is given twice");
        FailureOr<ExprPtr> value = parseExpr();
        if (failed(value))
          return failure();
        FailureOr<Attribute> stored =
            constantOf(**value, record.fields[place].second);
        if (failed(stored))
          return failure();
        row[place] = *stored;
        if (!consumeIf(Token::Comma))
          break;
      }
      if (failed(expect(Token::RBrace, "'}'")))
        return failure();
      for (auto [place, value] : llvm::enumerate(row)) {
        if (!value)
          return error(rowAt, "this row needs a value for '" +
                                  record.fields[place].first + "'");
        columns[place].push_back(value);
      }
    }
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RBracket, "']'")))
    return failure();
  if (columns[0].empty())
    return error(at, "a table has at least one row");
  if (!key.empty()) {
    // Every case, and the rows in the order of the cases.
    const SmallVector<std::string> &cases = enums[key];
    for (auto [number, label] : llvm::enumerate(cases))
      if (!llvm::is_contained(order, unsigned(number)))
        return error(at, "table '" + *name + "' has no row for '" + label +
                             "': it has one for every case of '" + key + "'");
    for (SmallVector<Attribute> &column : columns) {
      SmallVector<Attribute> sorted(column.size());
      for (auto [written, number] : llvm::enumerate(order))
        sorted[number] = column[written];
      column = std::move(sorted);
    }
  }
  SmallVector<StringRef> names;
  SmallVector<Type> types;
  SmallVector<Attribute> values;
  for (auto [field, column] : llvm::zip(record.fields, columns)) {
    names.push_back(field.first);
    types.push_back(field.second);
    values.push_back(builder.getArrayAttr(column));
  }
  StringAttr symbolName = declareSymbol(at, *name);
  OperationState state(loc(at), TableOp::getOperationName());
  state.addAttribute("sym_name", symbolName);
  state.addAttribute("field_names", builder.getStrArrayAttr(names));
  state.addAttribute("field_types", builder.getTypeArrayAttr(types));
  state.addAttribute("values", builder.getArrayAttr(values));
  if (!key.empty()) {
    state.addAttribute("key", symbol(key));
    tableKeys[symbolName.getValue()] = resolve(key);
  }
  builder.create(state);
  tableRows[symbolName.getValue()] = columns[0].size();
  tables[*name] = std::move(record);
  return success();
}

// `{ name: type, ... }`
LogicalResult Parser::parseFields(Record &record) {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  while (!token.is(Token::RBrace)) {
    FailureOr<std::string> name = identifier("a field name");
    if (failed(name) || failed(expect(Token::Colon, "':'")))
      return failure();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    record.fields.push_back({*name, *type});
    if (!consumeIf(Token::Comma))
      break;
  }
  return expect(Token::RBrace, "'}'");
}

// buffer Name { fields } capacity N
LogicalResult Parser::parseBuffer() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a buffer name");
  if (failed(name))
    return failure();
  Record record;
  llvm::SMLoc fieldsAt = token.loc;
  if (failed(parseFields(record)))
    return failure();
  if (record.fields.empty())
    return error(fieldsAt, "a buffer's rows have fields");
  for (auto &[field, type] : record.fields)
    if (!isa<IntegerType, FloatType, EnumType, EntityType, IndexOfType>(type))
      return error(fieldsAt, "'" + field + "': a buffer's fields are "
                             "numbers, bools, enums, entities and row "
                             "numbers ('index of Name')");
  if (!token.isKeyword("capacity"))
    return error(at, "a buffer says how many rows it holds at most: 'capacity "
                 "N' (a program gives a module's another with 'capacity " +
                 *name + " N')");
  advance();
  FailureOr<int64_t> capacity = integer("a capacity");
  if (failed(capacity))
    return failure();
  if (*capacity <= 0)
    return error(at, "a capacity is at least 1");
  SmallVector<Attribute> names, types;
  for (auto &[field, type] : record.fields) {
    names.push_back(builder.getStringAttr(field));
    types.push_back(TypeAttr::get(type));
  }
  BufferOp declared = BufferOp::create(
      builder, loc(at), declareSymbol(at, *name), builder.getArrayAttr(names),
      builder.getArrayAttr(types), builder.getI64IntegerAttr(*capacity));
  if (transientNext)
    declared->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
  buffers[*name] = std::move(record);
  return success();
}

// set Name: entities, each once: a bit for each entity key, in a buffer
// of words (marked ent.set) as many as the layout says.
LogicalResult Parser::parseSet() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a set name");
  if (failed(name))
    return failure();
  BufferOp declared = BufferOp::create(
      builder, loc(at), declareSymbol(at, *name),
      builder.getArrayAttr({builder.getStringAttr("bits")}),
      builder.getArrayAttr({TypeAttr::get(builder.getI64Type())}),
      builder.getI64IntegerAttr(1));
  declared->setAttr(WorldLayout::kSetAttr, builder.getUnitAttr());
  declared->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
  sets[*name] = true;
  consumeIf(Token::Semicolon);
  return success();
}

// Name += e, Name -= e: an entity into the set, or out of it, at once (in
// a `for`, from any thread; see ent.set.add). Name = [], none; Name =
// Other, the entities of another set.
LogicalResult Parser::parseSetStatement(const std::string &name,
                                        llvm::SMLoc at) {
  FlatSymbolRefAttr target = symbol(name);
  if (inFunction)
    return error(at, "'" + name +
                         "' is a set, which a system changes; a "
                         "fn or proc does not");
  if (token.is(Token::PlusAssign) || token.is(Token::MinusAssign)) {
    bool remove = token.is(Token::MinusAssign);
    advance();
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    FailureOr<mlir::Value> entity = emit(**value, EntityType::get(context));
    if (failed(entity))
      return failure();
    if (!isa<EntityType>(entity->getType()))
      return mismatch((*value)->loc, entity->getType(),
                      EntityType::get(context));
    SetAddOp::create(builder, loc(at), target, *entity,
                     remove ? builder.getUnitAttr() : UnitAttr());
    return success();
  }
  if (!consumeIf(Token::Assign))
    return error(at, "'" + name + "' is a set: '" + name +
                         " += e' puts an "
                         "entity into it, '" +
                         name + " -= e' takes one out, '" + name +
                         " = []' empties it, '" + name +
                         " = Other' "
                         "gives it the entities of another");
  if (inQuery && !inTurn)
    return error(at, "a set is emptied or given another's entities outside "
                     "a 'for': every entity would");
  if (consumeIf(Token::LBracket)) {
    if (failed(expect(Token::RBracket, "']': '" + name +
                                           " = []' empties "
                                           "the set")))
      return failure();
    ClearOp::create(builder, loc(at), target);
    return success();
  }
  llvm::SMLoc fromAt = token.loc;
  FailureOr<std::string> from = identifier("a set");
  if (failed(from))
    return failure();
  if (!sets.count(*from))
    return error(fromAt, "a set is given the entities of another set: '" +
                             *from + "' is none");
  if (*from != name)
    BufferCopyOp::create(builder, loc(at), target, symbol(*from));
  return success();
}

// In what an index of a grid declares for itself: the most cells it may
// have, until the capacity of its first component is known
// (finishIndexOrders).
static constexpr int64_t kCellsPlaceholder = 918273645;
// In what a hash index declares: how many slots its table has, until the
// capacity of its first component is known (finishIndexOrders).
static constexpr int64_t kSlotsPlaceholder = 918273644;
// (kTangledPlaceholder, Ent/EntOps.h: how out of order a grid index is.)

// From how many entities on an index's update writes the rows on many
// threads (where the program is built for them).
static constexpr int64_t kIndexParallelMin = 16384;

// index Name of Component, .. by field { name: Component.field, .. }
//
// The entities that have every component named, in the order of a whole
// number of the first one's, as the rows of a buffer the program keeps
// and only reads: each row the entity (`who`), its key and the fields
// copied after it. The first component keeps where each entity's row is
// (a field of its own, which a spawn does not give). The index declares
// for itself, as ent-lang, a system that fills it (`Name_update`: the
// entities by their key, then each told its row), one that marks it out
// of date (`Name_stale`), the unique that says whether it is
// (`Name_fresh`) and the fn that halves its rows (`Name_find`); the pass
// --ent-indexes runs the first before what reads the index where the
// second has run after what changed what it holds.
LogicalResult Parser::parseIndex() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("an index name");
  if (failed(name))
    return failure();
  if (!consumeKeyword("of"))
    return error("expected 'of' and the components of the entities it "
                 "holds: 'index " + *name + " of Component by field'");
  // Of a relation: each entity's edges in the order of a field of theirs
  // (`by weight`), or of the entity at the other end (`by source, target`:
  // out of an entity by their targets, into it by their sources), which a
  // loop over them `by` that field, or `where` the other end is a given
  // entity, reads (see EdgesOp).
  if (token.is(Token::Identifier) && relations.count(token.spelling.str()) &&
      !components.count(token.spelling.str())) {
    std::string relation = token.spelling.str();
    advance();
    if (failed(expectKeyword("by")))
      return failure();
    SmallVector<Attribute> orders;
    do {
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field of the edges");
      if (failed(field))
        return failure();
      if (*field != "source" && *field != "target") {
        Type type = relations[relation].fieldType(*field);
        if (!type)
          return error(fieldAt, "relation '" + relation + "' has no field '" +
                                    *field + "'");
        if ((!type.isIntOrIndex() || type.isInteger(1)) && !type.isF32() &&
            !type.isF64())
          return error(fieldAt, "an index of a relation is by a whole "
                                "number or a fraction (f32, f64), or by "
                                "'source, target'; '" +
                                    *field + "' is " + typeWords(type));
        if (!orders.empty())
          return error(fieldAt, "an index of a relation is by one field of "
                                "the edges, or by 'source, target'");
      }
      orders.push_back(builder.getStringAttr(*field));
    } while (consumeIf(Token::Comma));
    auto ends = [&](StringRef end) {
      return llvm::any_of(orders, [&](Attribute a) {
        return cast<StringAttr>(a).getValue() == end;
      });
    };
    if (ends("source") != ends("target"))
      return error(at, "an index by the edges' other ends is by both: "
                       "'index " +
                           *name + " of " + relation + " by source, target'");
    if (token.isKeyword("hash"))
      return error("an index of a relation keeps each entity's edges in "
                   "order, and finds an edge to one entity by halving: "
                   "'by source, target', without 'hash'");
    auto op = module.lookupSymbol<RelationOp>(symbol(relation).getAttr());
    SmallVector<Attribute> all;
    if (auto had = op->getAttrOfType<ArrayAttr>("ordered_by"))
      llvm::append_range(all, had.getValue());
    for (Attribute order : orders)
      if (!llvm::is_contained(all, order))
        all.push_back(order);
    op->setAttr("ordered_by", builder.getArrayAttr(all));
    (void)declareSymbol(at, *name);
    return success();
  }
  SmallVector<std::string> of;
  do {
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component))
      return failure();
    if (!components.count(*component))
      return error(componentAt, "unknown component '" + *component + "'");
    if (llvm::is_contained(of, *component))
      return error(componentAt, "'" + *component + "' is named twice");
    of.push_back(*component);
  } while (consumeIf(Token::Comma));
  // `grid (x, y, z) cell size [wrap box]`: the entities by the cell of a
  // grid they are in, its key a field of the index's own; else `by` a
  // field of the first component.
  bool grid = false, morton = false;
  // (Of `morton`: bits a side, and how many a cell holds unsplit.)
  int64_t mortonBits = 0, mortonLeaf = 16;
  bool hashed = false, unique = false;
  SmallVector<std::string> positions;
  Type positionType;
  std::string cellText, wrapText;
  FailureOr<std::string> key = failure();
  Type keyType;
  // An expression as it is written: worked out where the index is made.
  auto written = [&](std::string &into) -> LogicalResult {
    const char *start = token.loc.getPointer();
    FailureOr<ExprPtr> parsed = parseExpr();
    if (failed(parsed))
      return failure();
    into = StringRef(start, previousEnd - start).str();
    return success();
  };
  // (`morton (x, y)`: a grid's places, its cells split where the
  // entities are many.)
  morton = token.isKeyword("morton");
  if (consumeKeyword("grid") || consumeKeyword("morton")) {
    grid = true;
    std::string kindWord = morton ? "morton" : "grid";
    if (failed(expect(Token::LParen, "'(' and the fields of '" + of.front() +
                                         "' that say where an entity is: '" +
                                         kindWord + " (x, y)'")))
      return failure();
    do {
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      Type type = components[of.front()].fieldType(*field);
      if (!type)
        return error(fieldAt, "component '" + of.front() +
                                  "' has no field '" + *field + "'");
      if (!type.isF32() && !type.isF64() && !type.isSignlessInteger(32) &&
          !type.isSignlessInteger(64))
        return error(fieldAt, "a grid is over places, f32, f64, i32 or "
                              "i64; '" +
                                  *field + "' is " + typeWords(type));
      if (positionType && type != positionType)
        return error(fieldAt, "a grid's places are of one type: '" + *field +
                                  "' is " + typeWords(type) + ", not " +
                                  typeWords(positionType));
      if (llvm::is_contained(positions, *field))
        return error(fieldAt, "'" + *field + "' is named twice");
      positionType = type;
      positions.push_back(*field);
    } while (consumeIf(Token::Comma));
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    if (positions.size() < 2 || positions.size() > 3)
      return error(at, std::string(morton ? "a Morton index" : "a grid") +
                           " is of two places or of three");
    if (morton) {
      // A key of 63 bits at most, so it is in order as an i64.
      int64_t most = positions.size() == 2 ? 31 : 21;
      mortonBits = most;
      bool bitsSaid = false, leafSaid = false;
      while (token.isKeyword("bits") || token.isKeyword("leaf") ||
             token.isKeyword("wrap")) {
        llvm::SMLoc optionAt = token.loc;
        std::string option = token.spelling.str();
        advance();
        if (option == "wrap") {
          if (!wrapText.empty())
            return error(optionAt, "'wrap' is said once");
          if (failed(written(wrapText)))
            return failure();
          continue;
        }
        bool &said = option == "bits" ? bitsSaid : leafSaid;
        if (said)
          return error(optionAt, "'" + option + "' is said once");
        said = true;
        llvm::SMLoc numberAt = token.loc;
        if (!token.is(Token::Integer))
          return error(numberAt, "'" + option +
                                     "' takes a whole number, "
                                     "as it is written: '" +
                                     option + " 16'");
        int64_t value = 0;
        if (token.spelling.getAsInteger(0, value))
          return error(numberAt,
                       "'" + token.spelling.str() + "' is too big a number");
        advance();
        if (option == "bits") {
          if (value < 1 || value > most)
            return error(numberAt, "a Morton index of " +
                                       Twine(positions.size()) +
                                       " places has from 1 to " + Twine(most) +
                                       " bits a side");
          mortonBits = value;
        } else {
          if (value < 1)
            return error(numberAt, "a cell holds 1 entity or more before "
                                   "it is split");
          mortonLeaf = value;
        }
      }
      if (token.isKeyword("cell"))
        return error("a Morton index's cells are split where the entities "
                     "are many: no 'cell' ('bits' says how fine they go)");
      key = "key_in_" + *name;
    } else {
      if (!consumeKeyword("cell"))
        return error("expected 'cell' and how wide a cell is at least");
      if (failed(written(cellText)))
        return failure();
      if (consumeKeyword("wrap") && failed(written(wrapText)))
        return failure();
      key = "cell_in_" + *name;
    }
    keyType = builder.getI64Type();
  } else {
    if (!consumeKeyword("by"))
      return error("expected 'by' and a field of '" + of.front() +
                   "': the whole number the entities are in order by "
                   "(or 'grid' and the fields of where they are)");
    llvm::SMLoc keyAt = token.loc;
    key = identifier("a field");
    if (failed(key))
      return failure();
    keyType = components[of.front()].fieldType(*key);
    if (!keyType)
      return error(keyAt, "component '" + of.front() + "' has no field '" +
                              *key + "'");
    auto whole = dyn_cast<IntegerType>(keyType);
    // `hash [unique]`: by equal keys (a whole number, an enum, an entity,
    // a row number or a text), not in order.
    const char *hashAt = token.loc.getPointer();
    if (consumeKeyword("hash")) {
      hashed = true;
      // (On the same line: a `unique` on the next is a declaration.)
      unique =
          token.isKeyword("unique") &&
          !StringRef(hashAt, token.loc.getPointer() - hashAt).contains('\n') &&
          consumeKeyword("unique");
      if (!(whole && whole.getWidth() > 1) &&
          !isa<EnumType, EntityType, IndexOfType, TextType, StringType>(
              keyType))
        return error(keyAt, "a hash index is by a whole number, an enum, an "
                            "entity, a row number or a text; '" +
                                *key + "' is " + typeWords(keyType));
    } else if ((!whole || whole.getWidth() == 1) && !keyType.isF32() &&
               !keyType.isF64()) {
      return error(keyAt, "an index is in order by a whole number or a "
                          "fraction (f32, f64); '" +
                              *key + "' is " + typeWords(keyType));
    }
  }

  // `clustered`: the archetype's rows themselves kept in its order.
  llvm::SMLoc clusteredAt = token.loc;
  bool clustered = consumeKeyword("clustered");
  if (clustered && hashed)
    return error(clusteredAt, "a hash index has no order to keep the rows "
                              "in: it is not 'clustered'");
  // When it is brought up to date (`lazy`, the default, or `eager`) and
  // how (`rebuild`, `incremental`, or as fits), after the key or after
  // what it copies (docs/indexes.md, section 4).
  bool eager = false;
  std::string how;
  auto ways = [&]() -> LogicalResult {
    while (token.isKeyword("lazy") || token.isKeyword("eager") ||
           token.isKeyword("rebuild") || token.isKeyword("incremental")) {
      llvm::SMLoc wayAt = token.loc;
      std::string way = token.spelling.str();
      advance();
      if (way == "lazy" || way == "eager") {
        eager = way == "eager";
        continue;
      }
      if (!how.empty() && how != way)
        return error(wayAt, "an index is made again one way: '" + how +
                                "' or '" + way + "'");
      if (grid && way == "incremental")
        return error(wayAt, std::string(morton ? "a Morton index's keys"
                                               : "a grid's cells") +
                                " are worked out for every entity each "
                                "time: it is not 'incremental'");
      how = way;
    }
    return success();
  };
  if (failed(ways()))
    return failure();

  // { name: Component.field, .. }: what each row has besides.
  struct Copy {
    std::string name, component, field;
    Type type;
  };
  SmallVector<Copy> copies;
  if (consumeIf(Token::LBrace)) {
    while (!token.is(Token::RBrace)) {
      llvm::SMLoc nameAt = token.loc;
      FailureOr<std::string> field = identifier("a field name");
      if (failed(field) || failed(expect(Token::Colon, "':'")))
        return failure();
      if (*field == "key" || *field == "who" || *field == "place")
        return error(nameAt, "every row has '" + *field + "' already");
      for (const Copy &copy : copies)
        if (copy.name == *field)
          return error(nameAt, "'" + *field + "' is named twice");
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component) ||
          failed(expect(Token::Dot, "'.' and a field: 'name: Component.field'")))
        return failure();
      if (!components.count(*component))
        return error(componentAt, "unknown component '" + *component + "'");
      llvm::SMLoc fromAt = token.loc;
      FailureOr<std::string> from = identifier("a field");
      if (failed(from))
        return failure();
      Type type = components[*component].fieldType(*from);
      if (!type)
        return error(fromAt, "component '" + *component + "' has no field '" +
                                 *from + "'");
      if (!isa<IntegerType, FloatType, EnumType, EntityType, IndexOfType>(type))
        return error(fromAt, "'" + *from + "': a row of an index holds "
                             "numbers, bools, enums, entities and row "
                             "numbers");
      if (!llvm::is_contained(of, *component))
        of.push_back(*component);
      copies.push_back({*field, *component, *from, type});
      if (!consumeIf(Token::Comma))
        break;
    }
    if (failed(expect(Token::RBrace, "'}'")))
      return failure();
  }
  if (failed(ways()))
    return failure();
  consumeIf(Token::Semicolon);
  // A grid's rows have where the entity is, under the fields' names.
  for (auto [number, field] : llvm::enumerate(positions)) {
    for (const Copy &copy : copies)
      if (copy.name == field)
        return error(at, "'" + field + "' is in every row already: where "
                         "the entity is in the grid");
    copies.insert(copies.begin() + number,
                  Copy{field, of.front(), field, positionType});
  }

  // Where each entity's row is: a field of the first component, which
  // the spawns and adds of it that were read already are given (0), as
  // those after will be; where it is among the entities as they are
  // stored (`place_in_Name`: entities of the same key are in that order,
  // however the index was made); and, of a grid, the cell it is in.
  std::string rowField = "row_in_" + *name;
  std::string placeField = "place_in_" + *name;
  Record &holder = components[of.front()];
  SmallVector<std::pair<std::string, Type>> hidden{
      {rowField, builder.getI32Type()}, {placeField, builder.getI64Type()}};
  if (grid)
    hidden.push_back({*key, keyType});
  for (auto &[field, type] : hidden)
    if (holder.fieldType(field))
      return error(at, "'" + of.front() + "' has a field '" + field +
                           "' already: the index keeps its own there");
  auto holderOp = SymbolTable::lookupSymbolIn(module, symbol(of.front()));
  auto componentOp = cast<ComponentOp>(holderOp);
  unsigned before = componentOp.getFieldNames().size();
  auto fieldsOf = [&](Attribute component) -> unsigned {
    auto op = SymbolTable::lookupSymbolIn(
        module, cast<FlatSymbolRefAttr>(component).getAttr());
    return cast<ComponentOp>(op).getFieldNames().size();
  };
  auto giveRow = [&](Operation *op, unsigned place) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(op);
    SmallVector<mlir::Value> noughts;
    for (auto &[field, type] : hidden)
      noughts.push_back(noughtOf(op->getLoc(), type));
    op->insertOperands(place, noughts);
  };
  FlatSymbolRefAttr holderName = symbol(of.front());
  module.walk([&](Operation *op) {
    if (auto add = dyn_cast<AddOp>(op)) {
      if (add.getComponentAttr() == holderName)
        giveRow(op, op->getNumOperands());
      return;
    }
    auto spawn = dyn_cast<SpawnOp>(op);
    if (!spawn)
      return;
    unsigned place = 0;
    if (std::optional<ArrayAttr> listed = spawn.getComponents()) {
      for (Attribute component : *listed) {
        if (component == holderName)
          return giveRow(op, place + before);
        place += fieldsOf(component);
      }
      return;
    }
    if (auto archetype = SymbolTable::lookupSymbolIn(
            module, spawn.getArchetypeAttr().getAttr())) {
      auto declared = cast<ArchetypeOp>(archetype);
      for (Attribute component : declared.getComponents()) {
        if (declared.isOptional(cast<FlatSymbolRefAttr>(component)))
          continue;
        if (component == holderName)
          return giveRow(op, place + before);
        place += fieldsOf(component);
      }
    }
  });
  SmallVector<Attribute> fieldNames(componentOp.getFieldNames().getValue());
  SmallVector<Attribute> fieldTypes(componentOp.getFieldTypes().getValue());
  for (auto &[field, type] : hidden) {
    fieldNames.push_back(builder.getStringAttr(field));
    fieldTypes.push_back(TypeAttr::get(type));
    holder.fields.push_back({field, type});
    holder.hidden.push_back(field);
  }
  componentOp.setFieldNamesAttr(builder.getArrayAttr(fieldNames));
  componentOp.setFieldTypesAttr(builder.getArrayAttr(fieldTypes));

  // The rows: a buffer the program keeps, which a saved world leaves out
  // (it is made again from the entities).
  Record record;
  // (A hash index by a text keeps two hashes of the text, of 64 bits each,
  // by two seeds: the entity has the text, which is not compared, so two
  // texts are taken for one where both are the same, a chance of 2^-128.)
  bool textKey = hashed && isa<TextType, StringType>(keyType);
  Type storedKey =
      textKey ? Type(builder.getIntegerType(64, /*isSigned=*/false)) : keyType;
  record.fields.push_back({"key", storedKey});
  if (textKey)
    record.fields.push_back({"check", storedKey});
  if (hashed) {
    // (A hash index's slots: a key's, with the first and the last row of
    // its list and how many it has; the entities are rows of `N_list`.)
    record.fields.push_back({"head", builder.getI32Type()});
    record.fields.push_back({"last", builder.getI32Type()});
    record.fields.push_back({"count", builder.getI32Type()});
    // (The entity, where the key has one: what `get` gives without its
    // list.)
    record.fields.push_back({"one", EntityType::get(context)});
  } else {
    record.fields.push_back({"who", EntityType::get(context)});
    record.fields.push_back({"place", builder.getI64Type()});
    for (const Copy &copy : copies)
      record.fields.push_back({copy.name, copy.type});
  }
  SmallVector<Attribute> names, types;
  for (auto &[field, type] : record.fields) {
    names.push_back(builder.getStringAttr(field));
    types.push_back(TypeAttr::get(type));
  }
  StringAttr bufferName = declareSymbol(at, *name);
  BufferOp declared = BufferOp::create(builder, loc(at), bufferName,
                                       builder.getArrayAttr(names),
                                       builder.getArrayAttr(types),
                                       builder.getI64IntegerAttr(1));
  declared->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
  buffers[*name] = std::move(record);
  // A hash index: its entities, a row each in the order they are stored,
  // with its key (a text's hashes), its place there, the rows before and
  // after it of its key, and what it copies (the room the first
  // component's capacity).
  if (hashed) {
    Record listed;
    listed.fields.push_back({"who", EntityType::get(context)});
    listed.fields.push_back({"key", storedKey});
    if (textKey)
      listed.fields.push_back({"check", storedKey});
    listed.fields.push_back({"place", builder.getI64Type()});
    listed.fields.push_back({"next", builder.getI32Type()});
    listed.fields.push_back({"prev", builder.getI32Type()});
    for (const Copy &copy : copies)
      listed.fields.push_back({copy.name, copy.type});
    SmallVector<Attribute> listNames, listTypes;
    for (auto &[field, type] : listed.fields) {
      listNames.push_back(builder.getStringAttr(field));
      listTypes.push_back(TypeAttr::get(type));
    }
    std::string listName = *name + "_list";
    BufferOp made = BufferOp::create(
        builder, loc(at), declareSymbol(at, listName),
        builder.getArrayAttr(listNames), builder.getArrayAttr(listTypes),
        builder.getI64IntegerAttr(1));
    made->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
    buffers[listName] = std::move(listed);
    // And a byte for each slot (`N_tags`): 128 where it is empty, else 7
    // bits of its key's hash; the first 16 again after the last, so that
    // 16 from any slot on are read at once (see findHashSlot).
    Record tagged;
    tagged.fields.push_back({"tag", builder.getIntegerType(8, false)});
    std::string tagsName = *name + "_tags";
    BufferOp tags = BufferOp::create(
        builder, loc(at), declareSymbol(at, tagsName),
        builder.getArrayAttr({builder.getStringAttr("tag")}),
        builder.getArrayAttr({TypeAttr::get(builder.getIntegerType(8, false))}),
        builder.getI64IntegerAttr(1));
    tags->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
    buffers[tagsName] = std::move(tagged);
  }
  // Made again incrementally: the entities whose key or what it copies
  // changed, each with those as they are now (its room the first
  // component's capacity, put in where that is known).
  // (A hash index too, but made anew every time: its rows are mended
  // where they are, as fits, from those it finds changed.)
  if (how == "incremental" || (hashed && how != "rebuild")) {
    Record changed;
    changed.fields.push_back({"who", EntityType::get(context)});
    changed.fields.push_back({"key", storedKey});
    if (textKey)
      changed.fields.push_back({"check", storedKey});
    for (const Copy &copy : copies)
      changed.fields.push_back({copy.name, copy.type});
    SmallVector<Attribute> changedNames, changedTypes;
    for (auto &[field, type] : changed.fields) {
      changedNames.push_back(builder.getStringAttr(field));
      changedTypes.push_back(TypeAttr::get(type));
    }
    std::string changedName = *name + "_changed";
    BufferOp made = BufferOp::create(
        builder, loc(at), declareSymbol(at, changedName),
        builder.getArrayAttr(changedNames), builder.getArrayAttr(changedTypes),
        builder.getI64IntegerAttr(1));
    made->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
    buffers[changedName] = std::move(changed);
  }

  // What the index declares for itself.
  std::string spelled =
      !keyType.isIntOrFloat()
          ? std::string()
          : (isa<FloatType>(keyType) ? "f"
             : isUnsigned(keyType)   ? "u"
                                     : "i") +
                std::to_string(keyType.getIntOrFloatBitWidth());
  std::string n = *name;
  std::string text;
  llvm::raw_string_ostream out(text);
  // Each copied field by the binding it is read through (`c1.x`).
  auto through = [&](const Copy &copy) {
    return "c" + std::to_string(llvm::find(of, copy.component) - of.begin()) +
           "." + copy.field;
  };
  std::string bindings;
  for (auto [number, component] : llvm::enumerate(of))
    bindings += ", c" + std::to_string(number) + ": " + component;
  SmallVector<std::string> fields{"key", "who", "place"};
  for (const Copy &copy : copies)
    fields.push_back(copy.name);
  std::string mutBindings = bindings;
  mutBindings.insert(mutBindings.find(':') + 2, "mut ");
  if (hashed) {
    // (A type as the source writes it: of an enum or a buffer, its name as
    // the module that declares the index has it.)
    auto typeSpelling = [&](Type type) -> std::string {
      auto leaf = [](StringRef symbolName) {
        return symbolName.rsplit('.').second.empty()
                   ? symbolName.str()
                   : symbolName.rsplit('.').second.str();
      };
      if (isa<EntityType>(type))
        return "entity";
      if (auto named = dyn_cast<EnumType>(type))
        return leaf(named.getName().getValue());
      if (auto row = dyn_cast<IndexOfType>(type))
        return "index of " + leaf(row.getBuffer().getValue());
      return (isUnsigned(type) ? "u" : "i") +
             std::to_string(type.getIntOrFloatBitWidth());
    };
    // A table of slots (twice the entities there may be, at least, and a
    // power of two: put in where the capacity is known), each empty
    // (`head` -1) or a key's (a text's hashes): a key goes into the slot
    // its hash says, or the next empty one after it (linear probing). The
    // entities are rows of their own (`N_list`), in the order they are
    // stored, each with its key, its place and what is copied; those of a
    // key are a list, from the slot's `head` to its `last` along `next`
    // (and back along `prev`), in the order they are stored as made.
    auto hashOf = [&](const std::string &stored) {
      return textKey ? stored : "hash_of(" + stored + ")";
    };
    auto slotOf = [&](const std::string &row) {
      return n + "_slot(order, " + row + ".key" +
             (textKey ? ", " + row + ".check" : "") + ")";
    };
    // (The other hash of a text: by another seed, written in hex.)
    std::string other = "0x9e3779b97f4a7c15";
    std::string keyType_ = typeSpelling(storedKey);
    out << "transient unique " << n << "_fresh: bool\n"
        << "transient unique " << n << "_whole: bool\n"
        << "transient unique " << n
        << "_places: i64\n"
        // (How many keys it has: what its slots are made for.)
        << "transient unique " << n
        << "_keys: i64\n"
        // (A key's home: by the hash's high bits, which runs of whole
        // numbers spread over the slots evenly, as a multiply and a shift
        // do; its low bits do not.)
        << "fn " << n << "_home(order: " << n << ", h: u64) -> i32 {\n"
        << "  (((h >> 32) * (len(order) as u64)) >> 32) as i32\n}\n"
        // (A slot's tag: 7 bits of its key's hash, others than its home's;
        // 128 where it is empty. The first 16 slots' again after the last.)
        << "fn " << n << "_tag(h: u64) -> u8 {\n"
        << "  ((h >> 32) & 127) as u8\n}\n"
        << "proc " << n << "_mark(tags: mut " << n
        << "_tags, s: i32, t: u8, slots: i32) {\n"
        << "  tags[s].tag = t\n"
        << "  if s < 16 { tags[s + slots].tag = t }\n}\n"
        // The slot of a key, or the empty one it would have.
        << "fn " << n << "_slot(order: " << n << ", key: " << keyType_
        << (textKey ? ", check: u64" : "") << ") -> i32 {\n"
        << "  var s = " << n << "_home(order, " << hashOf("key") << ")\n"
        << "  while order[s].head >= 0 && !(order[s].key == key"
        << (textKey ? " && order[s].check == check" : "") << ") {\n"
        << "    s = (s + 1) & (len(order) - 1)\n  }\n  s\n}\n"
        // An entity's row at the end of its key's list; whether no other
        // has that key.
        << "proc " << n << "_append(order: mut " << n << ", tags: mut " << n
        << "_tags, list: mut " << n << "_list, r: i32) -> bool {\n"
        << "  let s = " << slotOf("list[r]") << "\n"
        << "  let fresh = order[s].head < 0\n"
        << "  list[r].next = -1\n"
        << "  if !fresh {\n"
        << "    list[order[s].last].next = r\n"
        << "    list[r].prev = order[s].last\n"
        << "    order[s].count += 1\n"
        << "    order[s].one = none\n"
        << "  } else {\n"
        << "    order[s].key = list[r].key\n"
        << (textKey ? "    order[s].check = list[r].check\n" : "")
        << "    order[s].head = r\n"
        << "    order[s].count = 1\n"
        << "    order[s].one = list[r].who\n"
        << "    " << n << "_mark(tags, s, " << n << "_tag("
        << hashOf("list[r].key") << "), len(order))\n"
        << "    list[r].prev = -1\n"
        << "  }\n"
        << "  order[s].last = r\n  fresh\n}\n";
    // Under `unique`: where a key's list has two entities, the program
    // stops and says which.
    auto checkUnique = [&](const std::string &indent, const std::string &row) {
      if (!unique)
        return;
      out << indent << "let u = " << slotOf(row) << "\n"
          << indent << "let first_ = order[u].head\n"
          << indent << "if list[first_].next >= 0 {\n"
          << indent
          << "  index_duplicate(list[first_].key, list[first_].who, "
             "list[list[first_].next].who)\n"
          << indent << "}\n";
    };
    // Made: the slots as many as twice the keys at least (a power of two,
    // 64 at least, at most `slots`), from as many as twice those it had
    // last (`hint`), four times as many where the keys come to more than
    // half; how many keys there are.
    out << "proc " << n << "_build(order: mut " << n << ", tags: mut " << n
        << "_tags, list: mut " << n << "_list, held: mut " << of.front()
        << ", slots: i32, hint: i64) -> i64 {\n"
        << "  var keys: i64 = 0\n"
        << "  if len(list) == 0 {\n    order = []\n    tags = []\n  } else {\n"
        << "    var size = 64\n"
        << "    while (size as i64) < hint * 2 && size < slots { size *= 2 }\n"
        << "    var again = true\n"
        << "    while again {\n"
        << "      order = [{ key: list[0].key, head: -1, last: -1, count: 0, "
           "one: none"
        << (textKey ? ", check: 0" : "") << " }; size]\n"
        << "      tags = [{ tag: 128 as u8 }; size + 16]\n"
        << "      keys = 0\n"
        << "      for k in 0..len(list) {\n"
        << "        if keys * 2 <= size as i64 {\n"
        << "          if " << n
        << "_append(order, tags, list, k) { keys += 1 }\n"
        << "          held[list[k].who]." << rowField << " = k\n"
        << "        }\n"
        << "      }\n"
        << "      again = keys * 2 > size as i64\n"
        << "      if again { size = min(size * 4, slots) }\n"
        << "    }\n";
    if (unique) {
      out << "    for k in 0..len(list) {\n";
      checkUnique("      ", "list[k]");
      out << "    }\n";
    }
    out << "  }\n  keys\n}\n";
    // Mended: each changed entity's copies written into its row; where
    // its key changed, its row taken out of its key's list (and where no
    // other has that key, the slot emptied, those after it along the
    // slots that their home no longer reaches moved back into the hole),
    // then put at the end of its new key's, with its place as it was.
    if (how != "rebuild") {
      // (How many keys there are, given and said; -1 where a key more
      // could make them more than half the slots: then made anew.)
      out << "proc " << n << "_mend(order: mut " << n << ", tags: mut " << n
          << "_tags, list: mut " << n << "_list, held: mut " << of.front()
          << ", changed: " << n << "_changed, given: i64) -> i64 {\n"
          << "  var keys = given\n"
          << "  let mask = len(order) - 1\n"
          << "  for k in 0..len(changed) {\n"
          << "   if keys >= 0 && (keys + 1) * 2 > len(order) as i64 {\n"
          << "    keys = -1\n"
          << "   }\n"
          << "   if keys >= 0 {\n"
          << "    let r = held[changed[k].who]." << rowField << "\n";
      for (const Copy &copy : copies)
        out << "    list[r]." << copy.name << " = changed[k]." << copy.name
            << "\n";
      out << "    if list[r].key != changed[k].key"
          << (textKey ? " || list[r].check != changed[k].check" : "") << " {\n"
          << "      let s = " << slotOf("list[r]") << "\n"
          << "      let p = list[r].prev\n"
          << "      let q = list[r].next\n"
          << "      if p < 0 { order[s].head = q } else { list[p].next = q }\n"
          << "      if q < 0 { order[s].last = p } else { list[q].prev = p }\n"
          << "      order[s].count -= 1\n"
          << "      if order[s].count == 1 { order[s].one = "
             "list[order[s].head].who }\n"
          << "      if order[s].head < 0 {\n"
          << "        var hole = s\n"
          << "        var j = (s + 1) & mask\n"
          << "        while order[j].head >= 0 {\n"
          << "          let h = " << n << "_home(order, "
          << hashOf("order[j].key") << ")\n"
          << "          let stays = (hole <= j && hole < h && h <= j) || "
             "(hole > j && (hole < h || h <= j))\n"
          << "          if !stays {\n"
          << "            order[hole].key = order[j].key\n"
          << (textKey ? "            order[hole].check = order[j].check\n" : "")
          << "            order[hole].head = order[j].head\n"
          << "            order[hole].last = order[j].last\n"
          << "            order[hole].count = order[j].count\n"
          << "            order[hole].one = order[j].one\n"
          << "            " << n
          << "_mark(tags, hole, tags[j].tag, len(order))\n"
          << "            hole = j\n"
          << "          }\n"
          << "          j = (j + 1) & mask\n"
          << "        }\n"
          << "        order[hole].head = -1\n"
          << "        " << n << "_mark(tags, hole, 128 as u8, len(order))\n"
          << "        keys -= 1\n"
          << "      }\n"
          << "      list[r].key = changed[k].key\n"
          << (textKey ? "      list[r].check = changed[k].check\n" : "")
          << "      if " << n << "_append(order, tags, list, r) { keys += 1 }\n"
          << "    }\n   }\n  }\n";
      // (After all of them: two may have swapped their keys.)
      if (unique) {
        out << "  if keys >= 0 {\n   for k in 0..len(changed) {\n"
            << "    let r = held[changed[k].who]." << rowField << "\n";
        checkUnique("    ", "list[r]");
        out << "   }\n  }\n";
      }
      out << "  keys\n}\n";
    }
    // (`Name.get(key)` is `ent.index.get`, lowered where the slots are
    // read: see findHashSlot.)
    out << "system " << n << "_update() {\n";
    // (Its key as the index holds it, and the rest of a row.)
    std::string held = textKey
                           ? "hash_of(c0." + *key + "), check: hash_of(c0." +
                                 *key + ", " + other + ")"
                           : "c0." + *key;
    std::string rest;
    for (const Copy &copy : copies)
      rest += ", " + copy.name + ": " + through(copy);
    // Where the same entities are in it, mended: those a reactive `for`
    // visits (`incremental`), or whose key or copies differ from their
    // row's (as fits).
    if (how != "rebuild") {
      out << "  if " << n << "_whole {\n"
          << "    " << n << "_changed = []\n"
          << "    for e" << bindings;
      if (how == "incremental") {
        out << " on changed c0." << *key;
        for (const Copy &copy : copies)
          out << ", changed " << through(copy);
      } else {
        std::string row = n + "_list[c0." + rowField + "]";
        out << " where " << row << ".key != "
            << (textKey ? "hash_of(c0." + *key + ")" : "c0." + *key);
        if (textKey)
          out << " || " << row << ".check != hash_of(c0." << *key << ", "
              << other << ")";
        for (const Copy &copy : copies)
          out << " || " << row << "." << copy.name << " != " << through(copy);
      }
      out << " {\n      " << n << "_changed += { who: e, key: " << held << rest
          << " }\n    }\n"
          << "    " << n << "_keys = " << n << "_mend(" << n << ", " << n
          << "_tags, " << n << "_list, " << of.front() << ", " << n
          << "_changed, " << n << "_keys)\n"
          << "    if " << n << "_keys < 0 { " << n << "_whole = false }\n"
          << "  }\n"
          << "  if !" << n << "_whole {\n";
    }
    out << "  " << n << "_places = 0\n"
        << "  " << n << "_list = []\n"
        << "  for e" << mutBindings << " {\n"
        << "    let place_ = " << n
        << "_places += 1\n"
        // (Where it is stored: what a `for` it answers goes by.)
        << "    c0." << placeField << " = place_\n"
        << "    " << n << "_list += { who: e, key: " << held
        << ", place: place_, next: -1, prev: -1" << rest << " }\n  }\n"
        << "  " << n << "_keys = " << n << "_build(" << n << ", " << n
        << "_tags, " << n << "_list, " << of.front() << ", "
        << kSlotsPlaceholder << ", " << n << "_keys)\n"
        << "  " << n << "_whole = true\n";
    if (how != "rebuild")
      out << "  }\n";
    out << "  " << n << "_fresh = true\n}\n"
        << "system " << n << "_stale() { " << n << "_fresh = false }\n"
        << "system " << n << "_moved() {\n  " << n << "_fresh = false\n  " << n
        << "_whole = false\n}\n";
  } else {
    out << "transient unique " << n << "_fresh: bool\n"
        << "transient unique " << n << "_places: i64\n"
        << "transient unique " << n << "_skip: i64\n"
        << "transient unique " << n << "_wait: i64\n"
        << "transient unique " << n
        << "_whole: bool\n"
        // Every entity told its row.
        << "proc " << n << "_number(order: " << n << ", held: mut "
        << of.front() << ") {\n"
        << "  for row in 0..len(order) { held[order[row].who]." << rowField
        << " = row }\n}\n"
        // How many rows are below the one before them; and whether that is
        // more than one in 64 of them, so many that they are better put in
        // order anew (64 where nothing else says: `tune`, ent-lower-to-
        // loops, says another of a trace, which also sees this call).
        << "fn " << n << "_below(order: " << n << ") -> i32 {\n"
        << "  var below = 0\n"
        << "  for i in 1..len(order) {\n"
        // (Its place read only where the keys do not say: seldom.)
        << "    if order[i].key <= order[i - 1].key {\n"
        << "      if order[i].key < order[i - 1].key || order[i].place < "
           "order[i - 1].place { below += 1 }\n    }\n"
        << "  }\n  below\n}\n"
        << "fn " << n << "_tangled(below: i32, rows: i32) -> bool {\n"
        << "  (below as i64) * " << kTangledPlaceholder
        << " > (rows as i64)\n}\n"
        // The rows in order again, where few are out of it: each that is
        // below the one before (by key, then by place) goes down past those
        // above it (insertion), and the entities of the rows that moved are
        // told their rows. Where that would move them more steps than
        // there are rows, it stops and says so: they are better put in
        // order anew (a sort is some passes over the rows; rows moved far,
        // keys at random, would take as many steps as rows for each). What
        // it moved by then is made anew as well.
        << "proc " << n << "_settle(order: mut " << n << ", held: mut "
        << of.front() << ") -> bool {\n"
        << "  var steps = 0\n"
        << "  var i = 1\n"
        << "  while i < len(order) {\n"
        << "    if order[i].key <= order[i - 1].key && (order[i].key < "
           "order[i - 1].key || order[i].place < order[i - 1].place) {\n";
    for (const std::string &field : fields)
      out << "      let " << field << "_ = order[i]." << field << "\n";
    out << "      var j = i\n"
        << "      while j > 0 && steps <= len(order) && (order[j - 1].key > "
           "key_ || (order[j - 1].key == key_ && order[j - 1].place > "
           "place_)) {\n"
        << "        steps += 1\n";
    for (const std::string &field : fields)
      out << "        order[j]." << field << " = order[j - 1]." << field
          << "\n";
    out << "        held[order[j].who]." << rowField << " = j\n"
        << "        j -= 1\n      }\n";
    for (const std::string &field : fields)
      out << "      order[j]." << field << " = " << field << "_\n";
    out << "      held[who_]." << rowField << " = j\n"
        << "      if steps > len(order) { i = len(order) }\n"
        << "    }\n    i += 1\n  }\n"
        << "  steps <= len(order)\n}\n"
        // Made again: where the same entities are in it, each writes its
        // key and what is copied into its own row, and the rows are put in
        // order again; else all are put in order by their keys.
        ;
    // Of one made again incrementally: the entities whose key or what it
    // copies changed (the room is the first component's capacity, put in
    // where that is known), and what moves each one's row up or down to its
    // place, the others in order as they are. Where that would move rows
    // more steps than there are rows, it stops and says so.
    if (how == "incremental") {
      // (Each with its key and what is copied as they are now: a buffer of
      // its own, declared as the index's rows are, below.)
      out << "proc " << n << "_mend(order: mut " << n << ", held: mut "
          << of.front() << ", changed: " << n << "_changed) -> bool {\n"
          << "  var steps = 0\n"
          << "  for k in 0..len(changed) {\n"
          << "    if steps <= len(order) {\n"
          << "      let i = held[changed[k].who]." << rowField << "\n"
          << "      order[i].key = changed[k].key\n";
      for (const Copy &copy : copies)
        out << "      order[i]." << copy.name << " = changed[k]." << copy.name
            << "\n";
      for (const std::string &field : fields)
        out << "      let " << field << "_ = order[i]." << field << "\n";
      out << "      var j = i\n";
      for (int down = 0; down < 2; ++down) {
        if (!down)
          out << "      while j > 0 && steps <= len(order) && (order[j - "
                 "1].key "
                 "> key_ || (order[j - 1].key == key_ && order[j - 1].place > "
                 "place_)) {\n";
        else
          out << "      while j + 1 < len(order) && steps <= len(order) && "
                 "(order[j + 1].key < key_ || (order[j + 1].key == key_ && "
                 "order[j + 1].place < place_)) {\n";
        std::string next = down ? "j + 1" : "j - 1";
        out << "        steps += 1\n";
        for (const std::string &field : fields)
          out << "        order[j]." << field << " = order[" << next << "]."
              << field << "\n";
        out << "        held[order[j].who]." << rowField << " = j\n"
            << "        j " << (down ? "+=" : "-=") << " 1\n      }\n";
      }
      for (const std::string &field : fields)
        out << "      order[j]." << field << " = " << field << "_\n";
      out << "      held[who_]." << rowField << " = j\n"
          << "    }\n  }\n"
          << "  steps <= len(order)\n}\n";
    }
    // Of a grid: how many cells it has on each side, how wide they are and
    // where they start (`N_grid`); each cell's first row (`N_cells`, a row
    // more at the end); and what finds a place's cell and the cells around.
    std::string ts = !positionType                ? "f64"
                     : positionType.isF32()       ? "f32"
                     : positionType.isF64()       ? "f64"
                     : positionType.isInteger(32) ? "i32"
                                                  : "i64";
    // (A grid of whole numbers: its cells as wide as whole numbers, where a
    // place is in one and how far things are worked out to the unit, in
    // i64.)
    bool whole = ts[0] == 'i';
    unsigned dims = positions.size();
    bool wrap = !wrapText.empty();
    auto side = [&](const char *what, unsigned d) {
      return std::string(what) + std::to_string(d);
    };
    if (grid && !morton) {
      out << "transient unique " << n << "_grid { cells: i64";
      for (unsigned d = 0; d < dims; ++d)
        out << ", " << side("n", d) << ": i64, " << side("w", d) << ": " << ts
            << ", " << side("lo", d) << ": " << ts << ", " << side("hi", d)
            << ": " << ts;
      out << ", size: " << ts << " }\n"
          << "transient buffer " << n
          << "_cells { first: i32 } capacity 1\n"
          // The cell of a place on one side.
          << "fn " << n << "_cell(x: " << ts << ", lo: " << ts << ", w: " << ts
          << ", n: i64) -> i64 {\n";
      if (whole)
        out << "  let q = (x as i64) - (lo as i64)\n"
            << "  var c = q / (w as i64)\n"
            << "  if c * (w as i64) > q { c -= 1 }\n";
      else
        out << "  let q = (x - lo) / w\n"
            << "  var c = q as i64\n"
            << "  if (c as " << ts << ") > q { c -= 1 }\n";
      if (wrap)
        out << "  c = c % n\n"
            << "  if c < 0 { c += n }\n";
      else
        out << "  if c < 0 { c = 0 }\n"
            << "  if c >= n { c = n - 1 }\n";
      out << "  c\n}\n"
          // How many cells out from a place's own are within `r`.
          << "fn " << n << "_reach(r: " << ts << ", w: " << ts << ") -> i64 {\n"
          << (whole ? "  var m = (r as i64) / (w as i64)\n"
                      "  if m * (w as i64) < (r as i64) { m += 1 }\n"
                    : "  var m = (r / w) as i64\n"
                      "  if (m as " +
                          ts + ") * w < r { m += 1 }\n")
          << "  m\n}\n"
          // The first of the cells around cell `c`, and how many.
          << "fn " << n << "_from(c: i64, m: i64, n: i64) -> i64 {\n";
      if (wrap)
        out << "  if 2 * m + 1 >= n { 0 } else { c - m }\n}\n";
      else
        out << "  max(0, c - m)\n}\n";
      out << "fn " << n << "_many(c: i64, m: i64, n: i64) -> i64 {\n";
      if (wrap)
        out << "  min(2 * m + 1, n)\n}\n";
      else
        out << "  min(n - 1, c + m) - max(0, c - m) + 1\n}\n";
      // A little of a cell's width, for where a place found its cell by
      // rounding.
      std::string slack = ts == "f32" ? "0.01" : "0.000000001";
      // A cell counted from the first, round the box (from `_from`, never
      // a box or more below the first cell or past the last).
      out << "fn " << n << "_at(k: i64, n: i64) -> i64 {\n"
          << "  if k < 0 { k + n } else if k >= n { k - n } else { k }\n}\n"
          // The lowest and the highest cell, counted on from the first
          // (not round the box), that a place's `r` either side reaches.
          ;
      for (const char *end : {"low", "high"}) {
        const char *sign = end[0] == 'l' ? "-" : "+";
        out << "fn " << n << "_" << end << "(p: " << ts << ", r: " << ts
            << ", lo: " << ts << ", w: " << ts << ") -> i64 {\n";
        if (whole)
          out << "  let q = (p as i64) " << sign
              << " (r as i64) - (lo as i64)\n"
              << "  var c = q / (w as i64)\n"
              << "  if c * (w as i64) > q { c -= 1 }\n";
        else
          out << "  let q = (p " << sign << " r " << sign << " w * " << slack
              << " - lo) / w\n"
              << "  var c = q as i64\n"
              << "  if (c as " << ts << ") > q { c -= 1 }\n";
        out << "  c\n}\n";
      }
      out
          // The cells from `l` to `h` of those `_from` and `_many` give, in
          // the order they are visited, as two runs of cells next to each
          // other (where they go round the box): `part` 0 and 1 the first
          // and last of the first run, 2 and 3 of the second; a run with no
          // cells is from 1 to 0.
          << "fn " << n << "_run(part: i64, l: i64, h: i64, from: i64, "
          << "many: i64, n: i64) -> i64 {\n"
          << "  var a1: i64 = 1\n  var b1: i64 = 0\n"
          << "  var a2: i64 = 1\n  var b2: i64 = 0\n";
      if (wrap)
        // (All the cells, from the first: where they are no more than a
        // box, those of `l` to `h` round it.)
        out << "  if many == n {\n"
            << "    if h - l + 1 >= n {\n"
            << "      a1 = 0\n      b1 = n - 1\n"
            << "    } else {\n"
            << "      let lw = " << n << "_at(l - n * (l / n), n)\n"
            << "      let hw = " << n << "_at(h - n * (h / n), n)\n"
            << "      if lw <= hw {\n        a1 = lw\n        b1 = hw\n"
            << "      } else {\n        a1 = 0\n        b1 = hw\n"
            << "        a2 = lw\n        b2 = n - 1\n      }\n"
            << "    }\n"
            << "  } else {\n"
            << "    let s = max(l, from)\n"
            << "    let e = min(h, from + many - 1)\n"
            << "    if s <= e {\n"
            << "      if e < 0 {\n        a1 = s + n\n        b1 = e + n\n"
            << "      } else if s < 0 {\n        a1 = s + n\n        b1 = n - "
               "1\n"
            << "        a2 = 0\n        b2 = e\n"
            << "      } else if s >= n {\n"
            << "        a1 = s - n\n        b1 = e - n\n"
            << "      } else if e >= n {\n        a1 = s\n        b1 = n - 1\n"
            << "        a2 = 0\n        b2 = e - n\n"
            << "      } else {\n        a1 = s\n        b1 = e\n      }\n"
            << "    }\n"
            << "  }\n";
      else
        out << "  let s = max(l, from)\n"
            << "  let e = min(h, from + many - 1)\n"
            << "  if s <= e {\n    a1 = s\n    b1 = e\n  }\n";
      out << "  if part == 0 { a1 } else if part == 1 { b1 } else if part == 2 "
             "{ a2 } else { b2 }\n}\n"
          // Where `x` is seen from `p`: the image nearest it.
          << "fn " << n << "_image(x: " << ts << ", p: " << ts
          << ", size: " << ts << ") -> " << ts << " {\n";
      if (wrap)
        // (Moved by the size in each branch: the same, to the bit, as
        // taking away a shift of the size or of its opposite.)
        out << "  let d = x - p\n"
            << (whole ? "  if d + d > size { x - size } else if d + d < 0 - "
                        "size { x + size } else { x }\n}\n"
                      : "  if d > size * 0.5 { x - size } else if d < 0.0 - "
                        "size * 0.5 { x + size } else { x }\n}\n");
      else
        out << "  x\n}\n";
      // How far, at least, an entity in cell `c` is from `p` on one side
      // (as `_image` sees it, where the box wraps): from `p` to the cell,
      // less a little for where a place found its cell by rounding. No
      // nearer than that, so a cell farther than a distance has no entity
      // within it.
      out << "fn " << n << "_out(a: " << ts << ", w: " << ts << ", p: " << ts
          << ") -> " << ts << " {\n"
          << "  if p < a { a - p } else if p > a + w { p - (a + w) } else "
             "{ "
          << (whole ? "0" : "0.0") << " }\n}\n"
          << "fn " << n << "_gap(c: i64, p: " << ts << ", lo: " << ts
          << ", w: " << ts << ", size: " << ts << ") -> " << ts << " {\n"
          << "  let a = lo + (c as " << ts << ") * w\n"
          << "  var g = " << n << "_out(a, w, p)\n";
      if (wrap)
        out << "  let below = " << n << "_out(a - size, w, p)\n"
            << "  if below < g { g = below }\n"
            << "  let above = " << n << "_out(a + size, w, p)\n"
            << "  if above < g { g = above }\n";
      if (whole)
        out << "  g\n}\n"
            // The whole number whose square is at most `v`, the most.
            << "fn " << n << "_root(v: i64) -> i64 {\n"
            << "  var s = sqrt(v as f64) as i64\n"
            << "  while s * s > v { s -= 1 }\n"
            << "  while (s + 1) * (s + 1) <= v { s += 1 }\n"
            << "  s\n}\n";
      else
        out << "  g -= w * " << slack << "\n"
            << "  if g < 0.0 { 0.0 } else { g }\n}\n";
      // Each cell's first row: the first whose cell is not below it.
      out << "proc " << n << "_bounds(order: " << n << ", starts: mut " << n
          << "_cells, cells: i64) {\n"
          << "  starts = [{ first: 0 }; (cells + 1) as i32]\n"
          << "  var r = 0\n"
          << "  for c in 0..cells + 1 {\n"
          << "    while r < len(order) && order[r].key < c { r += 1 }\n"
          << "    starts[c as i32].first = r\n"
          << "  }\n}\n";
    }
    // Of a Morton index: its box (`N_box`: where it starts on each side and
    // ends, how wide its widest side is, the steps a unit of that, and of
    // a box that wraps its size); and its cells (`N_cells`), each a run of
    // rows whose keys share their first `level` digits, those inside a cell
    // side by side (from `child`, `kids` of them; a leaf's `child` is -1),
    // each with the cell after it in preorder that is not inside it
    // (`next`) and the box its entities are in. `N_tree` lays them out.
    if (morton) {
      std::string most = ts == "i32"   ? "2147483647"
                         : ts == "i64" ? "9223372036854775807"
                                       : "1.0 / 0.0";
      std::string least =
          whole ? "0 - " + most + " - 1" : std::string("0.0 - 1.0 / 0.0");
      int64_t children = int64_t(1) << dims;
      int64_t top = (int64_t(1) << mortonBits) - 1;
      out << "transient unique " << n << "_box { side: f64, scale: f64, "
          << "size: " << ts;
      for (unsigned d = 0; d < dims; ++d)
        out << ", " << side("lo", d) << ": " << ts << ", " << side("hi", d)
            << ": " << ts;
      out << " }\n"
          << "transient buffer " << n << "_cells { first: i32, count: i32, "
          << "level: i32, child: i32, kids: i32, next: i32";
      for (unsigned d = 0; d < dims; ++d)
        out << ", " << side("lo", d) << ": " << ts << ", " << side("hi", d)
            << ": " << ts;
      out << " } capacity 1\n"
          // For each row, the leaf whose first row it is (`count` 1; else
          // 0): which cell an entity works out the box of.
          << "transient buffer " << n
          << "_lead { cell: i32, count: i32 } capacity 1\n"
          // A side's steps, their bits spread `dims` apart.
          << "fn " << n << "_spread(q: u64) -> u64 {\n";
      if (dims == 3)
        out << "  var x = q & 0x1fffff\n"
            << "  x = (x | (x << 32)) & 0x1f00000000ffff\n"
            << "  x = (x | (x << 16)) & 0x1f0000ff0000ff\n"
            << "  x = (x | (x << 8)) & 0x100f00f00f00f00f\n"
            << "  x = (x | (x << 4)) & 0x10c30c30c30c30c3\n"
            << "  x = (x | (x << 2)) & 0x1249249249249249\n";
      else
        out << "  var x = q & 0xffffffff\n"
            << "  x = (x | (x << 16)) & 0x0000ffff0000ffff\n"
            << "  x = (x | (x << 8)) & 0x00ff00ff00ff00ff\n"
            << "  x = (x | (x << 4)) & 0x0f0f0f0f0f0f0f0f\n"
            << "  x = (x | (x << 2)) & 0x3333333333333333\n"
            << "  x = (x | (x << 1)) & 0x5555555555555555\n";
      out << "  x\n}\n"
          // The step of a place on one side, within the box.
          << "fn " << n << "_step(x: " << ts << ", lo: " << ts
          << ", scale: f64) -> u64 {\n"
          << "  var q = (((x as f64) - (lo as f64)) * scale) as i64\n"
          << "  if q < 0 { q = 0 }\n"
          << "  if q > " << top << " { q = " << top << " }\n"
          << "  q as u64\n}\n"
          // The key of a place: its steps on each side, interleaved.
          << "fn " << n << "_key(";
      for (unsigned d = 0; d < dims; ++d)
        out << (d ? ", " : "") << "p" << d << ": " << ts;
      for (unsigned d = 0; d < dims; ++d)
        out << ", " << side("lo", d) << ": " << ts;
      out << ", scale: f64) -> i64 {\n  (";
      for (unsigned d = 0; d < dims; ++d) {
        std::string spread = n + "_spread(" + n + "_step(p" +
                             std::to_string(d) + ", " + side("lo", d) +
                             ", scale))";
        out << (d ? " | (" + spread + " << " + std::to_string(d) + ")"
                  : spread);
      }
      out << ") as i64\n}\n";
      out
          // Where `x` is seen from `p`: the image nearest it.
          << "fn " << n << "_image(x: " << ts << ", p: " << ts
          << ", size: " << ts << ") -> " << ts << " {\n";
      if (wrap)
        out << "  let d = x - p\n"
            << (whole ? "  if d + d > size { x - size } else if d + d < 0 - "
                        "size { x + size } else { x }\n}\n"
                      : "  if d > size * 0.5 { x - size } else if d < 0.0 - "
                        "size * 0.5 { x + size } else { x }\n}\n");
      else
        out << "  x\n}\n";
      // The cells: the root, then those inside each cell (a cell of more
      // rows than `leaf` is split, unless it is at the finest level), in
      // the order of their digits, after all the cells before them; each
      // cell's `next`, from the root down; and each leaf at its first row
      // in `lead`. The leaves' boxes are then worked out by the entities
      // (in the update, on many threads), and the others' (`N_up`) from
      // the last cell back.
      std::string none = whole ? "0" : "0.0";
      std::string boxes;
      for (unsigned d = 0; d < dims; ++d)
        boxes += ", " + side("lo", d) + ": " + none + ", " + side("hi", d) +
                 ": " + none;
      out << "proc " << n << "_tree(order: " << n << ", cells: mut " << n
          << "_cells, lead: mut " << n << "_lead) {\n"
          << "  cells = []\n"
          << "  if len(order) > 0 { cells += { first: 0, count: len(order), "
             "level: 0, child: 0 - 1, kids: 0, next: 1"
          << boxes << " } }\n"
          << "  var i = 0\n"
          << "  while i < len(cells) {\n"
          << "    let first = cells[i].first\n"
          << "    let count = cells[i].count\n"
          << "    let level = cells[i].level\n"
          << "    if count > " << mortonLeaf << " && level < " << mortonBits
          << " {\n"
          << "      let shift = ((" << mortonBits << " - level - 1) * " << dims
          << ") as i64\n"
          << "      let child = len(cells)\n"
          << "      var start = first\n"
          << "      let end = first + count\n"
          << "      for digit in 0.." << children << " {\n"
          << "        var low = start\n"
          << "        var high = end\n"
          << "        while low < high {\n"
          << "          let mid = (low + high) >> 1\n"
          << "          if ((order[mid].key >> shift) & " << children - 1
          << ") <= digit as i64 { low = mid + 1 } else { high = mid }\n"
          << "        }\n"
          << "        if low > start { cells += { first: start, count: low - "
             "start, level: level + 1, child: 0 - 1, kids: 0, next: 0"
          << boxes << " } }\n"
          << "        start = low\n"
          << "      }\n"
          << "      cells[i].child = child\n"
          << "      cells[i].kids = len(cells) - child\n"
          << "    }\n"
          << "    i += 1\n"
          << "  }\n"
          << "  if len(cells) > 0 { cells[0].next = len(cells) }\n"
          << "  for p in 0..len(cells) {\n"
          << "    let child = cells[p].child\n"
          << "    if child >= 0 {\n"
          << "      let last = child + cells[p].kids - 1\n"
          << "      for c in child..last { cells[c].next = c + 1 }\n"
          << "      cells[last].next = cells[p].next\n"
          << "    }\n"
          << "  }\n"
          << "  lead = [{ cell: 0, count: 0 }; len(order)]\n"
          << "  for c in 0..len(cells) {\n"
          << "    if cells[c].child < 0 {\n"
          << "      lead[cells[c].first].cell = c\n"
          << "      lead[cells[c].first].count = 1\n"
          << "    }\n"
          << "  }\n}\n"
          << "proc " << n << "_up(cells: mut " << n << "_cells) {\n"
          << "  var c = len(cells) - 1\n"
          << "  while c >= 0 {\n"
          << "    let child = cells[c].child\n"
          << "    if child >= 0 {\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "      var " << side("lo", d) << ": " << ts << " = " << most
            << "\n      var " << side("hi", d) << ": " << ts << " = " << least
            << "\n";
      out << "      for k in child..child + cells[c].kids {\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "        " << side("lo", d) << " = min(" << side("lo", d)
            << ", cells[k]." << side("lo", d) << ")\n        " << side("hi", d)
            << " = max(" << side("hi", d) << ", cells[k]." << side("hi", d)
            << ")\n";
      out << "      }\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "      cells[c]." << side("lo", d) << " = " << side("lo", d)
            << "\n      cells[c]." << side("hi", d) << " = " << side("hi", d)
            << "\n";
      out << "    }\n"
          << "    c -= 1\n"
          << "  }\n}\n";
    }
    out << "system " << n << "_update() {\n";
    if (grid && !morton) {
      // The grid as the entities are now: its cells at least `cell` wide,
      // round the box where it wraps (`size` on every side), else over
      // where the entities are; no more cells than entities can be (the
      // number is the first component's capacity, put in where the index's
      // capacity is known).
      // (Of the places' type: a number written out takes it.)
      out << "  var cell_: " << ts << " = " << cellText << "\n";
      if (wrap) {
        out << "  var size_: " << ts << " = " << wrapText << "\n"
            << "  " << n << "_grid.size = size_\n";
        for (unsigned d = 0; d < dims; ++d)
          out << "  " << n << "_grid." << side("lo", d) << " = "
              << (whole ? "0" : "0.0") << "\n"
              << "  " << n << "_grid." << side("hi", d) << " = size_\n"
              << "  var " << side("n", d)
              << " = max(1, (size_ / cell_) as i64)\n";
      } else {
        // (Of whole numbers, the most and the least there are.)
        std::string most = ts == "i32" ? "2147483647" : "9223372036854775807";
        for (unsigned d = 0; d < dims; ++d)
          out << "  " << n << "_grid." << side("lo", d) << " = "
              << (whole ? most : "1.0 / 0.0") << "\n"
              << "  " << n << "_grid." << side("hi", d) << " = "
              << (whole ? "0 - " + most + " - 1" : "0.0 - 1.0 / 0.0") << "\n";
        out << "  for c0: " << of.front() << " {\n";
        for (unsigned d = 0; d < dims; ++d)
          out << "    " << n << "_grid." << side("lo", d) << " min= c0."
              << positions[d] << "\n"
              << "    " << n << "_grid." << side("hi", d) << " max= c0."
              << positions[d] << "\n";
        out << "  }\n";
        for (unsigned d = 0; d < dims; ++d)
          if (whole)
            out << "  var " << side("n", d) << " = max(1, ((" << n << "_grid."
                << side("hi", d) << " as i64) - (" << n << "_grid."
                << side("lo", d) << " as i64)) / (cell_ as i64) + 1)\n";
          else
            out << "  var " << side("n", d) << " = max(1, ((" << n << "_grid."
                << side("hi", d) << " - " << n << "_grid." << side("lo", d)
                << ") / cell_) as i64 + 1)\n";
      }
      std::string product = "n0";
      for (unsigned d = 1; d < dims; ++d)
        product += " * " + side("n", d);
      out << "  while " << product << " > " << kCellsPlaceholder << " {\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "    " << side("n", d) << " = max(1, " << side("n", d)
            << " / 2)\n";
      out << "  }\n";
      // (Where a grid of whole numbers wraps, its cells are all as wide:
      // as many as the size can be cut into so.)
      if (whole && wrap)
        for (unsigned d = 0; d < dims; ++d)
          out << "  while (size_ as i64) % " << side("n", d) << " != 0 { "
              << side("n", d) << " -= 1 }\n";
      out << "  " << n << "_grid.cells = " << product << "\n";
      for (unsigned d = 0; d < dims; ++d) {
        out << "  " << n << "_grid." << side("n", d) << " = " << side("n", d)
            << "\n  " << n << "_grid." << side("w", d) << " = ";
        if (wrap)
          out << "size_ / (" << side("n", d) << " as " << ts << ")\n";
        else if (whole)
          // (One more than the floor: the cells are over every entity.)
          out << "max(cell_, ((((" << n << "_grid." << side("hi", d)
              << " as i64) - (" << n << "_grid." << side("lo", d)
              << " as i64)) / " << side("n", d) << " + 1) as " << ts << "))\n";
        else
          out << "max(cell_, (" << n << "_grid." << side("hi", d) << " - " << n
              << "_grid." << side("lo", d) << ") / (" << side("n", d) << " as "
              << ts << "))\n";
        out << "  let " << side("w_", d) << " = " << n << "_grid."
            << side("w", d) << "\n  let " << side("lo_", d) << " = " << n
            << "_grid." << side("lo", d) << "\n";
      }
      // Each entity's cell: x fastest, then y, then z.
      std::string number;
      for (unsigned d = dims; d-- > 0;) {
        std::string cell = n + "_cell(c0." + positions[d] + ", " +
                           side("lo_", d) + ", " + side("w_", d) + ", " +
                           side("n", d) + ")";
        number = number.empty()
                     ? cell
                     : "(" + number + ") * " + side("n", d) + " + " + cell;
      }
      out << "  for c0: mut " << of.front() << " {\n    c0." << *key << " = "
          << number << "\n  }\n";
    }
    if (morton) {
      // The box as the entities are now: the box that wraps, else from the
      // lowest of them on each side, as wide as they are on the widest (so
      // its cells are cubes); each entity's key its steps on each side,
      // interleaved, x the lowest bit of each digit.
      if (wrap) {
        out << "  var size_: " << ts << " = " << wrapText << "\n"
            << "  " << n << "_box.size = size_\n";
        for (unsigned d = 0; d < dims; ++d)
          out << "  " << n << "_box." << side("lo", d) << " = "
              << (whole ? "0" : "0.0") << "\n"
              << "  " << n << "_box." << side("hi", d) << " = size_\n";
        out << "  var side_ = size_ as f64\n";
      } else {
        std::string most = ts == "i32"   ? "2147483647"
                           : ts == "i64" ? "9223372036854775807"
                                         : "1.0 / 0.0";
        for (unsigned d = 0; d < dims; ++d)
          out << "  " << n << "_box." << side("lo", d) << " = " << most
              << "\n  " << n << "_box." << side("hi", d) << " = "
              << (whole ? "0 - " + most + " - 1" : "0.0 - 1.0 / 0.0") << "\n";
        out << "  for c0: " << of.front() << " {\n";
        for (unsigned d = 0; d < dims; ++d)
          out << "    " << n << "_box." << side("lo", d) << " min= c0."
              << positions[d] << "\n"
              << "    " << n << "_box." << side("hi", d) << " max= c0."
              << positions[d] << "\n";
        out << "  }\n  var side_: f64 = 0.0\n";
        for (unsigned d = 0; d < dims; ++d)
          out << "  side_ = max(side_, (" << n << "_box." << side("hi", d)
              << " as f64) - (" << n << "_box." << side("lo", d)
              << " as f64))\n";
      }
      out << "  if !(side_ > 0.0) { side_ = 1.0 }\n"
          << "  " << n << "_box.side = side_\n"
          << "  let scale_ = " << (int64_t(1) << mortonBits) << ".0 / side_\n"
          << "  " << n << "_box.scale = scale_\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "  let " << side("lo_", d) << " = " << n << "_box."
            << side("lo", d) << "\n";
      out << "  for c0: mut " << of.front() << " {\n    c0." << *key << " = "
          << n << "_key(";
      for (unsigned d = 0; d < dims; ++d)
        out << (d ? ", " : "") << "c0." << positions[d];
      for (unsigned d = 0; d < dims; ++d)
        out << ", " << side("lo_", d);
      out << ", scale_)\n  }\n";
    }
    // Made anew by `by`: every entity in order by its key, those of a key
    // in the order they are stored (the order insertion by their places
    // gives).
    // Where each entity is as they are stored, counted first where it may
    // have moved there (entities came, went or moved: no other change moves
    // one, so made again in place the places are as they were, in the rows
    // too, which keep theirs as they move).
    auto anew = [&](const std::string &indent, bool counted) {
      if (counted)
        out << indent << n << "_places = 0\n"
            << indent << "for c0: mut " << of.front() << " {\n"
            << indent << "  let place_ = " << n << "_places += 1\n"
            << indent << "  c0." << placeField << " = place_\n"
            << indent << "}\n";
      out << indent << n << " = []\n"
          << indent << "for e" << bindings << " by c0." << *key << " {\n"
          << indent << "  " << n << " += { key: c0." << *key << ", who: e, "
          << "place: c0." << placeField;
      for (const Copy &copy : copies)
        out << ", " << copy.name << ": " << through(copy);
      out << " }\n"
          << indent << "}\n"
          << indent << n << "_number(" << n << ", " << of.front() << ")\n";
    };
    if (how == "rebuild") {
      // (Every time anew.)
      anew("  ", /*counted=*/true);
      out << "  " << n << "_whole = true\n";
    } else if (how == "incremental") {
      // (Where the same entities are in it, only those whose key or what it
      // copies changed, which a reactive `for` visits; their rows moved
      // to their places.)
      out << "  if " << n << "_whole {\n"
          << "    " << n << "_changed = []\n"
          << "    for e" << bindings << " on changed c0." << *key;
      for (const Copy &copy : copies)
        out << ", changed " << through(copy);
      out << " {\n      " << n << "_changed += { who: e, key: c0." << *key;
      for (const Copy &copy : copies)
        out << ", " << copy.name << ": " << through(copy);
      out << " }\n    }\n"
          << "    if !" << n << "_mend(" << n << ", " << of.front() << ", " << n
          << "_changed) {\n";
      anew("      ", /*counted=*/false);
      out << "    }\n  } else {\n";
      anew("    ", /*counted=*/true);
      out << "    " << n << "_whole = true\n  }\n";
    } else {
      out << "  if " << n << "_whole {\n";
      // (As fits, not a grid: where trying in place came to too many steps,
      // anew the next so many times without trying, twice as many after
      // each try that does, up to 64; where it did not, it tries every time.
      // Keys that change much each time are then sorted anew without first
      // writing every row and moving them each time.)
      if (!grid) {
        out << "  if " << n << "_skip > 0 {\n    " << n << "_skip -= 1\n";
        anew("    ", /*counted=*/false);
        out << "  } else {\n";
      }
      out << "    for e" << bindings << ", w: mut " << n << "[c0." << rowField
          << ", 1] {\n      w[0].key = c0." << *key << "\n";
      for (const Copy &copy : copies)
        out << "      w[0]." << copy.name << " = " << through(copy) << "\n";
      out << "    }\n";
      // (Where many rows are out of order, sorted on many threads; else each
      // goes down past those above it. A grid counts them first, which a
      // trace sees (and --tune chooses from): `Name_tangled`.)
      if (grid) {
        out << "    if " << n << "_tangled(" << n << "_below(" << n << "), len("
            << n << ")) {\n";
        anew("      ", /*counted=*/false);
        out << "    } else if !" << n << "_settle(" << n << ", " << of.front()
            << ") {\n";
        anew("      ", /*counted=*/false);
        out << "    }\n";
      } else {
        out << "    if !" << n << "_settle(" << n << ", " << of.front()
            << ") {\n";
        anew("      ", /*counted=*/false);
        out << "      " << n << "_wait = min(max(" << n
            << "_wait * 2, 1), 64)\n"
            << "      " << n << "_skip = " << n << "_wait\n"
            << "    } else {\n      " << n << "_wait = 0\n    }\n  }\n";
      }
      out << "  } else {\n";
      anew("    ", /*counted=*/true);
      out << "    " << n << "_whole = true\n  }\n";
    }
    if (grid && !morton)
      out << "  " << n << "_bounds(" << n << ", " << n << "_cells, " << n
          << "_grid.cells)\n";
    if (morton) {
      // (The box of each leaf by the entity at its first row, the leaf's
      // cell its own: on many threads.)
      std::string lead = n + "_lead[c0." + rowField + "]";
      std::string most = ts == "i32"   ? "2147483647"
                         : ts == "i64" ? "9223372036854775807"
                                       : "1.0 / 0.0";
      std::string least =
          whole ? "0 - " + most + " - 1" : std::string("0.0 - 1.0 / 0.0");
      out << "  " << n << "_tree(" << n << ", " << n << "_cells, " << n
          << "_lead)\n"
          << "  for c0: " << of.front() << ", w: mut " << n << "_cells[" << lead
          << ".cell, " << lead << ".count] {\n"
          << "    if len(w) > 0 {\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "      var " << side("lo", d) << ": " << ts << " = " << most
            << "\n      var " << side("hi", d) << ": " << ts << " = " << least
            << "\n";
      out << "      for r in w[0].first..w[0].first + w[0].count {\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "        " << side("lo", d) << " = min(" << side("lo", d) << ", "
            << n << "[r]." << positions[d] << ")\n        " << side("hi", d)
            << " = max(" << side("hi", d) << ", " << n << "[r]." << positions[d]
            << ")\n";
      out << "      }\n";
      for (unsigned d = 0; d < dims; ++d)
        out << "      w[0]." << side("lo", d) << " = " << side("lo", d)
            << "\n      w[0]." << side("hi", d) << " = " << side("hi", d)
            << "\n";
      out << "    }\n  }\n"
          << "  " << n << "_up(" << n << "_cells)\n";
    }
    out << "  " << n
        << "_fresh = true\n}\n"
        // Out of date: its keys or what it copies changed, or (`moved`)
        // which entities it holds.
        << "system " << n << "_stale() { " << n << "_fresh = false }\n"
        << "system " << n << "_moved() {\n  " << n << "_fresh = false\n  " << n
        << "_whole = false\n}\n"
        << "fn " << n << "_find(order: " << n << ", key: " << spelled
        << ") -> i32 {\n"
        << "  var low = 0\n  var high = len(order)\n"
        << "  let rounds = (bits_of(len(order) as f32) >> 23) as i32 - 126\n"
        << "  for round in 0..rounds {\n    if low < high {\n"
        << "      let mid = (low + high) >> 1\n"
        << "      if order[mid].key < key { low = mid + 1 } else { high = mid "
           "}\n"
        << "    }\n  }\n  low\n}\n";
  }
  auto source = llvm::MemoryBuffer::getMemBufferCopy(text, "index " + n);
  StringRef bytes = source->getBuffer();
  sourceMgr.AddNewSourceBuffer(std::move(source), at);
  Lexer savedLexer = lexer;
  Token savedToken = token;
  const char *savedEnd = previousEnd;
  lexer = Lexer(bytes);
  buildingIndex = true;
  buildingIndexName = resolve(n);
  advance();
  LogicalResult made = parseDeclarations();
  buildingIndex = false;
  lexer = savedLexer;
  token = savedToken;
  previousEnd = savedEnd;
  if (failed(made))
    return failure();
  // (Made again incrementally: the entities that changed are listed in any
  // order, which the mending does not show, so the reactive `for` that
  // lists them goes through the log of what changed, not every entity.)
  if (how == "incremental" && !grid)
    if (auto update = dyn_cast_or_null<SystemOp>(SymbolTable::lookupSymbolIn(
            module, builder.getStringAttr(resolve(n + "_update")))))
      update.walk([&](AppendOp append) {
        if (append.getBufferAttr().getValue() == resolve(n + "_changed"))
          append->setAttr("ent.any_order", builder.getUnitAttr());
      });

  auto strings = [&](ArrayRef<std::string> list) {
    SmallVector<Attribute> attrs;
    for (const std::string &item : list)
      attrs.push_back(builder.getStringAttr(item));
    return builder.getArrayAttr(attrs);
  };
  SmallVector<std::string> ofSymbols, copied;
  for (const std::string &component : of)
    ofSymbols.push_back(resolve(component));
  for (const Copy &copy : copies) {
    copied.push_back(resolve(copy.component));
    copied.push_back(copy.field);
  }
  declared->setAttr(
      "ent.index",
      builder.getDictionaryAttr({
          builder.getNamedAttr("of", strings(ofSymbols)),
          builder.getNamedAttr("key", strings({resolve(of.front()), *key})),
          builder.getNamedAttr("copies", strings(copied)),
          builder.getNamedAttr("row", strings({resolve(of.front()), rowField})),
          builder.getNamedAttr("update",
                               builder.getStringAttr(resolve(n + "_update"))),
          builder.getNamedAttr("stale",
                               builder.getStringAttr(resolve(n + "_stale"))),
          builder.getNamedAttr("moved",
                               builder.getStringAttr(resolve(n + "_moved"))),
          builder.getNamedAttr("fresh",
                               builder.getStringAttr(resolve(n + "_fresh"))),
          builder.getNamedAttr(
              "own",
              strings(
                  morton
                      ? SmallVector<
                            std::string>{resolve(n),
                                         resolve(n + "_cells"),
                                         resolve(n + "_box"),
                                         resolve(n + "_lead")}
                  : grid   ? SmallVector<std::string>{resolve(n),
                                                      resolve(n + "_cells"),
                                                      resolve(n + "_grid")}
                  : hashed ? SmallVector<std::string>{resolve(n),
                                                      resolve(n + "_list"),
                                                      resolve(n + "_tags")}
                           : SmallVector<std::string>{resolve(n)})),
          builder.getNamedAttr(
              "hidden",
              strings(grid
                          ? SmallVector<std::string>{rowField, placeField, *key}
                          : SmallVector<std::string>{rowField, placeField})),
      }));
  {
    NamedAttrList info(declared->getAttrOfType<DictionaryAttr>("ent.index"));
    if (clustered)
      info.set("clustered", builder.getUnitAttr());
    if (eager)
      info.set("eager", builder.getUnitAttr());
    if (!how.empty())
      info.set("how", builder.getStringAttr(how));
    if (hashed)
      info.set("hash", builder.getUnitAttr());
    if (unique)
      info.set("unique", builder.getUnitAttr());
    declared->setAttr("ent.index", info.getDictionary(context));
  }
  Index &kept = indexes[bufferName.getValue()];
  kept = {resolve(of.front()), rowField, keyType, resolve(n + "_find")};
  kept.hashed = hashed;
  kept.textKey = textKey;
  kept.keyField = *key;
  for (const Copy &copy : copies)
    kept.copied.push_back({resolve(copy.component), copy.field, copy.name});
  for (const std::string &component : of)
    kept.of.push_back(resolve(component));
  if (grid) {
    kept.positions.assign(positions.begin(), positions.end());
    kept.positionType = positionType;
    kept.wrap = !wrapText.empty();
    kept.gridUnique = resolve(n + (morton ? "_box" : "_grid"));
    kept.cells = resolve(n + "_cells");
    kept.morton = morton;
    kept.bits = mortonBits;
    kept.leaf = mortonLeaf;
  }
  // Where it holds the same entities, each writes its own row: a loop
  // worth running on many threads from far fewer entities than a loop
  // that reads what lies in memory, since it reads what the loops before
  // it wrote there, on many threads (bench/traffic/RESULTS.md).
  auto update = cast<SystemOp>(SymbolTable::lookupSymbolIn(
      module, builder.getStringAttr(resolve(n + "_update"))));
  update.walk([&](QueryOp query) {
    if (!query->hasAttr("by"))
      query->setAttr("ent.parallel_min_entities",
                     builder.getI64IntegerAttr(kIndexParallelMin));
    if (query->hasAttr(QueryOp::kTriggersAttr))
      query->setAttr("ent.index_changes", builder.getUnitAttr());
  });
  // Clustered: made, the rows move into its order.
  if (clustered) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(update.getBody().front().getTerminator());
    ClusterOp::create(builder, loc(at),
                      FlatSymbolRefAttr::get(context, bufferName));
  }
  return success();
}

// An index holds a row for every entity that has its first component at
// most: as many as that one's capacity.
LogicalResult Parser::finishIndexOrders() {
  for (BufferOp buffer : module.getOps<BufferOp>()) {
    auto found = indexes.find(buffer.getSymName());
    if (found == indexes.end())
      continue;
    auto component = cast<ComponentOp>(SymbolTable::lookupSymbolIn(
        module, builder.getStringAttr(found->second.component)));
    IntegerAttr capacity = component.getCapacityAttr();
    if (!capacity)
      capacity = module->getAttrOfType<IntegerAttr>("ent.default_capacity");
    if (!capacity)
      return emitError(buffer.getLoc())
             << "index '" << buffer.getSymName()
             << "' holds a row for each entity with '"
             << found->second.component
             << "': give that component a capacity (or the program a "
                "default_capacity)";
    buffer.setCapacityAttr(builder.getI64IntegerAttr(capacity.getInt()));
    // A hash index: room for its slots twice as many as there may be
    // entities, at least, a power of two; its list as many as there may
    // be entities.
    if (found->second.hashed) {
      int64_t slots = 1;
      while (slots < 2 * capacity.getInt())
        slots *= 2;
      buffer.setCapacityAttr(builder.getI64IntegerAttr(slots));
      cast<BufferOp>(
          SymbolTable::lookupSymbolIn(
              module, builder.getStringAttr(buffer.getSymName() + "_list")))
          .setCapacityAttr(builder.getI64IntegerAttr(capacity.getInt()));
      // (Its tags: one for each slot, and the first 16 again.)
      cast<BufferOp>(
          SymbolTable::lookupSymbolIn(
              module, builder.getStringAttr(buffer.getSymName() + "_tags")))
          .setCapacityAttr(builder.getI64IntegerAttr(slots + 16));
      std::string update = buffer.getSymName().str() + "_update";
      module.walk([&](arith::ConstantOp constant) {
        auto value = dyn_cast<IntegerAttr>(constant.getValue());
        if (!value || value.getInt() != kSlotsPlaceholder)
          return;
        auto system = constant->getParentOfType<SystemOp>();
        if (system && system.getSymName() == update)
          constant.setValueAttr(IntegerAttr::get(value.getType(), slots));
      });
    }
    // Made again incrementally: as many changed as there may be entities.
    if (auto changed = dyn_cast_or_null<BufferOp>(SymbolTable::lookupSymbolIn(
            module, builder.getStringAttr(buffer.getSymName() + "_changed"))))
      changed.setCapacityAttr(builder.getI64IntegerAttr(capacity.getInt()));
    // A grid: as many cells at most as there may be entities.
    if (found->second.cells.empty())
      continue;
    auto setCapacity = [&](StringRef symbolName, int64_t rows) {
      cast<BufferOp>(SymbolTable::lookupSymbolIn(
                         module, builder.getStringAttr(symbolName)))
          .setCapacityAttr(builder.getI64IntegerAttr(rows));
    };
    // A Morton index: a leaf for each entity at most, and cells split, of
    // more than `leaf` each and apart, as many as fit at each level.
    if (const Index &index = found->second; index.morton) {
      int64_t entities = capacity.getInt();
      setCapacity(index.cells,
                  entities + index.bits * (entities / (index.leaf + 1)) + 1);
      setCapacity(buffer.getSymName().str() + "_lead", entities);
      continue;
    }
    setCapacity(found->second.cells, capacity.getInt() + 1);
    std::string update =
        buffer.getSymName().str() + "_update";
    module.walk([&](arith::ConstantOp constant) {
      auto value = dyn_cast<IntegerAttr>(constant.getValue());
      if (!value || value.getInt() != kCellsPlaceholder)
        return;
      auto system = constant->getParentOfType<SystemOp>();
      if (system && system.getSymName() == update)
        constant.setValueAttr(
            IntegerAttr::get(value.getType(), capacity.getInt()));
    });
  }
  return success();
}

// Index.row(e): where entity e is in the index (none where it is not);
// Index.find(key): the first row whose key is not below `key` (as many
// as the index has where none is).
FailureOr<mlir::Value> Parser::emitIndexCall(const Expr &expr,
                                             StringRef symbolName,
                                             mlir::Value given) {
  Location at = loc(expr.loc);
  const Index &index = indexes[symbolName];
  auto rowType =
      IndexOfType::get(context, FlatSymbolRefAttr::get(context, symbolName));
  if (expr.operands.size() != 1)
    return error(expr.loc, "'" + expr.name + "." + expr.field +
                               "' takes one argument");
  if (expr.field == "row") {
    if (inFunction)
      return error(expr.loc, "a fn or proc has no entities: where an entity "
                             "is in '" + expr.name + "' is asked in a system");
    FailureOr<mlir::Value> who =
        emit(*expr.operands[0], EntityType::get(context));
    if (failed(who))
      return failure();
    if (!isa<EntityType>(who->getType()))
      return error(expr.operands[0]->loc, "'" + expr.name +
                                              ".row' is given an entity");
    auto lookup = LookupOp::create(
        builder, at, builder.getI32Type(), builder.getI1Type(), *who,
        FlatSymbolRefAttr::get(context, index.component),
        builder.getStringAttr(index.rowField));
    mlir::Value none = integer(at, builder.getI32Type(), -1);
    mlir::Value row = arith::SelectOp::create(builder, at, lookup.getFound(),
                                              lookup.getValue(), none);
    return asIndex(at, row, rowType);
  }
  if ((expr.field == "get") != index.hashed)
    return error(expr.loc,
                 index.hashed ? "'" + expr.name +
                                    "' is a hash index: an "
                                    "entity by its key is '" +
                                    expr.name + ".get(key)'"
                              : "'" + expr.name +
                                    ".get' is of a hash index "
                                    "('by key hash'); this one is in order: '" +
                                    expr.name + ".find(key)'");
  if (index.hashed) {
    // The entity of a key, of a hash index (none where none has it): of a
    // text, by its two hashes, as the index holds it.
    if (given || inFunction)
      return error(expr.loc, "'" + expr.name +
                                 ".get' is asked of the "
                                 "world's index, in a system");
    Type wanted = index.textKey ? Type(StringType::get(context)) : index.key;
    FailureOr<mlir::Value> key = emit(*expr.operands[0], wanted);
    if (failed(key))
      return failure();
    if (!index.textKey && key->getType() != index.key)
      return mismatch(expr.operands[0]->loc, key->getType(), index.key);
    mlir::Value check;
    if (index.textKey) {
      Type i64 = builder.getI64Type();
      auto seed = [&](uint64_t value) -> mlir::Value {
        return arith::ConstantIntOp::create(builder, at, i64, int64_t(value));
      };
      mlir::Value text = *key;
      key = TextHashOp::create(builder, at, i64, text,
                               seed(0xbdd89aa982704029ull))
                .getResult();
      check = TextHashOp::create(
          builder, at, i64, text,
          seed(0xbdd89aa982704029ull ^ 0x9e3779b97f4a7c15ull));
    }
    return IndexGetOp::create(builder, at, EntityType::get(context),
                              FlatSymbolRefAttr::get(context, symbolName), *key,
                              check)
        .getResult();
  }
  FailureOr<mlir::Value> key = emit(*expr.operands[0], index.key);
  if (failed(key))
    return failure();
  if (key->getType() != index.key)
    return mismatch(expr.operands[0]->loc, key->getType(), index.key);
  Function &find = functions.ofSymbol(index.find);
  mlir::Value rows = given;
  if (!rows)
    rows = BufferOfOp::create(builder, at, find.params.front(),
                              FlatSymbolRefAttr::get(context, symbolName))
               .getResult();
  mlir::Value row =
      InvokeOp::create(builder, at, builder.getI32Type(),
                       FlatSymbolRefAttr::get(context, index.find),
                       ValueRange{rows, *key}, UnitAttr())
          ->getResult(0);
  return asIndex(at, row, rowType);
}

// Name += { field: value, ... } / Name = []: a row more, or none.
LogicalResult Parser::parseBufferStatement(const std::string &name,
                                           llvm::SMLoc at) {
  BufferUse held = *bufferNamed(name);
  const Record &buffer = buffers.ofSymbol(held.symbol);
  auto target = FlatSymbolRefAttr::get(context, held.symbol);
  if (inFunction && !held.given)
    return error(at, "'" + name + "' is a buffer, which a system fills: a "
                     "proc changes one it is given ('name: mut " + name +
                         "')");
  if (!held.mut && indexes.count(held.symbol))
    return error(at, "'" + name + "' is an index: the program keeps its rows "
                     "in order, and they are only read");
  if (!held.mut)
    return error(at, "'" + name + "' was given to read: a proc that changes "
                     "a buffer takes it as 'name: mut Buffer'");
  // { field: value, .. }: a row, with every field.
  auto parseRow = [&]() -> FailureOr<SmallVector<mlir::Value>> {
    llvm::SMLoc rowAt = token.loc;
    if (failed(expect(Token::LBrace, "'{': a row, '{ field: value, .. }'")))
      return failure();
    SmallVector<ExprPtr> given(buffer.fields.size());
    while (!token.is(Token::RBrace)) {
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field");
      if (failed(field) || failed(expect(Token::Colon, "':'")))
        return failure();
      size_t place = 0;
      while (place < buffer.fields.size() &&
             buffer.fields[place].first != *field)
        ++place;
      if (place == buffer.fields.size())
        return error(fieldAt,
                     "buffer '" + name + "' has no field '" + *field + "'");
      if (given[place])
        return error(fieldAt, "'" + *field + "' is given twice");
      FailureOr<ExprPtr> value = parseExpr();
      if (failed(value))
        return failure();
      given[place] = std::move(*value);
      if (!consumeIf(Token::Comma))
        break;
    }
    if (failed(expect(Token::RBrace, "'}'")))
      return failure();
    SmallVector<mlir::Value> values;
    for (auto [value, field] : llvm::zip(given, buffer.fields)) {
      if (!value)
        return error(rowAt, "the row has no '" + field.first +
                                "': a row gives every field of '" + name +
                                "'");
      FailureOr<mlir::Value> emitted = emit(*value, field.second);
      if (failed(emitted))
        return failure();
      if (emitted->getType() != field.second)
        return mismatch(value->loc, emitted->getType(), field.second);
      values.push_back(*emitted);
    }
    return values;
  };
  // name[i].field op value: another value for a row it has.
  if (consumeIf(Token::LBracket)) {
    FailureOr<ExprPtr> number = parseExpr();
    if (failed(number) || failed(expect(Token::RBracket, "']'")) ||
        failed(expect(Token::Dot, "'.' and a field: a row is changed by "
                                  "its fields")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = buffer.fieldType(*field);
    if (!type)
      return error(fieldAt,
                   "buffer '" + name + "' has no field '" + *field + "'");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    // (In a `for` a row is given its value as it is: another value made
    // of the row would read the buffer, which the `for` does not.)
    if (inQuery && !inTurn && (op->first != Token::Assign || op->second))
      return error(at, "in a 'for' over entities a row of a buffer is given "
                       "a value with '=': each row is written by one entity "
                       "of the 'for', which reads nothing of the buffer (or "
                       "take turns: 'in turn by name.field')");
    FailureOr<mlir::Value> index = emitRowNumber(**number, target, name);
    if (failed(index))
      return failure();
    StringAttr fieldName = builder.getStringAttr(*field);
    return emitAssignment(
        at, op->first, op->second, [&] { return type; },
        [&] {
          return BufferAtOp::create(builder, loc(at), type, target, fieldName,
                                    *index)
              .getResult();
        },
        [&](mlir::Value stored) {
          BufferSetOp::create(builder, loc(at), target, fieldName, *index,
                              stored);
          return success();
        },
        **value);
  }
  if (consumeIf(Token::Assign)) {
    // Name = Name[..n]: its first n rows, and none after them.
    // Name = Other: the rows of another buffer of the same fields.
    if (token.is(Token::Identifier) && token.spelling != name &&
        !peek().is(Token::LBracket)) {
      llvm::SMLoc fromAt = token.loc;
      std::string from = token.spelling.str();
      advance();
      std::optional<BufferUse> source = bufferNamed(from);
      if (!source || source->given || held.given)
        return error(fromAt, "a buffer is given the rows of another buffer "
                             "of the program: '" +
                                 from + "' is none");
      const Record &mine = buffers.ofSymbol(held.symbol);
      const Record &theirs = buffers.ofSymbol(source->symbol);
      if (mine.fields != theirs.fields)
        return error(fromAt, "'" + from + "' has other fields than '" + name +
                                 "': a buffer is given the rows of "
                                 "one with the same fields, of the same "
                                 "types, in the same order");
      if (inQuery && !inTurn)
        return error(at, "a buffer is given another's rows outside a 'for': "
                         "every entity would");
      BufferCopyOp::create(builder, loc(at), target,
                           FlatSymbolRefAttr::get(context, source->symbol));
      return success();
    }
    if (token.is(Token::Identifier)) {
      llvm::SMLoc keptAt = token.loc;
      if (token.spelling != name)
        return error(keptAt, "a buffer keeps its own first rows, '" + name +
                                 " = " + name + "[..n]': it is not "
                                 "assigned another's");
      advance();
      if (failed(expect(Token::LBracket, "'[..n]': the rows it keeps")) ||
          failed(expect(Token::DotDot, "'..': '" + name + "[..n]' keeps "
                                       "the first n rows")))
        return failure();
      FailureOr<ExprPtr> count = parseExpr();
      if (failed(count) || failed(expect(Token::RBracket, "']'")))
        return failure();
      if (inQuery && !inTurn)
        return error(at, "a buffer is shortened outside a 'for': every "
                         "entity would shorten it");
      FailureOr<mlir::Value> rows = emit(**count, builder.getI32Type());
      if (failed(rows))
        return failure();
      if (!rows->getType().isInteger(32))
        return error((*count)->loc, "how many rows is an i32");
      BufferKeepOp::create(builder, loc(at), target, *rows);
      return success();
    }
    if (failed(expect(Token::LBracket, "'[]': a buffer is assigned no rows, "
                                       "'[{ .. }; n]', n rows alike, or '" +
                                       name + "[..n]', its first n")))
      return failure();
    if (inQuery && !inTurn)
      return error(at, "a buffer is emptied outside a 'for': every entity "
                       "would empty it");
    if (consumeIf(Token::RBracket)) {
      ClearOp::create(builder, loc(at), target);
      return success();
    }
    // [{ field: value, .. }; n]: so many rows, all alike.
    FailureOr<SmallVector<mlir::Value>> values = parseRow();
    if (failed(values) ||
        failed(expect(Token::Semicolon, "';' and how many rows: "
                                        "'[{ .. }; n]'")))
      return failure();
    FailureOr<ExprPtr> count = parseExpr();
    if (failed(count) || failed(expect(Token::RBracket, "']'")))
      return failure();
    FailureOr<mlir::Value> rows = emit(**count, builder.getI32Type());
    if (failed(rows))
      return failure();
    if (!rows->getType().isInteger(32))
      return error((*count)->loc, "how many rows is an i32");
    BufferFillOp::create(builder, loc(at), target, *values, *rows);
    return success();
  }
  if (!consumeIf(Token::PlusAssign))
    return error(at, "'" + name + "' is a buffer: '" + name + " += { field: "
                     "value, .. }' gives it a row, '" + name + " = []' "
                     "empties it, '" + name + "[i].field = value' changes "
                     "a row");
  if ((inEach || inEdges) && !inTurn && !inFunction)
    return error(at, "a row is given to a buffer once for each entity of a "
                     "'for', not in a loop over others in it");
  FailureOr<SmallVector<mlir::Value>> values = parseRow();
  if (failed(values))
    return failure();
  AppendOp::create(builder, loc(at), target, *values);
  return success();
}

// w[i].field op value: another value for a row of a window of a `for`,
// which its entity took (`w: mut Buffer[start, count]`): read and written
// as it likes, `+=` too.
LogicalResult Parser::parseWindowStatement(const std::string &name,
                                           const Variable &window,
                                           llvm::SMLoc at) {
  Variable held = window;
  if (failed(expect(Token::LBracket, "'[': a row of window '" + name +
                                         "' is changed, '" + name +
                                         "[i].field = value'")))
    return failure();
  FailureOr<ExprPtr> number = parseExpr();
  if (failed(number) || failed(expect(Token::RBracket, "']'")) ||
      failed(expect(Token::Dot, "'.' and a field: a row is changed by its "
                                "fields")))
    return failure();
  llvm::SMLoc fieldAt = token.loc;
  FailureOr<std::string> field = identifier("a field");
  if (failed(field))
    return failure();
  Type type = buffers.ofSymbol(held.component).fieldType(*field);
  if (!type)
    return error(fieldAt, "the buffer of window '" + name +
                              "' has no field '" + *field + "'");
  if (!held.mut)
    return error(at, "'" + name + "' is a window to read: 'name: mut " +
                         "Buffer[start, count]' takes rows to change");
  auto op = parseAssignOp();
  if (failed(op))
    return failure();
  FailureOr<ExprPtr> value = parseExpr();
  if (failed(value))
    return failure();
  FailureOr<mlir::Value> row = windowRow(held, **number);
  if (failed(row))
    return failure();
  auto target = FlatSymbolRefAttr::get(context, held.component);
  StringAttr fieldName = builder.getStringAttr(*field);
  return emitAssignment(
      at, op->first, op->second, [&] { return type; },
      [&] {
        auto read = BufferAtOp::create(builder, loc(at), type, target,
                                       fieldName, *row);
        read->setAttr(kWindowAttr, builder.getUnitAttr());
        return read.getResult();
      },
      [&](mlir::Value stored) {
        auto set = BufferSetOp::create(builder, loc(at), target, fieldName,
                                       *row, stored);
        set->setAttr(kWindowAttr, builder.getUnitAttr());
        return success();
      },
      **value);
}

// component Name { fields } [apart] [capacity N] / tag Name [apart]
// [capacity N]
LogicalResult Parser::parseComponent(bool tag) {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a component name");
  if (failed(name))
    return failure();
  Record record;
  if (!tag && failed(parseFields(record)))
    return failure();
  bool apart = consumeKeyword("apart");
  IntegerAttr capacity;
  if (consumeKeyword("capacity")) {
    FailureOr<int64_t> value = integer("a capacity");
    if (failed(value))
      return failure();
    capacity = builder.getI64IntegerAttr(*value);
  }
  SmallVector<Attribute> names, types;
  for (auto &[field, type] : record.fields) {
    names.push_back(builder.getStringAttr(field));
    types.push_back(TypeAttr::get(type));
  }
  ComponentOp::create(builder, loc(at), declareSymbol(at, *name),
                      builder.getArrayAttr(names), builder.getArrayAttr(types),
                      capacity, apart ? builder.getUnitAttr() : UnitAttr());
  components[*name] = std::move(record);
  return success();
}

// relation Name [{ fields }] [from C] [to D] [tree [sorted]] capacity N
LogicalResult Parser::parseRelation() {
  llvm::SMLoc at = token.loc;
  // (Source)-[Name { fields }]->(Target): what the ends have, either or
  // both of which may be left open, `()`. Without ends, the name alone.
  FlatSymbolRefAttr ends[2];
  std::string endNames[2];
  bool hasEnds = token.is(Token::LParen);
  auto parseEnd = [&](unsigned index) -> LogicalResult {
    if (failed(expect(Token::LParen, "'('")))
      return failure();
    if (!token.is(Token::RParen)) {
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component))
        return failure();
      if (!components.count(*component))
        return error(componentAt, "unknown component '" + *component + "'");
      ends[index] = symbol(*component);
      endNames[index] = *component;
    }
    return expect(Token::RParen, "')'");
  };
  ArrowStart arrow;
  if (hasEnds) {
    if (failed(parseEnd(0)))
      return failure();
    FailureOr<ArrowStart> start = parseArrowStart();
    if (failed(start))
      return failure();
    arrow = *start;
    if (arrow.sibling || arrow.reversed)
      return error(arrow.loc, "a relation is declared from its sources to "
                              "its targets: '(Source)-[Name]->(Target)'");
    at = token.loc;
  }
  FailureOr<std::string> name = identifier("a relation name");
  if (failed(name))
    return failure();
  if (current->names.contains(*name))
    return error(at, "'" + *name + "' is already declared");
  Record record;
  llvm::SMLoc fieldsAt = token.loc;
  if (token.is(Token::LBrace) && failed(parseFields(record)))
    return failure();
  // (An edge's fields are moved as plain values wherever edges are put
  // in order or taken away: nothing there gives a text's block back.)
  for (auto &[field, type] : record.fields) {
    if (isa<StringType>(type))
      return error(fieldsAt, "an edge does not hold a text of any length "
                             "yet ('" + field + "'): give it a capacity "
                             "('text[N]')");
    if (isa<IndexOfType>(type))
      return error(fieldsAt, "an edge does not hold a row number of a "
                             "buffer yet ('" + field + "'): an i32 does");
  }
  if (hasEnds && (failed(parseArrowEnd(arrow)) || failed(parseEnd(1))))
    return failure();
  if (token.isKeyword("from") || token.isKeyword("to"))
    return error("what a relation's ends have is said with an arrow: "
                 "'relation (Source)-[" + *name + "]->(Target) ...'");
  record.tree = consumeKeyword("tree");
  // sorted: the entities stored in the tree's order.
  llvm::SMLoc sortedAt = token.loc;
  bool sorted = consumeKeyword("sorted");
  if (sorted && !record.tree)
    return error(sortedAt, "only a tree gives its entities an order: "
                           "'relation " + *name + " ... tree sorted "
                           "capacity N'");
  if (sorted && !(ends[0] && ends[1]))
    return error(sortedAt, "a sorted tree says what its ends have, so that "
                           "the archetypes it sorts are known: 'relation "
                           "(C)-[" + *name + "]->(D) tree sorted capacity "
                           "N'");
  // ordered by Component.field: the children of an entity, by that field
  // of theirs.
  FlatSymbolRefAttr orderComponent;
  StringAttr orderField;
  llvm::SMLoc orderedAt = token.loc;
  if (consumeKeyword("ordered")) {
    if (!record.tree)
      return error(orderedAt, "only a tree's entities have siblings to be "
                              "in an order: 'relation " + *name +
                                  " ... tree ordered by C.f capacity N'");
    if (failed(expectKeyword("by")))
      return failure();
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component))
      return failure();
    auto key = components.find(*component);
    if (key == components.end())
      return error(componentAt, "unknown component '" + *component + "'");
    if (failed(expect(Token::Dot, "'.' and a field")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = key->second.fieldType(*field);
    if (!type)
      return error(fieldAt, "component '" + *component + "' has no field '" +
                                *field + "'");
    if (!type.isSignlessInteger() || type.isInteger(1))
      return error(fieldAt, "the children are ordered by an integer, and '" +
                                *component + "." + *field + "' is not one");
    orderComponent = symbol(*component);
    orderField = builder.getStringAttr(*field);
    record.ordered = true;
  }
  if (failed(expectKeyword("capacity")))
    return failure();
  FailureOr<int64_t> capacity = integer("a capacity");
  if (failed(capacity))
    return failure();
  SmallVector<Attribute> names, types;
  for (auto &[field, type] : record.fields) {
    names.push_back(builder.getStringAttr(field));
    types.push_back(TypeAttr::get(type));
  }
  RelationOp::create(builder, loc(at), declareSymbol(at, *name),
                     builder.getArrayAttr(names), builder.getArrayAttr(types),
                     ends[0], ends[1],
                     record.tree ? builder.getUnitAttr() : UnitAttr(),
                     sorted ? builder.getUnitAttr() : UnitAttr(),
                     builder.getI64IntegerAttr(*capacity), orderComponent,
                     orderField);
  record.ends[0] = endNames[0];
  record.ends[1] = endNames[1];
  relations[*name] = std::move(record);
  return success();
}

// unique Name { fields } / unique Name: type
LogicalResult Parser::parseUnique() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a unique's name");
  if (failed(name))
    return failure();
  Record record;
  if (consumeIf(Token::Colon)) {
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    record.fields.push_back({"value", *type});
    record.shorthand = true;
  } else if (failed(parseFields(record))) {
    return failure();
  }
  SmallVector<Attribute> names, types;
  for (auto &[field, type] : record.fields) {
    names.push_back(builder.getStringAttr(field));
    types.push_back(TypeAttr::get(type));
  }
  ResourceOp declared =
      ResourceOp::create(builder, loc(at), declareSymbol(at, *name),
                         builder.getArrayAttr(names),
                         builder.getArrayAttr(types));
  if (transientNext)
    declared->setAttr(WorldLayout::kTransientAttr, builder.getUnitAttr());
  uniques[*name] = std::move(record);
  return success();
}

// archetype Name { [optional] Component, ... } capacity N
LogicalResult Parser::parseArchetype() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("an archetype name");
  if (failed(name) || failed(expect(Token::LBrace, "'{'")))
    return failure();
  SmallVector<Attribute> all, optional;
  while (!token.is(Token::RBrace)) {
    bool isOptional = consumeKeyword("optional");
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component))
      return failure();
    if (!components.count(*component))
      return error(componentAt, "unknown component '" + *component + "'");
    all.push_back(symbol(*component));
    if (isOptional)
      optional.push_back(symbol(*component));
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RBrace, "'}'")) ||
      failed(expectKeyword("capacity")))
    return failure();
  FailureOr<int64_t> capacity = integer("a capacity");
  if (failed(capacity))
    return failure();
  ArchetypeOp::create(builder, loc(at), declareSymbol(at, *name),
                      builder.getArrayAttr(all),
                      optional.empty() ? ArrayAttr()
                                       : builder.getArrayAttr(optional),
                      builder.getI64IntegerAttr(*capacity));
  return success();
}

// A, B, ... (up to the next keyword or '{')
FailureOr<ArrayAttr> Parser::parseNameList(StringRef what) {
  SmallVector<Attribute> names;
  if (token.is(Token::LBrace) || token.isKeyword("writes") ||
      token.is(Token::Semicolon))
    return builder.getArrayAttr(names);
  do {
    FailureOr<std::string> name = identifier(what);
    if (failed(name))
      return failure();
    names.push_back(symbol(*name));
  } while (consumeIf(Token::Comma));
  return builder.getArrayAttr(names);
}

// [gpu] system name(params) [reads A, B] [writes C] { statements }
// extern system name(params) [reads A, B] [writes C]
LogicalResult Parser::parseSystem(bool isExtern, bool gpu) {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a system name");
  if (failed(name) || failed(expect(Token::LParen, "'('")))
    return failure();
  SmallVector<std::pair<std::string, Type>> params;
  while (!token.is(Token::RParen)) {
    FailureOr<std::string> param = identifier("a parameter name");
    if (failed(param) || failed(expect(Token::Colon, "':'")))
      return failure();
    llvm::SMLoc typeAt = token.loc;
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    if (isExtern && isa<IndexOfType>(*type))
      return error(typeAt, "C is given numbers: an i32 for a row number");
    params.push_back({*param, *type});
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RParen, "')'")))
    return failure();

  OperationState state(loc(at), isExtern ? ExternOp::getOperationName()
                                         : SystemOp::getOperationName());
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     declareSymbol(at, *name));
  if (gpu)
    state.addAttribute("gpu", builder.getUnitAttr());
  if (consumeKeyword("reads")) {
    FailureOr<ArrayAttr> reads = parseNameList("a component or unique");
    if (failed(reads))
      return failure();
    state.addAttribute("reads", *reads);
  }
  if (consumeKeyword("writes")) {
    FailureOr<ArrayAttr> writes =
        parseNameList("a component, unique or archetype");
    if (failed(writes))
      return failure();
    state.addAttribute("writes", *writes);
  }
  if (isExtern) {
    // Implemented in C: no body. A ';' may end the declaration.
    if (token.is(Token::LBrace))
      return error("an extern system has no body; it is implemented "
                   "outside the program");
    consumeIf(Token::Semicolon);
    SmallVector<Type> types;
    for (auto &[param, type] : params)
      types.push_back(type);
    state.addAttribute("params", builder.getTypeArrayAttr(types));
    systems[*name] = types;
    builder.create(state);
    return success();
  }
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  SmallVector<Type> types;
  for (auto &[param, type] : params) {
    types.push_back(type);
    block->addArgument(type, loc(at));
  }
  systems[*name] = types;
  Operation *system = builder.create(state);
  llvm::SaveAndRestore<Operation *> inThis(systemOp, system);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [param, arg] : llvm::zip(params, block->getArguments()))
    bind(param.first, Variable::ofValue(arg));
  llvm::SaveAndRestore<bool> calls(inSystem, true);
  if (failed(parseBlock()))
    return failure();
  SystemOp::ensureTerminator(system->getRegion(0), builder, loc(at));
  return success();
}

// fn name(params) -> type { [let x = e]* value }
// extern fn name(params) -> type
// extern proc name(params) [-> type]
LogicalResult Parser::parseFunction(bool proc, bool isExtern) {
  llvm::SMLoc at = token.loc;
  StringRef kind = proc ? "proc" : "fn";
  FailureOr<std::string> name = identifier("a function name");
  if (failed(name) || failed(expect(Token::LParen, "'('")))
    return failure();
  Function function;
  function.proc = proc;
  function.isExtern = isExtern;
  SmallVector<std::string> names;
  while (!token.is(Token::RParen)) {
    FailureOr<std::string> param = identifier("a parameter name");
    if (failed(param) || failed(expect(Token::Colon, "':'")))
      return failure();
    // name: Buffer, to read; name: mut Buffer, to change (a proc's).
    llvm::SMLoc typeAt = token.loc;
    bool changed = token.isKeyword("mut") && peek().is(Token::Identifier);
    if (changed)
      advance();
    llvm::SaveAndRestore<bool> hands(handsBuffers, true);
    typedComponent.clear();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    function.components.push_back(typedComponent);
    if (!typedComponent.empty() && isExtern)
      return error(typeAt, "the entities that have a component are given "
                           "to a fn or proc of the program, not to one in "
                           "C");
    if (auto handed = dyn_cast<BufferType>(*type)) {
      if (isExtern && !proc)
        return error(typeAt, "an extern fn computes from values; a buffer "
                             "is handed to an extern proc");
      if (changed && !proc)
        return error(typeAt, "a fn only computes: it reads a buffer it is "
                             "given, and a proc may change one ('mut')");
      if (isExtern)
        for (auto &[field, fieldType] :
             buffers.ofSymbol(handed.getName().getValue()).fields)
          if (isa<EntityType>(fieldType))
            return error(typeAt, "'" + field + "' is an entity: a buffer "
                                 "that says which entities is the "
                                 "program's own, and is not handed to C");
      *type = BufferType::get(context, handed.getName(), changed);
    } else if (auto rows = dyn_cast<RowsType>(*type)) {
      if (isExtern)
        return error(typeAt, "rows of a buffer are given to a fn or proc of "
                             "the program; C is handed a whole buffer");
      if (changed && !proc)
        return error(typeAt, "a fn only computes: it reads rows it is "
                             "given, and a proc may change them ('mut')");
      *type = RowsType::get(context, rows.getName(), changed);
    } else if (changed) {
      return error(typeAt, "'mut' is of a buffer a proc is given to "
                           "change; a value is its own");
    } else if (isExtern && isa<IndexOfType>(*type)) {
      return error(typeAt, "C is given numbers: an i32 for a row number "
                           "('as i32')");
    }
    names.push_back(*param);
    function.params.push_back(*type);
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RParen, "')'")))
    return failure();
  // `-> type`: what it gives back.
  if (token.is(Token::Minus) && peek().is(Token::Greater)) {
    advance();
    advance();
    llvm::SMLoc resultAt = token.loc;
    // `-> (T, U)`: several values.
    bool several = consumeIf(Token::LParen);
    do {
      FailureOr<Type> type = parseType();
      if (failed(type))
        return failure();
      function.results.push_back(*type);
    } while (several && consumeIf(Token::Comma));
    if (several && failed(expect(Token::RParen, "')'")))
      return failure();
    if (several && function.results.size() < 2)
      return error(resultAt, "one value is written without parentheses");
    if (isExtern && several)
      return error(resultAt, "C gives one value back; a fn with a body may "
                             "give several");
    if (isExtern && isa<TextType, StringType>(function.results.front()))
      return error(resultAt, "C cannot give a text back yet");
    if (isExtern && isa<IndexOfType>(function.results.front()))
      return error(resultAt, "C gives numbers back: an i32 for a row number");
    if (!several)
      function.result = function.results.front();
  } else if (!proc) {
    return error(at, isExtern
                         ? "an extern fn gives a value back ('-> type'); one "
                           "that only acts is an 'extern proc'"
                         : "a fn gives a value back ('-> type')");
  }
  if (isExtern && token.is(Token::LBrace))
    return error(at,
                 "an extern " + kind + " has no body; it is implemented in C");
  // Without a body, and not in C: said now, with its body further down,
  // so that what stands between may call it (two that call each other).
  bool later = !isExtern && !token.is(Token::LBrace);
  if (isExtern)
    consumeIf(Token::Semicolon);
  if (name->size() == 3 && (*name == "min" || *name == "max" || *name == "len"))
    return error(at, "'" + *name + "' is built in");
  // The body of one that was said before: as it was said.
  auto said = functions.find(*name);
  FunctionOp op;
  bool defines = said != functions.end() && said->second.later && !isExtern &&
                 !later;
  if (defines) {
    Function &before = said->second;
    if (before.proc != proc || before.params != function.params ||
        before.results != function.results ||
        before.components != function.components) {
      emitError(loc(at)) << "'" << *name << "' was said to take and give "
                            "otherwise where it was said without its body";
      emitError(loc(before.laterAt)) << "said here";
      hadError = true;
      return failure();
    }
    op = cast<FunctionOp>(before.later);
    before.later = nullptr;
  } else {
    if (said != functions.end() && said->second.later)
      return error(at, "'" + *name + "' was said before, without its body: "
                       "what follows is its body, once");
    StringAttr symbolName = declareSymbol(at, *name);
    op = FunctionOp::create(builder, loc(at), symbolName,
                            builder.getTypeArrayAttr(function.params),
                            builder.getTypeArrayAttr(function.results), proc);
    if (later) {
      function.later = op;
      function.laterAt = at;
    }
  }
  // Known before its body is parsed: a fn may call itself.
  Type result = function.result;
  SmallVector<Type> results = function.results;
  SmallVector<Type> params = function.params;
  SmallVector<std::string> given = function.components;
  if (!defines)
    functions[*name] = std::move(function);
  if (isExtern || later)
    return success();

  auto *block = new Block();
  op.getBody().push_back(block);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [param, type] : llvm::zip(names, params)) {
    mlir::Value arg = block->addArgument(type, loc(at));
    // (Rows it is given: a window of them, from 0.)
    if (auto rows = dyn_cast<RowsType>(type)) {
      Variable window{Variable::Window,
                      RowsStartOp::create(builder, loc(at),
                                          builder.getI32Type(), arg)
                          .getResult(),
                      rows.getName().getValue().str(), rows.getMut()};
      window.which =
          RowsCountOp::create(builder, loc(at), builder.getI32Type(), arg)
              .getResult();
      bind(param, window);
      continue;
    }
    bind(param, Variable::ofValue(arg));
  }
  givenComponents.clear();
  for (auto [param, component] : llvm::zip(names, given))
    if (!component.empty())
      givenComponents[param] = component;
  llvm::scope_exit forget([&] { givenComponents.clear(); });
  llvm::SaveAndRestore<bool> calls(inSystem, true);
  llvm::SaveAndRestore<bool> computes(inFunction, true);
  llvm::SaveAndRestore<bool> acts(inProc, proc);
  advance();
  // A proc that gives nothing: statements, to the end.
  if (results.empty()) {
    while (!token.is(Token::RBrace)) {
      if (token.is(Token::Eof))
        return error("expected '}' at the end of the proc's body");
      if (failed(parseStatement()))
        return failure();
      consumeIf(Token::Semicolon);
    }
    advance();
    YieldOp::create(builder, loc(at), ValueRange());
    return success();
  }
  // Statements, then the value the fn gives.
  while (true) {
    bool statement = token.isKeyword("let") || token.isKeyword("var") ||
                     token.isKeyword("for") || token.isKeyword("while") ||
                     (token.isKeyword("loop") && peek().is(Token::LBrace));
    if (token.isKeyword("if"))
      statement = ifIsStatement();
    // name op value: an assignment (`==` would be part of the value).
    if (token.is(Token::Identifier) && lookup(token.spelling)) {
      Token next = peek();
      statement = next.is(Token::Assign) ||
                  computedBy(next.kind) != Token::Eof ||
                  next.isKeyword("min") || next.isKeyword("max");
    }
    // (A buffer it was given, changed.)
    if (atBufferStatement())
      statement = true;
    // (In a proc: a call of a proc, for what it does.)
    if (!statement && proc && token.is(Token::Identifier) &&
        peek().is(Token::LParen)) {
      Callable *through = callableNamed(token.spelling);
      statement = through ? through->proc && !through->result
                          : functions.count(token.spelling) &&
                                functions[token.spelling].results.empty();
      // (What a unique index declares: the call that stops the program.)
      statement |= buildingIndex && token.isKeyword("index_duplicate");
    }
    if (!statement)
      break;
    if (failed(parseStatement()))
      return failure();
    consumeIf(Token::Semicolon);
  }
  if (token.is(Token::RBrace))
    return error("a " + kind + "'s body ends with the value it gives");
  FailureOr<ExprPtr> body = parseExpr();
  if (failed(body))
    return failure();
  llvm::SaveAndRestore<bool> gives(
      givesText, llvm::any_of(results, [](Type type) {
        return isa<StringType>(type);
      }));
  SmallVector<mlir::Value> values;
  if (results.size() > 1) {
    FailureOr<SmallVector<mlir::Value>> several = emitSeveral(**body, results);
    if (failed(several))
      return failure();
    values = std::move(*several);
  } else {
    FailureOr<mlir::Value> value = emit(**body, result);
    if (failed(value))
      return failure();
    if (value->getType() != result)
      return error((*body)->loc,
                   "the fn's value has a different type than it gives back");
    values.push_back(*value);
  }
  consumeIf(Token::Semicolon);
  if (!token.is(Token::RBrace))
    return error("expected '}' after the fn's value, found '" +
                 token.spelling + "'");
  advance();
  YieldOp::create(builder, loc(at), values);
  return success();
}

/// At an `if` in a fn's body: whether it is a statement. The `if` that
/// starts the last thing in the body is (the start of) the value the fn
/// gives: after its last branch comes the body's '}', or what continues a
/// value ('+', '==', 'as', ...). A '-' there starts the next thing.
bool Parser::ifIsStatement() {
  Lexer ahead = lexer;
  Token next = ahead.next();
  // Skips to the '}' that closes the block starting at the next '{'
  // outside parentheses, and reads the token after it.
  auto skipBlock = [&] {
    for (int parens = 0; !next.is(Token::Eof); next = ahead.next()) {
      if (next.is(Token::LParen))
        ++parens;
      else if (next.is(Token::RParen))
        --parens;
      else if (next.is(Token::LBrace) && parens <= 0)
        break;
    }
    for (int braces = 0; !next.is(Token::Eof); next = ahead.next()) {
      if (next.is(Token::LBrace))
        ++braces;
      else if (next.is(Token::RBrace) && --braces == 0)
        break;
    }
    next = ahead.next();
  };
  skipBlock();
  while (next.isKeyword("else"))
    skipBlock();
  switch (next.kind) {
  case Token::RBrace:
  case Token::Plus:
  case Token::Star:
  case Token::Slash:
  case Token::Percent:
  case Token::Equal:
  case Token::NotEqual:
  case Token::Less:
  case Token::LessEqual:
  case Token::Greater:
  case Token::GreaterEqual:
  case Token::AndAnd:
  case Token::OrOr:
    return false;
  default:
    return !next.isKeyword("as");
  }
}

// schedule name(params) [run_if cond] { system(args) [run_if cond] ... }

/// Emit a run or schedule condition, ending its block with `ent.yield`.
LogicalResult Parser::emitCondition(const Expr &expr) {
  FailureOr<mlir::Value> value = emit(expr, builder.getI1Type());
  if (failed(value))
    return failure();
  if (!value->getType().isInteger(1))
    return error(expr.loc, "a 'run_if' condition must be a bool");
  YieldOp::create(builder, loc(expr.loc), ValueRange{*value});
  return success();
}

LogicalResult Parser::parseSchedule() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a schedule name");
  if (failed(name) || failed(expect(Token::LParen, "'('")))
    return failure();
  SmallVector<std::pair<std::string, Type>> params;
  while (!token.is(Token::RParen)) {
    FailureOr<std::string> param = identifier("a parameter name");
    if (failed(param) || failed(expect(Token::Colon, "':'")))
      return failure();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    params.push_back({*param, *type});
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RParen, "')'")))
    return failure();

  ExprPtr runIf;
  if (consumeKeyword("run_if")) {
    FailureOr<ExprPtr> condition = parseExpr();
    if (failed(condition))
      return failure();
    runIf = std::move(*condition);
  }

  OperationState state(loc(at), ScheduleOp::getOperationName());
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     declareSymbol(at, *name));
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  for (auto &[param, type] : params)
    block->addArgument(type, loc(at));
  state.addRegion(); // the condition, filled below if there is one
  Operation *schedule = builder.create(state);
  scheduleOps[*name] = schedule;
  schedules.try_emplace(*name);
  for (auto &[param, type] : params)
    schedules[*name].push_back(type);

  OpBuilder::InsertionGuard guard(builder);
  // The schedule's condition: a block of its own taking the parameters.
  if (runIf) {
    auto *conditionBlock = new Block();
    schedule->getRegion(1).push_back(conditionBlock);
    for (auto &[param, type] : params)
      conditionBlock->addArgument(type, loc(at));
    builder.setInsertionPointToEnd(conditionBlock);
    ScopeGuard scope(*this);
    for (auto [param, arg] : llvm::zip(params, conditionBlock->getArguments()))
      bind(param.first, Variable::ofValue(arg));
    if (failed(emitCondition(*runIf)))
      return failure();
  }
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [param, arg] : llvm::zip(params, block->getArguments()))
    bind(param.first, Variable::ofValue(arg));
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  while (!token.is(Token::RBrace)) {
    llvm::SMLoc callAt = token.loc;
    FailureOr<std::string> system = identifier("a system to run");
    if (failed(system) || failed(expect(Token::LParen, "'('")))
      return failure();
    auto known = systems.find(*system);
    // A schedule: its runs take their place here, with its parameters
    // the values given.
    auto whole = schedules.find(*system);
    if (known == systems.end() && whole != schedules.end() &&
        scheduleOps.count(*system)) {
      Operation *other = scheduleOps[*system];
      if (other == schedule)
        return error(callAt, "a schedule does not run itself");
      if (!other->getRegion(1).empty())
        return error(callAt, "'" + *system + "' runs only if its own "
                                 "condition holds, which a schedule that "
                                 "runs it would have to ask: not supported "
                                 "yet");
      SmallVector<mlir::Value> given;
      while (!token.is(Token::RParen)) {
        FailureOr<ExprPtr> arg = parseExpr();
        if (failed(arg))
          return failure();
        Type expected = given.size() < whole->second.size()
                            ? whole->second[given.size()]
                            : Type();
        FailureOr<mlir::Value> value = emit(**arg, expected);
        if (failed(value))
          return failure();
        given.push_back(*value);
        if (!consumeIf(Token::Comma))
          break;
      }
      if (failed(expect(Token::RParen, "')'")))
        return failure();
      if (given.size() != whole->second.size())
        return error(callAt, "'" + *system + "' takes " +
                                 Twine(whole->second.size()) +
                                 " values, and is given " +
                                 Twine(given.size()));
      Block &runs = other->getRegion(0).front();
      IRMapping mapping;
      for (auto [parameter, value] : llvm::zip(runs.getArguments(), given)) {
        if (parameter.getType() != value.getType())
          return error(callAt, "a value given to '" + *system +
                                   "' has another type than its parameter");
        mapping.map(parameter, value);
      }
      for (Operation &op : runs.without_terminator())
        builder.insert(op.clone(mapping));
      if (token.isKeyword("run_if"))
        return error("a schedule that is run in another has no condition "
                     "there: give its runs theirs");
      consumeIf(Token::Semicolon);
      continue;
    }
    if (known == systems.end())
      return error(callAt, "unknown system '" + *system +
                               "'; declare systems before the schedules "
                               "that run them");
    SmallVector<mlir::Value> args;
    while (!token.is(Token::RParen)) {
      FailureOr<ExprPtr> arg = parseExpr();
      if (failed(arg))
        return failure();
      Type expected = args.size() < known->second.size()
                          ? known->second[args.size()]
                          : Type();
      FailureOr<mlir::Value> value = emit(**arg, expected);
      if (failed(value))
        return failure();
      args.push_back(*value);
      if (!consumeIf(Token::Comma))
        break;
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    auto run = RunOp::create(builder, loc(callAt), symbol(*system), args);
    if (consumeKeyword("run_if")) {
      FailureOr<ExprPtr> condition = parseExpr();
      if (failed(condition))
        return failure();
      OpBuilder::InsertionGuard inner(builder);
      builder.setInsertionPointToEnd(
          builder.createBlock(&run.getCondition()));
      if (failed(emitCondition(**condition)))
        return failure();
    }
    consumeIf(Token::Semicolon);
  }
  if (failed(expect(Token::RBrace, "'}'")))
    return failure();
  ScheduleOp::ensureTerminator(schedule->getRegion(0), builder, loc(at));
  return success();
}

// world { statements }: the state the program starts with.
LogicalResult Parser::parseWorld() {
  llvm::SMLoc at = token.loc;
  advance();
  if (!current->worldSchedule.empty())
    return error(at, "a module has one 'world'");
  current->worldSchedule = current->prefix + kWorldSchedule.str();

  OperationState state(loc(at), SystemOp::getOperationName());
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     declareSymbol(at, kWorldSystem));
  state.addRegion()->push_back(new Block());
  Operation *system = builder.create(state);
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(&system->getRegion(0).front());
    ScopeGuard scope(*this);
    llvm::SaveAndRestore<bool> body(inSystem, true);
    if (failed(parseBlock()))
      return failure();
    SystemOp::ensureTerminator(system->getRegion(0), builder, loc(at));
  }

  OperationState scheduleState(loc(at), ScheduleOp::getOperationName());
  scheduleState.addAttribute(SymbolTable::getSymbolAttrName(),
                             declareSymbol(at, kWorldSchedule));
  scheduleState.addRegion()->push_back(new Block());
  scheduleState.addRegion();
  Operation *schedule = builder.create(scheduleState);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(&schedule->getRegion(0).front());
  RunOp::create(builder, loc(at), symbol(kWorldSystem), ValueRange{});
  ScheduleOp::ensureTerminator(schedule->getRegion(0), builder, loc(at));
  return success();
}

// main { schedule(args) | loop { ... } [until cond] ... }
LogicalResult Parser::parseMain() {
  llvm::SMLoc at = token.loc;
  advance();
  if (hasMain)
    return error(at, "a program has one 'main'");
  hasMain = true;
  OperationState state(loc(at), MainOp::getOperationName());
  Region *body = state.addRegion();
  body->push_back(new Block());
  Operation *main = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(&main->getRegion(0).front());
  ScopeGuard scope(*this);
  if (failed(parseMainBlock()))
    return failure();
  MainOp::ensureTerminator(main->getRegion(0), builder, loc(at));
  return success();
}

// { step* } of main or of a loop in it, at the builder's insertion point.
LogicalResult Parser::parseMainBlock() {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  while (!token.is(Token::RBrace)) {
    llvm::SMLoc at = token.loc;
    if (consumeKeyword("loop")) {
      OperationState state(loc(at), LoopOp::getOperationName());
      Region *body = state.addRegion();
      body->push_back(new Block());
      Operation *loop = builder.create(state);
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToEnd(&loop->getRegion(0).front());
      if (failed(parseMainBlock()))
        return failure();
      // The condition is evaluated after the body, in the loop.
      if (consumeKeyword("until")) {
        FailureOr<ExprPtr> condition = parseExpr();
        if (failed(condition))
          return failure();
        FailureOr<mlir::Value> value = emit(**condition, builder.getI1Type());
        if (failed(value))
          return failure();
        if (!value->getType().isInteger(1))
          return error((*condition)->loc, "an 'until' condition must be a "
                                          "bool");
        YieldOp::create(builder, loc((*condition)->loc), ValueRange{*value});
      } else {
        LoopOp::ensureTerminator(loop->getRegion(0), builder, loc(at));
      }
      consumeIf(Token::Semicolon);
      continue;
    }
    FailureOr<std::string> schedule = identifier("a schedule to run or 'loop'");
    if (failed(schedule) || failed(expect(Token::LParen, "'('")))
      return failure();
    auto known = schedules.find(*schedule);
    if (known == schedules.end())
      return error(at, "unknown schedule '" + *schedule +
                           "'; 'main' runs schedules, declared before it");
    SmallVector<mlir::Value> args;
    while (!token.is(Token::RParen)) {
      FailureOr<ExprPtr> arg = parseExpr();
      if (failed(arg))
        return failure();
      Type expected = args.size() < known->second.size()
                          ? known->second[args.size()]
                          : Type();
      FailureOr<mlir::Value> value = emit(**arg, expected);
      if (failed(value))
        return failure();
      args.push_back(*value);
      if (!consumeIf(Token::Comma))
        break;
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    CallOp::create(builder, loc(at), symbol(*schedule), args);
    consumeIf(Token::Semicolon);
  }
  return expect(Token::RBrace, "'}'");
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

// { statement* }, at the builder's insertion point.
LogicalResult Parser::parseBlock(bool ownScope) {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  // (Or in the caller's scope, which outlives the block.)
  std::optional<ScopeGuard> scope;
  if (ownScope)
    scope.emplace(*this);
  if (failed(parseStatements()))
    return failure();
  advance();
  return success();
}

/// The statements of a block, up to its `}`. After one that may have left
/// the loop the block is in (`break`, or an `if` with one in it), the rest
/// run only where it did not.
LogicalResult Parser::parseStatements() {
  while (!token.is(Token::RBrace)) {
    if (token.is(Token::Eof))
      return error("expected '}'");
    mlir::Value flag = breakFlag(), skip = continueFlag();
    llvm::SMLoc at = token.loc;
    if (failed(parseStatement()))
      return failure();
    consumeIf(Token::Semicolon);
    mlir::Value now = breakFlag(), skipped = continueFlag();
    if (!flag || (now == flag && skipped == skip) || token.is(Token::RBrace))
      continue;
    // (What the rest leaves in a var where it runs, or the var as it was.)
    mlir::Value stops = now;
    if (skipped)
      stops = arith::OrIOp::create(builder, loc(at), now, skipped);
    mlir::Value goesOn = arith::XOrIOp::create(
        builder, loc(at), stops,
        arith::ConstantIntOp::create(builder, loc(at), 1, 1));
    auto branch = scf::IfOp::create(builder, loc(at), goesOn,
                                    /*withElseRegion=*/true);
    SmallVector<VarState> before = captureVars();
    SmallVector<VarState> thenVars;
    {
      OpBuilder::InsertionGuard inner(builder);
      builder.setInsertionPoint(branch.thenBlock()->getTerminator());
      // (What the rest declares is its own: the block ends with it.)
      ScopeGuard rest(*this);
      if (failed(parseStatements()))
        return failure();
      thenVars = captureVars();
      thenVars.resize(before.size());
    }
    restoreVars(before);
    branch.getElseRegion().getBlocks().clear();
    mergeBranches(branch, before, thenVars, before);
    return success();
  }
  return success();
}

/// A loop `break` may leave: its flag, a var that is false where the
/// loop starts and that `break` sets, in the scope the caller opened for
/// the loop; the innermost from here on.
std::string Parser::openBreak(Location where) {
  std::string name = " break " + std::to_string(breaks++);
  mlir::Value start = arith::ConstantIntOp::create(builder, where, 0, 1);
  breakStarts[name] = start;
  bind(name, Variable::ofVar(start));
  breakFlags.push_back(name);
  return name;
}

/// The body of a loop `break` may leave, a turn of it: with a flag that
/// `continue` sets, false where the turn starts, after which the rest of
/// the turn is not run (see parseStatements).
LogicalResult Parser::parseLoopBody() {
  ScopeGuard scope(*this);
  std::string name = " continue " + std::to_string(breaks++);
  bind(name, Variable::ofVar(breakStarts.lookup(breakFlags.back())));
  continueFlags.push_back(name);
  llvm::scope_exit pop([&] { continueFlags.pop_back(); });
  return parseBlock();
}

/// What the `continue` flag of the innermost loop's turn holds here; none
/// outside one (or before its body).
mlir::Value Parser::continueFlag() {
  if (breakFlags.size() <= breakFloor ||
      continueFlags.size() != breakFlags.size())
    return mlir::Value();
  const Variable *flag = lookup(continueFlags.back());
  return flag ? flag->value : mlir::Value();
}

/// What the flag of the innermost loop `break` may leave holds here; none
/// outside one.
mlir::Value Parser::breakFlag() {
  if (breakFlags.size() <= breakFloor)
    return mlir::Value();
  const Variable *flag = lookup(breakFlags.back());
  return flag ? flag->value : mlir::Value();
}

void Parser::setVar(StringRef name, mlir::Value value) {
  for (auto &scope : llvm::reverse(scopes)) {
    auto found = scope.find(name);
    if (found != scope.end()) {
      found->second.value = value;
      return;
    }
  }
}

/// Whether `name = Unique[.field] op` follows a `let`, with `op` one of
/// `+=`, `-=`, `min=`, `max=`: a running sum (see parseRunning).
bool Parser::atRunning() {
  if (!token.is(Token::Identifier))
    return false;
  Lexer ahead = lexer;
  if (!ahead.next().is(Token::Assign))
    return false;
  Token unique = ahead.next();
  if (!unique.is(Token::Identifier) || lookup(unique.spelling) ||
      !uniques.count(unique.spelling))
    return false;
  Token next = ahead.next();
  if (next.is(Token::Dot)) {
    if (!ahead.next().is(Token::Identifier))
      return false;
    next = ahead.next();
  }
  if (next.is(Token::PlusAssign) || next.is(Token::MinusAssign))
    return true;
  return (next.isKeyword("min") || next.isKeyword("max")) &&
         ahead.next().is(Token::Assign);
}

// let first = Unique.field += value: accumulated into the unique as in a
// `for`, and `first` what it had come to before this entity's value (by
// the entities before it, in the `for`'s order).
LogicalResult Parser::parseRunning(llvm::SMLoc at) {
  FailureOr<std::string> name = identifier("a name");
  if (failed(name) || failed(expect(Token::Assign, "'='")))
    return failure();
  llvm::SMLoc uniqueAt = token.loc;
  std::string unique = token.spelling.str();
  advance();
  auto record = uniques.find(unique);
  std::string field = "value";
  if (consumeIf(Token::Dot)) {
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> named = identifier("a field");
    if (failed(named))
      return failure();
    field = *named;
    if (!record->second.fieldType(field))
      return error(fieldAt,
                   "unique '" + unique + "' has no field '" + field + "'");
  } else if (!record->second.shorthand) {
    return error(uniqueAt, "unique '" + unique + "' has fields; sum into one "
                           "with '" + unique + ".field'");
  }
  if (!inQuery || inTurn || inFunction)
    return error(at, "a running sum ('let " + *name + " = " + unique +
                         (field == "value" ? "" : "." + field) +
                         " += ..') is for the body of a 'for' over entities, "
                         "where each entity gets what those before it sent; "
                         "elsewhere read and write the unique");
  Type type = record->second.fieldType(field);
  auto op = parseAssignOp();
  if (failed(op))
    return failure();
  StringRef rule = op->second ? *op->second : StringRef("add");
  bool negate = op->first == Token::MinusAssign;
  FailureOr<ExprPtr> value = parseStatementValue();
  if (failed(value))
    return failure();
  FailureOr<mlir::Value> rhs = emit(**value, type);
  if (failed(rhs))
    return failure();
  if (rhs->getType() != type)
    return mismatch((*value)->loc, rhs->getType(), type);
  mlir::Value v = *rhs;
  if (negate) {
    FailureOr<mlir::Value> negated =
        arithmetic(loc(at), Token::Minus, noughtOf(loc(at), type), v);
    if (failed(negated))
      return failure();
    v = *negated;
  }
  auto running = RunningOp::create(builder, loc(at), type, symbol(unique),
                                   builder.getStringAttr(field),
                                   builder.getStringAttr(rule), v);
  bind(*name, Variable::ofValue(running.getBefore()));
  return success();
}

LogicalResult Parser::parseStatement() {
  llvm::SMLoc at = token.loc;
  if (consumeKeyword("let")) {
    if (atRunning())
      return parseRunning(at);
    FailureOr<Let> let = parseLet(/*statement=*/true);
    if (failed(let))
      return failure();
    return emitLet(*let, /*isVar=*/false);
  }
  if (consumeKeyword("var")) {
    // var (a, b) = values: each a var of its own.
    if (token.is(Token::LParen)) {
      FailureOr<Let> let = parseLet(/*statement=*/true);
      if (failed(let))
        return failure();
      return emitLet(*let, /*isVar=*/true);
    }
    return parseVar();
  }
  if (consumeKeyword("for"))
    return parseFor();
  if (consumeKeyword("connect"))
    return parseConnect(at);
  if (consumeKeyword("if"))
    return parseIf();
  if (token.isKeyword("spawn")) {
    FailureOr<ExprPtr> spawn = parsePrimary();
    if (failed(spawn) || failed(emit(**spawn, Type())))
      return failure();
    return success();
  }
  if (consumeKeyword("loop"))
    return parseLoop(at);
  if (consumeKeyword("while"))
    return parseWhile(at);
  // break: out of the innermost counted `for`, `while` or loop over
  // edges; the statements after it are not run.
  if (consumeKeyword("break")) {
    if (breakFlags.size() <= breakFloor)
      return error(at, "'break' leaves a loop: a counted 'for', a 'while' or "
                       "a loop over edges ('for (e)-[R]->(v)'), not a 'for' "
                       "over entities");
    setVar(breakFlags.back(),
           arith::ConstantIntOp::create(builder, loc(at), 1, 1));
    return success();
  }
  // continue: on with the next turn of the innermost counted `for`,
  // `while` or loop over edges; the statements after it in this one are
  // not run.
  if (consumeKeyword("continue")) {
    if (breakFlags.size() <= breakFloor ||
        continueFlags.size() != breakFlags.size())
      return error(at, "'continue' goes on with the next turn of a loop: a "
                       "counted 'for', a 'while' or a loop over edges ('for "
                       "(e)-[R]->(v)'), not a 'for' over entities");
    setVar(continueFlags.back(),
           arith::ConstantIntOp::create(builder, loc(at), 1, 1));
    return success();
  }
  if (token.isKeyword("return"))
    return error("'" + token.spelling + "' is not supported yet");
  // binding.field(args), where the field holds a proc: a call of that
  // one.
  if (token.is(Token::Identifier) && peek().is(Token::Dot)) {
    Lexer ahead = lexer;
    ahead.next();
    Token field = ahead.next();
    Token paren = ahead.next();
    Expr held;
    held.kind = Expr::Field;
    held.loc = at;
    held.name = token.spelling.str();
    held.field = field.spelling.str();
    if (field.is(Token::Identifier) && paren.is(Token::LParen) &&
        field.spelling != "has" && lookup(held.name) &&
        lookup(held.name)->kind == Variable::Ref)
      if (Callable *through = callableOf(typeOf(held))) {
        FailureOr<ExprPtr> call = parsePrimary();
        if (failed(call))
          return failure();
        if (!through->proc)
          return error(at, "'" + held.name + "." + held.field +
                               "' holds a fn: it only gives a value, so "
                               "calling it for nothing does nothing");
        return emitCallThrough(**call, *through);
      }
  }
  // name(args), where the name holds a proc: a call of that one.
  if (token.is(Token::Identifier) && peek().is(Token::LParen))
    if (Callable *through = callableNamed(token.spelling)) {
      FailureOr<ExprPtr> call = parsePrimary();
      if (failed(call))
        return failure();
      if (!through->proc)
        return error(at, "'" + (*call)->name + "' holds a fn: it only gives "
                         "a value, so calling it for nothing does nothing");
      return emitCallThrough(**call, *through);
    }
  // (What a unique index declares: two entities of a key stop the
  // program.)
  if (buildingIndex && token.isKeyword("index_duplicate") &&
      peek().is(Token::LParen)) {
    FailureOr<ExprPtr> call = parsePrimary();
    if (failed(call) || failed(emit(**call, Type())))
      return failure();
    return success();
  }
  // name(args): a call for what it does.
  if (token.is(Token::Identifier) && peek().is(Token::LParen) &&
      functions.count(token.spelling)) {
    FailureOr<ExprPtr> call = parsePrimary();
    if (failed(call))
      return failure();
    Function &function = functions[(*call)->name];
    if (!function.proc)
      return error(at, "'" + (*call)->name + "' is a fn: it only gives a "
                       "value, so calling it for nothing does nothing");
    return emitInvoke(**call);
  }
  if (token.is(Token::Identifier))
    return parseNameStatement();
  return error(at, "expected a statement, found '" + token.spelling + "'");
}

// name[entity].field op value, of a component a proc was given `mut`:
// the field of the entity that has it is read as it is and set; where
// the entity has it not, nothing happens.
LogicalResult Parser::parseGivenStatement(const std::string &name,
                                          llvm::SMLoc at) {
  ComponentUse held = *componentGiven(name);
  if (!held.mut)
    return error(at, "'" + name + "' was given to read: a proc that "
                     "changes its entities is given it 'mut' ('" + name +
                         ": mut " + held.component + "')");
  advance();
  FailureOr<ExprPtr> which = parseExpr();
  if (failed(which) || failed(expect(Token::RBracket, "']'")) ||
      failed(expect(Token::Dot, "'.' and a field")))
    return failure();
  llvm::SMLoc fieldAt = token.loc;
  FailureOr<std::string> field = identifier("a field");
  if (failed(field))
    return failure();
  Type type = components[held.component].fieldType(*field);
  if (!type)
    return error(fieldAt, "component '" + held.component +
                              "' has no field '" + *field + "'");
  if (isa<TextType, StringType>(type))
    return error(fieldAt, "a text of another entity is not set here yet");
  auto op = parseAssignOp();
  if (failed(op))
    return failure();
  FailureOr<ExprPtr> value = parseExpr();
  if (failed(value))
    return failure();
  FailureOr<mlir::Value> entity = emit(**which, EntityType::get(context));
  if (failed(entity))
    return failure();
  if (!isa<EntityType>(entity->getType()))
    return error((*which)->loc, "an entity says which: '" + name +
                                    "[entity].field'");
  StringAttr fieldName = builder.getStringAttr(*field);
  FlatSymbolRefAttr component = symbol(held.component);
  return emitAssignment(
      at, op->first, op->second, [&] { return type; },
      [&]() -> mlir::Value {
        return LookupOp::create(builder, loc(at), type, builder.getI1Type(),
                                *entity, component, fieldName)
            .getValue();
      },
      [&](mlir::Value v) -> LogicalResult {
        ApplyOp::create(builder, loc(at), *entity, component, fieldName,
                        builder.getStringAttr("set"), v);
        return success();
      },
      **value);
}

// for (e)-[s: R]->(other) { } / for (child: C)-[R]->(e) { }, inside a
// `for`: the edges of the entity it visits, out of it or into it.
/// What a `for` over entities that is now whole must hold of the `for`s
/// inside it, which can only be told when all of its body is there: none
/// reads a field the outer one sets of its own entities (unless as it
/// was before: `old`), and what one sends or accumulates nothing else in
/// the outer one touches. (As EachOp's verifier says of the IR; here
/// with the program's words, at the program's lines.)
LogicalResult Parser::checkEaches(Operation *query) {
  LogicalResult result = success();
  auto report = [&](Operation *at, const Twine &message, Operation *other,
                    StringRef note) {
    if (failed(result))
      return;
    InFlightDiagnostic diag = emitError(at->getLoc()) << message;
    diag.attachNote(other->getLoc()) << note;
    hadError = true;
    result = failure();
  };
  Block &outer = query->getRegion(0).front();
  query->walk([&](EachOp each) {
    Block &body = each.getBody().front();
    for (BlockArgument arg : body.getArguments().take_front(
             each.getNumRefs())) {
      if (each.isOld(arg.getArgNumber()))
        continue;
      FlatSymbolRefAttr component =
          cast<RefType>(arg.getType()).getComponent();
      llvm::SmallPtrSet<Attribute, 4> read;
      for (Operation *user : arg.getUsers())
        if (auto get = dyn_cast<GetOp>(user))
          read.insert(get.getFieldAttr());
      query->walk([&](SetOp set) {
        auto written = dyn_cast<BlockArgument>(set.getRef());
        if (!written || written.getOwner() != &outer ||
            cast<RefType>(set.getRef().getType()).getComponent() !=
                component ||
            !read.contains(set.getFieldAttr()))
          return;
        StringRef name = component.getValue();
        name = name.substr(name.rfind('.') + 1);
        report(each,
               "this 'for' reads '" + set.getFieldAttr().getValue() +
                   "' of '" + name + "' of other entities, which the 'for' "
                   "around sets of its own: some would be set already and "
                   "some not. Bind it 'old' ('name: old " + name +
                   "') to read what it was before that 'for' started (a "
                   "copy is kept for it), or set another field and copy it "
                   "over after",
               set, "set here");
      });
    }
    each.getBody().walk([&](Operation *op) {
      if (auto apply = dyn_cast<ApplyOp>(op)) {
        query->walk([&](Operation *other) {
          Value ref;
          StringAttr field;
          if (auto get = dyn_cast<GetOp>(other))
            ref = get.getRef(), field = get.getFieldAttr();
          else if (auto set = dyn_cast<SetOp>(other))
            ref = set.getRef(), field = set.getFieldAttr();
          bool touches =
              ref && field == apply.getFieldAttr() &&
              cast<RefType>(ref.getType()).getComponent() ==
                  apply.getComponentAttr();
          if (auto lookup = dyn_cast<LookupOp>(other))
            touches = lookup.getComponentAttr() == apply.getComponentAttr() &&
                      lookup.getFieldAttr() == apply.getFieldAttr();
          if (!touches)
            return;
          StringRef name = apply.getComponentAttr().getValue();
          name = name.substr(name.rfind('.') + 1);
          report(op,
                 "what a 'for' inside a 'for' sends to a field lands as it "
                 "goes, so the 'for' around does not read or set that "
                 "field ('" + apply.getFieldAttr().getValue() + "' of '" +
                     name + "') too",
                 other, "it does here");
        });
      } else if (auto accumulate = dyn_cast<AccumulateOp>(op)) {
        query->walk([&](ReadOp read) {
          if (read.getResourceAttr() != accumulate.getResourceAttr() ||
              read.getFieldAttr() != accumulate.getFieldAttr())
            return;
          StringRef name = accumulate.getResourceAttr().getValue();
          name = name.substr(name.rfind('.') + 1);
          report(op,
                 "what a 'for' inside a 'for' accumulates lands as it goes, "
                 "so the 'for' around does not read that unique ('" + name +
                     "') too",
                 read, "it is read here");
        });
      }
    });
  });
  return result;
}

// for [name,] [binding: Component, ...] [with A, B] [without C] [where c]
// { statements }, inside a `for` over entities: the statements for every
// entity that has those, for the entity the outer one visits. It reads
// them; what it finds it leaves in the vars of the body it is in.
LogicalResult Parser::parseEach(llvm::SMLoc at) {
  if (inEach)
    return error(at, "a 'for' over entities is one deep in another: this "
                     "one is already inside one that is");
  if (inEdges)
    return error(at, "a 'for' over entities cannot be inside a 'for' over "
                     "edges");
  struct Bound {
    std::string name, component;
    bool old;
  };
  SmallVector<Bound> bindings;
  std::string entity;
  auto filter = [&] {
    return token.isKeyword("with") || token.isKeyword("without") ||
           token.isKeyword("where") || token.is(Token::LBrace);
  };
  auto known = [&](llvm::SMLoc where,
                   StringRef name) -> LogicalResult {
    if (!components.count(name))
      return error(where, "unknown component '" + name + "'");
    return success();
  };
  // (In a fn or proc: `[e,] c in name`, of a component it was given.)
  if (inFunction) {
    FailureOr<std::string> first = identifier("a name");
    if (failed(first))
      return failure();
    std::string binding = *first;
    if (consumeIf(Token::Comma)) {
      entity = *first;
      FailureOr<std::string> second = identifier("a name");
      if (failed(second))
        return failure();
      binding = *second;
    }
    if (failed(expectKeyword("in")))
      return failure();
    FailureOr<std::string> over = identifier("a component that was given");
    if (failed(over))
      return failure();
    bindings.push_back({binding, componentGiven(*over)->component, false});
  }
  while (!inFunction && !filter()) {
    if (token.isKeyword("optional") || token.isKeyword("on") ||
        token.isKeyword("top") || token.isKeyword("bottom"))
      return error("a 'for' inside a 'for' visits every entity that has "
                   "what it binds: no 'optional', 'on' or order");
    llvm::SMLoc nameAt = token.loc;
    FailureOr<std::string> name = identifier("a name");
    if (failed(name))
      return failure();
    if (!consumeIf(Token::Colon)) {
      // The entity's name: first, and once.
      if (!entity.empty() || !bindings.empty())
        return error(nameAt, "expected '" + *name + ": Component'");
      entity = *name;
      if (!consumeIf(Token::Comma) && !filter())
        return error("expected ',' and a binding, a filter or '{'");
      continue;
    }
    if (token.isKeyword("mut"))
      return error("a 'for' inside a 'for' reads its entities; what is to "
                   "change of one is sent to it after the loop "
                   "('Component(entity).field += value')");
    // `old`: as it was before the `for` around started.
    bool old = false;
    if (token.isKeyword("old") && peek().is(Token::Identifier)) {
      old = true;
      advance();
    }
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component) || failed(known(componentAt, *component)))
      return failure();
    bindings.push_back({*name, *component, old});
    if (!consumeIf(Token::Comma))
      break;
  }
  SmallVector<Attribute> with, without;
  while (token.isKeyword("with") || token.isKeyword("without")) {
    SmallVector<Attribute> &into = token.isKeyword("with") ? with : without;
    advance();
    do {
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component) || failed(known(componentAt, *component)))
        return failure();
      into.push_back(symbol(*component));
    } while (consumeIf(Token::Comma));
  }
  // `near (x, y) within r in Index`, `between (..) and (..) in Index`,
  // `nearest k to (x, y) in Index`: the entities of a grid index near a
  // place, in a box, or nearest it.
  if (token.isKeyword("near") || token.isKeyword("between") ||
      token.isKeyword("nearest")) {
    std::string form = "'for ... " + token.spelling.str() + "'";
    if (inFunction)
      return error("a fn or proc has no index: a " + form +
                   " is in a "
                   "system's 'for'");
    if (!with.empty() || !without.empty())
      return error("a " + form +
                   " visits the entities of an index, "
                   "which have its components: no 'with' or 'without'");
    SmallVector<std::pair<std::string, std::string>> bound;
    for (const Bound &binding : bindings) {
      if (binding.old)
        return error(at, "a " + form +
                             " reads the index as it was made, "
                             "before the 'for' around started: no 'old'");
      bound.push_back({binding.name, binding.component});
    }
    return parseNear(at, entity, bound);
  }
  if (bindings.empty() && with.empty())
    return error(at, "a 'for' inside a 'for' binds a component, or says "
                     "what its entities have ('with')");
  ExprPtr condition;
  if (consumeKeyword("where")) {
    FailureOr<ExprPtr> parsed = parseExpr();
    if (failed(parsed))
      return failure();
    condition = std::move(*parsed);
  }
  if (!token.is(Token::LBrace))
    return error("expected '{' and the statements to run for each entity");

  // Every var goes round: what one entity leaves in it the next starts
  // with, and the loop gives what the last one left.
  Location where = loc(at);
  SmallVector<VarState> vars = captureVars();
  OperationState state(where, EachOp::getOperationName());
  for (const VarState &var : vars) {
    state.addOperands(var.value);
    state.addTypes(var.value.getType());
  }
  if (!with.empty())
    state.addAttribute("with", builder.getArrayAttr(with));
  if (!without.empty())
    state.addAttribute("without", builder.getArrayAttr(without));
  if (!entity.empty())
    state.addAttribute("entity", builder.getUnitAttr());
  SmallVector<int64_t> old;
  for (auto [number, bound] : llvm::enumerate(bindings))
    if (bound.old)
      old.push_back(number);
  if (!old.empty())
    state.addAttribute("old", builder.getI64ArrayAttr(old));
  auto *block = new Block();
  state.addRegion()->push_back(block);
  for (const Bound &bound : bindings)
    block->addArgument(RefType::get(context, symbol(bound.component), false),
                       where);
  if (!entity.empty())
    block->addArgument(EntityType::get(context), where);
  for (const VarState &var : vars)
    block->addArgument(var.value.getType(), where);
  Operation *each = builder.create(state);
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(block);
    ScopeGuard scope(*this);
    llvm::SaveAndRestore inside(inEach, true);
    llvm::SaveAndRestore floor(breakFloor, breakFlags.size());
    unsigned next = 0;
    for (const Bound &bound : bindings)
      bind(bound.name, {Variable::Ref, block->getArgument(next++),
                        bound.component, false});
    // (A value: `has`, `destroy` and the like are for the entity the
    // outer `for` visits.)
    if (!entity.empty())
      bind(entity, Variable::ofValue(block->getArgument(next++)));
    SmallVector<VarState> round = vars;
    for (VarState &var : round)
      var.value = block->getArgument(next++);
    restoreVars(round);
    if (condition) {
      FailureOr<mlir::Value> holds = emit(*condition, builder.getI1Type());
      if (failed(holds))
        return failure();
      if (!holds->getType().isInteger(1))
        return error(condition->loc, "'where' takes a bool");
      auto branch = scf::IfOp::create(builder, where, *holds,
                                      /*withElseRegion=*/true);
      SmallVector<VarState> before = captureVars();
      {
        OpBuilder::InsertionGuard inner(builder);
        builder.setInsertionPoint(branch.thenBlock()->getTerminator());
        if (failed(parseBlock()))
          return failure();
      }
      SmallVector<VarState> thenVars = captureVars();
      restoreVars(before);
      branch.getElseRegion().getBlocks().clear();
      mergeBranches(branch, before, thenVars, before);
    } else if (failed(parseBlock())) {
      return failure();
    }
    SmallVector<mlir::Value> ends;
    for (const VarState &var : captureVars())
      if (ends.size() < vars.size())
        ends.push_back(var.value);
    YieldOp::create(builder, where, ends);
  }
  for (auto [var, result] : llvm::zip(vars, each->getResults()))
    var.value = result;
  restoreVars(vars);
  return success();
}

// A loop from `lower` to `upper` (indexes), the vars assigned in it
// going round, as in a counted `for`: `body` is given the count.
LogicalResult
Parser::loopWithVars(Location where, mlir::Value lower, mlir::Value upper,
                     function_ref<LogicalResult(mlir::Value)> body) {
  mlir::Value step = arith::ConstantIndexOp::create(builder, where, 1);
  auto loop = scf::ForOp::create(builder, where, lower, upper, step);
  SmallVector<VarState> before = captureVars(), after;
  SmallVector<Operation *> placeholders;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(loop.getBody()->getTerminator());
    ScopeGuard scope(*this);
    SmallVector<VarState> inside = before;
    for (VarState &state : inside) {
      auto placeholder = UnrealizedConversionCastOp::create(
          builder, where, state.value.getType(), state.value);
      placeholders.push_back(placeholder);
      state.value = placeholder.getResult(0);
    }
    restoreVars(inside);
    if (failed(body(loop.getInductionVar())))
      return failure();
    after = captureVars();
  }
  restoreVars(before);
  SmallVector<unsigned> carried;
  for (auto [i, state] : llvm::enumerate(after))
    if (state.value != placeholders[i]->getResult(0))
      carried.push_back(i);
  if (!carried.empty()) {
    SmallVector<mlir::Value> inits, ends;
    for (unsigned i : carried) {
      inits.push_back(before[i].value);
      ends.push_back(after[i].value);
    }
    auto carrying =
        scf::ForOp::create(builder, where, lower, upper, step, inits);
    Block *from = loop.getBody(), *to = carrying.getBody();
    from->getTerminator()->erase();
    loop.getInductionVar().replaceAllUsesWith(carrying.getInductionVar());
    to->getOperations().splice(to->end(), from->getOperations());
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToEnd(to);
      scf::YieldOp::create(builder, where, ends);
    }
    loop.erase();
    for (auto [k, i] : llvm::enumerate(carried)) {
      placeholders[i]->getResult(0).replaceAllUsesWith(
          carrying.getRegionIterArg(k));
      before[i].value = carrying.getResult(k);
    }
    restoreVars(before);
  }
  for (Operation *placeholder : placeholders) {
    placeholder->getResult(0).replaceAllUsesWith(placeholder->getOperand(0));
    placeholder->erase();
  }
  return success();
}

// `body` where `holds`, the vars it assigns as it left them there and as
// they were where it does not hold.
LogicalResult Parser::ifWithVars(Location where, mlir::Value holds,
                                 function_ref<LogicalResult()> body) {
  auto branch =
      scf::IfOp::create(builder, where, holds, /*withElseRegion=*/true);
  SmallVector<VarState> before = captureVars();
  {
    OpBuilder::InsertionGuard inner(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    if (failed(body()))
      return failure();
  }
  SmallVector<VarState> thenVars = captureVars();
  restoreVars(before);
  branch.getElseRegion().getBlocks().clear();
  mergeBranches(branch, before, thenVars, before);
  return success();
}

// `body` again and again while `condition` holds (asked before each
// round), every var going round as in a `while`.
LogicalResult Parser::whileWithVars(Location where,
                                    function_ref<mlir::Value()> condition,
                                    function_ref<LogicalResult()> body) {
  SmallVector<VarState> vars = captureVars();
  SmallVector<mlir::Value> starts;
  SmallVector<Type> types;
  for (const VarState &state : vars) {
    starts.push_back(state.value);
    types.push_back(state.value.getType());
  }
  auto bindTo = [&](ValueRange values) {
    SmallVector<VarState> inside = vars;
    for (auto [state, value] : llvm::zip(inside, values))
      state.value = value;
    restoreVars(inside);
  };
  auto loop = scf::WhileOp::create(builder, where, types, starts);
  LogicalResult made = success();
  {
    OpBuilder::InsertionGuard guard(builder);
    SmallVector<Location> locations(types.size(), where);
    Block *ask = builder.createBlock(&loop.getBefore(), {}, types, locations);
    bindTo(ask->getArguments());
    scf::ConditionOp::create(builder, where, condition(), ask->getArguments());
    Block *round = builder.createBlock(&loop.getAfter(), {}, types, locations);
    bindTo(round->getArguments());
    SmallVector<mlir::Value> ends(round->getArguments());
    if (failed(body())) {
      made = failure();
    } else {
      ends.clear();
      for (const VarState &state : captureVars())
        if (ends.size() < vars.size())
          ends.push_back(state.value);
    }
    scf::YieldOp::create(builder, where, ends);
  }
  for (auto [state, result] : llvm::zip(vars, loop.getResults()))
    state.value = result;
  restoreVars(vars);
  return made;
}

// Whether an op of the statements in a `near` loop only reads, or sets
// vars (which go round as values): such statements LLVM does many rows
// at once as they are.
static bool onlyReads(Operation *op) {
  if (isa<arith::ArithDialect, math::MathDialect>(op->getDialect()))
    return true;
  if (auto invoke = dyn_cast<InvokeOp>(op))
    return !invoke.getProc();
  return isa<scf::IfOp, scf::ForOp, scf::YieldOp, BufferAtOp, BufferLenOp,
             GetOp, ReadOp, LookupOp, TableAtOp, SameOp, NobodyOp, HasOp,
             EntityOp, RowsStartOp, RowsCountOp, UnrealizedConversionCastOp>(
      op);
}

// for [e,] b: Component, .. near (x, y[, z]) within r in Index [where c] { }
// for [e,] b: Component, .. between (x0, y0) and (x1, y1) in Index ..
// for [e,] b: Component, .. nearest k to (x, y) [within r] in Index ..
//
// The entities of a grid index within `r` of a place, in the order of the
// cells around the place's (as many out as `r` reaches: z, then y, then
// x, round the box where it wraps) and in each cell of its rows. A
// field of where they are reads as seen from the place (the image
// nearest it, where the box wraps), and the distance is of that; what
// the index copies is read from its row; anything else of the bound
// components is looked up. The vars go round as in any loop.
//
// `between`: those from one corner to the other on every side, the same
// order (seen at the image from the lowest corner on). `nearest k`: the k
// nearest the place, nearest first (by their rows where as near), of those its
// `where` takes: those within a distance that doubles, from a cell's
// width, until k are, kept as a list in order; then visited.
LogicalResult
Parser::parseNear(llvm::SMLoc at, const std::string &entity,
                  ArrayRef<std::pair<std::string, std::string>> bound) {
  Location where = loc(at);
  enum class Kind { Near, Between, Nearest };
  Kind kind = token.isKeyword("between")   ? Kind::Between
              : token.isKeyword("nearest") ? Kind::Nearest
                                           : Kind::Near;
  std::string form = kind == Kind::Near ? "'near (x, y) within r in Index'"
                     : kind == Kind::Between
                         ? "'between (x0, y0) and (x1, y1) in Index'"
                         : "'nearest k to (x, y) in Index'";
  advance(); // near, between or nearest
  auto places = [&](SmallVector<ExprPtr> &into,
                    StringRef what) -> LogicalResult {
    if (failed(expect(Token::LParen, "'(' and " + what.str() + ": " + form)))
      return failure();
    do {
      FailureOr<ExprPtr> coordinate = parseExpr();
      if (failed(coordinate))
        return failure();
      into.push_back(std::move(*coordinate));
    } while (consumeIf(Token::Comma));
    return expect(Token::RParen, "')'");
  };
  SmallVector<ExprPtr> place, high;
  ExprPtr radius, count;
  if (kind == Kind::Near) {
    if (failed(places(place, "the place")))
      return failure();
    if (!consumeKeyword("within"))
      return error("expected 'within' and how far: " + form);
    FailureOr<ExprPtr> parsed = parseExpr();
    if (failed(parsed))
      return failure();
    radius = std::move(*parsed);
  } else if (kind == Kind::Between) {
    if (failed(places(place, "the lowest corner")))
      return failure();
    if (!consumeKeyword("and"))
      return error("expected 'and' and the highest corner: " + form);
    if (failed(places(high, "the highest corner")))
      return failure();
  } else {
    FailureOr<ExprPtr> parsed = parseExpr();
    if (failed(parsed))
      return failure();
    count = std::move(*parsed);
    if (!consumeKeyword("to"))
      return error("expected 'to' and the place: " + form);
    if (failed(places(place, "the place")))
      return failure();
    if (consumeKeyword("within")) {
      FailureOr<ExprPtr> farthest = parseExpr();
      if (failed(farthest))
        return failure();
      radius = std::move(*farthest);
    }
  }
  if (!consumeKeyword("in"))
    return error("expected 'in' and the grid index: " + form);
  llvm::SMLoc indexAt = token.loc;
  FailureOr<std::string> named = identifier("an index");
  if (failed(named))
    return failure();
  std::optional<BufferUse> held = bufferNamed(*named);
  if (!held || !indexes.count(held->symbol) ||
      indexes[held->symbol].positions.empty())
    return error(indexAt, "'" + *named +
                              "' is not an index of a grid "
                              "('index " +
                              *named +
                              " of C grid (x, y) cell w') "
                              "or a Morton index ('index " +
                              *named + " of C morton (x, y)')");
  std::string symbolName = held->symbol;
  Index &index = indexes[symbolName];
  unsigned dims = index.positions.size();
  if (place.size() != dims || (kind == Kind::Between && high.size() != dims))
    return error(at,
                 "'" + *named + "' is a grid of " + Twine(dims) +
                     " places; the place is given " +
                     Twine(place.size() != dims ? place.size() : high.size()));
  for (auto &[name, component] : bound)
    if (!llvm::is_contained(index.of, resolve(component)))
      return error(at, "the entities of '" + *named + "' have its "
                       "components; '" + component + "' is not one");
  // `after me`: only those whose row in the index is after the row of
  // the entity the `for` around visits, read through its binding of the
  // index's first component.
  mlir::Value afterRow;
  if (token.isKeyword("after") && kind == Kind::Nearest)
    return error("'nearest' visits the nearest first, not by rows: no "
                 "'after'");
  if (consumeKeyword("after")) {
    llvm::SMLoc meAt = token.loc;
    FailureOr<std::string> me =
        identifier("the entity the 'for' around visits");
    if (failed(me))
      return failure();
    const Variable *visited = lookup(*me);
    if (!visited || visited->kind != Variable::Entity)
      return error(meAt, "'after' takes the entity the 'for' around "
                         "visits, by its name: 'for me, a: " +
                             index.of.front() + " { for o, b: " +
                             index.of.front() + " near (..) within r in " +
                             *named + " after me { .. } }'");
    mlir::Value ref;
    for (auto scope = scopes.rbegin(); scope != scopes.rend() && !ref;
         ++scope)
      for (auto &[name, variable] : *scope)
        if (variable.kind == Variable::Ref &&
            resolve(variable.component) == index.of.front()) {
          ref = variable.value;
          break;
        }
    if (!ref)
      return error(meAt, "the 'for' around binds no '" + index.of.front() +
                             "', by which '" + *me + "' has a row in '" +
                             *named + "'");
    afterRow = GetOp::create(builder, loc(meAt), builder.getI32Type(), ref,
                             builder.getStringAttr(index.rowField))
                   .getResult();
  }
  ExprPtr condition;
  if (consumeKeyword("where")) {
    FailureOr<ExprPtr> parsed = parseExpr();
    if (failed(parsed))
      return failure();
    condition = std::move(*parsed);
  }
  if (!token.is(Token::LBrace))
    return error("expected '{' and the statements to run for each entity");

  Type t = index.positionType;
  Type i32 = builder.getI32Type();
  Type i64 = builder.getI64Type();
  Type indexType = builder.getIndexType();
  auto emitPlace = [&](ArrayRef<ExprPtr> given,
                       SmallVector<mlir::Value> &into) -> LogicalResult {
    for (const ExprPtr &coordinate : given) {
      FailureOr<mlir::Value> value = emit(*coordinate, t);
      if (failed(value))
        return failure();
      if (value->getType() != t)
        return mismatch(coordinate->loc, value->getType(), t);
      into.push_back(*value);
    }
    return success();
  };
  SmallVector<mlir::Value> at_, hi_;
  if (failed(emitPlace(place, at_)) || failed(emitPlace(high, hi_)))
    return failure();
  mlir::Value given;
  if (radius) {
    FailureOr<mlir::Value> value = emit(*radius, t);
    if (failed(value))
      return failure();
    if (value->getType() != t)
      return mismatch(radius->loc, value->getType(), t);
    given = *value;
  }
  mlir::Value wanted;
  if (count) {
    FailureOr<mlir::Value> value = emit(*count, i32);
    if (failed(value))
      return failure();
    if (value->getType() != i32)
      return mismatch(count->loc, value->getType(), i32);
    wanted = *value;
  }
  // How far things are, squared: of the places' type, or, of a grid of
  // whole numbers, an i64.
  bool whole = isa<IntegerType>(t);
  Type distanceType = whole ? i64 : t;
  auto wide = [&](mlir::Value v) -> mlir::Value {
    return whole && v.getType() != i64
               ? arith::ExtSIOp::create(builder, where, i64, v).getResult()
               : v;
  };
  auto minus = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    if (whole)
      return arith::SubIOp::create(builder, where, wide(a), wide(b));
    return arith::SubFOp::create(builder, where, a, b);
  };
  auto plus = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    if (whole)
      return arith::AddIOp::create(builder, where, a, b);
    return arith::AddFOp::create(builder, where, a, b);
  };
  auto squared = [&](mlir::Value a) -> mlir::Value {
    if (whole)
      return arith::MulIOp::create(builder, where, wide(a), wide(a));
    return arith::MulFOp::create(builder, where, a, a);
  };
  auto compare = [&](arith::CmpIPredicate whole_, arith::CmpFPredicate real,
                     mlir::Value a, mlir::Value b) -> mlir::Value {
    if (isa<IntegerType>(a.getType()))
      return arith::CmpIOp::create(builder, where, whole_, a, b);
    return arith::CmpFOp::create(builder, where, real, a, b);
  };
  auto atMost = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    return compare(arith::CmpIPredicate::sle, arith::CmpFPredicate::OLE, a, b);
  };
  auto below = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    return compare(arith::CmpIPredicate::slt, arith::CmpFPredicate::OLT, a, b);
  };
  auto both = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    return a ? arith::AndIOp::create(builder, where, a, b).getResult() : b;
  };
  auto zeroOf = [&](Type type) -> mlir::Value {
    if (whole)
      return integer(where, type, 0);
    return arith::ConstantOp::create(builder, where,
                                     builder.getFloatAttr(type, 0.0));
  };
  auto grid = FlatSymbolRefAttr::get(context, index.gridUnique);
  auto read = [&](StringRef field, Type type) -> mlir::Value {
    return ReadOp::create(builder, where, type, grid,
                          builder.getStringAttr(field))
        .getResult();
  };
  auto call = [&](StringRef fn, ValueRange args,
                  Type type = Type()) -> mlir::Value {
    return InvokeOp::create(
               builder, where, type ? type : i64,
               FlatSymbolRefAttr::get(context, (symbolName + fn).str()), args,
               UnitAttr())
        ->getResult(0);
  };
  mlir::Value size = index.wrap ? read("size", t) : mlir::Value();
  SmallVector<mlir::Value> sides, starts, widths;
  for (unsigned d = 0; d < dims && !index.morton; ++d) {
    std::string number = std::to_string(d);
    sides.push_back(read("n" + number, i64));
    widths.push_back(read("w" + number, t));
    starts.push_back(read("lo" + number, t));
  }
  // Of `between`, where the box wraps: the corners moved by a whole number
  // of boxes so that the lowest is in the box (`shift` how far); each
  // entity is seen at its image from the lowest corner on, less than a
  // box past it.
  SmallVector<mlir::Value> lowCorner(at_), highCorner(hi_), shift;
  if (kind == Kind::Between && index.wrap)
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value boxes;
      if (whole) {
        mlir::Value q = arith::DivSIOp::create(builder, where, at_[d], size);
        mlir::Value under = arith::CmpIOp::create(
            builder, where, arith::CmpIPredicate::sgt,
            arith::MulIOp::create(builder, where, q, size), at_[d]);
        boxes = arith::SubIOp::create(
            builder, where, q,
            arith::ExtUIOp::create(builder, where, t, under));
        boxes = arith::MulIOp::create(builder, where, boxes, size);
      } else {
        boxes = arith::MulFOp::create(
            builder, where,
            math::FloorOp::create(
                builder, where,
                arith::DivFOp::create(builder, where, at_[d], size)),
            size);
      }
      shift.push_back(boxes);
      lowCorner[d] =
          whole
              ? arith::SubIOp::create(builder, where, at_[d], boxes).getResult()
              : arith::SubFOp::create(builder, where, at_[d], boxes)
                    .getResult();
      highCorner[d] =
          whole
              ? arith::SubIOp::create(builder, where, hi_[d], boxes).getResult()
              : arith::SubFOp::create(builder, where, hi_[d], boxes)
                    .getResult();
    }
  // The cells each side's walk goes over (`from`, and how `many`), and,
  // of `between`, the first and last cell of the corners on each side,
  // counted on from the first (not round the box).
  SmallVector<mlir::Value> from(dims), many(dims), lowCell(dims),
      highCell(dims);
  // The reach of a distance: the cells around the place's as far as it
  // reaches (`near`, `nearest`).
  mlir::Value reach, far;
  auto aim = [&](mlir::Value distance) {
    reach = distance;
    far = squared(distance);
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value own = call("_cell", {at_[d], starts[d], widths[d], sides[d]});
      mlir::Value out = call("_reach", {distance, widths[d]});
      from[d] = call("_from", {own, out, sides[d]});
      many[d] = call("_many", {own, out, sides[d]});
    }
  };
  if (kind == Kind::Near && !index.morton)
    aim(given);
  // (Of a Morton index: how far the rows within `r` are at most, squared,
  // which its walk goes by too.)
  if (kind == Kind::Near && index.morton)
    far = squared(given);
  if (kind == Kind::Between && !index.morton)
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value none = zeroOf(t);
      mlir::Value l = call("_low", {lowCorner[d], none, starts[d], widths[d]});
      mlir::Value h =
          call("_high", {highCorner[d], none, starts[d], widths[d]});
      mlir::Value one = integer(where, i64, 1);
      if (!index.wrap) {
        l = arith::MaxSIOp::create(builder, where, l, integer(where, i64, 0));
        h = arith::MinSIOp::create(
            builder, where, h,
            arith::SubIOp::create(builder, where, sides[d], one));
      }
      lowCell[d] = l;
      highCell[d] = h;
      from[d] = l;
      mlir::Value span = arith::AddIOp::create(
          builder, where, arith::SubIOp::create(builder, where, h, l), one);
      many[d] = arith::MaxSIOp::create(
          builder, where, integer(where, i64, 0),
          arith::MinSIOp::create(builder, where, span, sides[d]));
    }
  auto cells = FlatSymbolRefAttr::get(context, index.cells);
  auto rows = FlatSymbolRefAttr::get(context, symbolName);
  mlir::Value zero = arith::ConstantIndexOp::create(builder, where, 0);
  mlir::Value two = arith::ConstantIndexOp::create(builder, where, 2);
  SmallVector<mlir::Value> cell(dims);
  auto rowOf = [&](mlir::Value counted) -> mlir::Value {
    return asIndex(where,
                   arith::IndexCastOp::create(builder, where, i32, counted),
                   IndexOfType::get(context, rows));
  };
  // Where a row's entity is seen from the place (or, of `between`, the
  // image in the corners), and whether it is in what the form takes; of
  // a distance, how far it is, squared, too.
  auto seenAt = [&](mlir::Value row, NearRows &near,
                    mlir::Value *distanceOut = nullptr) -> mlir::Value {
    mlir::Value distance, inside;
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value stored =
          BufferAtOp::create(builder, where, t, rows,
                             builder.getStringAttr(index.positions[d]), row);
      if (kind == Kind::Between) {
        mlir::Value seen = stored;
        if (index.wrap) {
          auto add = [&](mlir::Value a, mlir::Value b, bool less) {
            if (whole)
              return less ? arith::SubIOp::create(builder, where, a, b)
                                .getResult()
                          : arith::AddIOp::create(builder, where, a, b)
                                .getResult();
            return less
                       ? arith::SubFOp::create(builder, where, a, b).getResult()
                       : arith::AddFOp::create(builder, where, a, b)
                             .getResult();
          };
          seen =
              arith::SelectOp::create(builder, where, below(seen, lowCorner[d]),
                                      add(seen, size, false), seen);
          seen = arith::SelectOp::create(
              builder, where, atMost(add(lowCorner[d], size, false), seen),
              add(seen, size, true), seen);
        }
        inside = both(inside, atMost(lowCorner[d], seen));
        inside = both(inside, atMost(seen, highCorner[d]));
        if (index.wrap)
          seen = whole ? arith::AddIOp::create(builder, where, seen, shift[d])
                             .getResult()
                       : arith::AddFOp::create(builder, where, seen, shift[d])
                             .getResult();
        near.seen.push_back({index.component, index.positions[d], seen});
        continue;
      }
      mlir::Value seen =
          index.wrap ? call("_image", {stored, at_[d], size}, t) : stored;
      near.seen.push_back({index.component, index.positions[d], seen});
      mlir::Value square = squared(minus(seen, at_[d]));
      distance = distance ? plus(distance, square) : square;
    }
    if (distanceOut)
      *distanceOut = distance;
    if (kind == Kind::Between)
      return inside;
    return far ? atMost(distance, far) : mlir::Value();
  };
  // The statements for the entity of a row the form takes (its `where`
  // too, unless that was asked already).
  auto visit = [&](mlir::Value row, const NearRows &near,
                   bool asked = false) -> LogicalResult {
    mlir::Value who =
        BufferAtOp::create(builder, where, EntityType::get(context), rows,
                           builder.getStringAttr("who"), row);
    ScopeGuard scope(*this);
    llvm::SaveAndRestore inside(inEach, true);
    llvm::SaveAndRestore floor(breakFloor, breakFlags.size());
    std::optional<NearRows> outer = nearRows;
    nearRows = near;
    auto restore = llvm::make_scope_exit([&] { nearRows = outer; });
    for (auto &[name, component] : bound) {
      Variable variable{Variable::Near, who, component, false};
      variable.which = row;
      bind(name, variable);
    }
    if (!entity.empty())
      bind(entity, Variable::ofValue(who));
    if (!condition || asked)
      return parseBlock();
    FailureOr<mlir::Value> holds = emit(*condition, builder.getI1Type());
    if (failed(holds))
      return failure();
    if (!holds->getType().isInteger(1))
      return error(condition->loc, "'where' takes a bool");
    return ifWithVars(where, *holds, [&] { return parseBlock(); });
  };
  // Whether the entity of a row is one the `where` takes (true without
  // one), with what it reads of it bound as the statements have it.
  auto takes = [&](mlir::Value row,
                   const NearRows &near) -> FailureOr<mlir::Value> {
    if (!condition)
      return mlir::Value(arith::ConstantIntOp::create(builder, where, 1, 1));
    mlir::Value who =
        BufferAtOp::create(builder, where, EntityType::get(context), rows,
                           builder.getStringAttr("who"), row);
    ScopeGuard scope(*this);
    llvm::SaveAndRestore inside(inEach, true);
    llvm::SaveAndRestore floor(breakFloor, breakFlags.size());
    std::optional<NearRows> outer = nearRows;
    nearRows = near;
    auto restore = llvm::make_scope_exit([&] { nearRows = outer; });
    for (auto &[name, component] : bound) {
      Variable variable{Variable::Near, who, component, false};
      variable.which = row;
      bind(name, variable);
    }
    if (!entity.empty())
      bind(entity, Variable::ofValue(who));
    FailureOr<mlir::Value> holds = emit(*condition, builder.getI1Type());
    if (failed(holds))
      return failure();
    if (!holds->getType().isInteger(1))
      return error(condition->loc, "'where' takes a bool");
    return *holds;
  };
  // What is done for a row the form may take (`plain`: the statements
  // asked of each row; else `found`, given the row, whether the form
  // takes it and how far it is).
  using Found = function_ref<LogicalResult(mlir::Value row, mlir::Value counted,
                                           mlir::Value in, mlir::Value distance,
                                           NearRows & near)>;
  // The rows of the cells along the first side that the form reaches,
  // where the other sides' cells are `cell` (`outer` how far, at least,
  // those are, squared): those cells as up to two runs of cells next to
  // each other (where they go round the box), and so of rows next to
  // each other; then the entities in them the form takes.
  // (Of a Morton index: the rows of the one leaf a walk is at, the run
  // `visitRuns` goes over in place of the grid's.)
  std::pair<mlir::Value, mlir::Value> mortonRun;
  auto visitRuns = [&](mlir::Value outer, Found found) -> LogicalResult {
    mlir::Value start1, end1, start2, end2;
    if (mortonRun.first) {
      start1 = mortonRun.first;
      end1 = mortonRun.second;
      start2 = end1;
      end2 = end1;
    } else {
      mlir::Value low, high;
      if (kind == Kind::Between) {
        low = lowCell[0];
        high = highCell[0];
      } else {
        mlir::Value left = reach;
        if (outer && whole) {
          left = call("_root",
                      {arith::SubIOp::create(builder, where, far, outer)});
          if (t != i64)
            left = arith::TruncIOp::create(builder, where, t, left);
        } else if (outer) {
          left = math::SqrtOp::create(
              builder, where,
              arith::SubFOp::create(builder, where, far, outer));
        }
        low = call("_low", {at_[0], left, starts[0], widths[0]});
        high = call("_high", {at_[0], left, starts[0], widths[0]});
      }
      auto part = [&](int64_t which) {
        return call("_run", {integer(where, i64, which), low, high, from[0],
                             many[0], sides[0]});
      };
      // The cell number of the first along the first side.
      mlir::Value base = integer(where, i64, 0);
      for (unsigned d = dims; d-- > 1;)
        base = arith::MulIOp::create(
            builder, where,
            arith::AddIOp::create(builder, where, base, cell[d]), sides[d - 1]);
      // (Rows the index has, by the cells: read by row numbers, which are
      // not asked whether they are rows of the buffer.)
      auto cellRow = IndexOfType::get(context, cells);
      auto firstOf = [&](mlir::Value along, int64_t plus) -> mlir::Value {
        mlir::Value number = arith::AddIOp::create(
            builder, where, base,
            arith::AddIOp::create(builder, where, along,
                                  integer(where, i64, plus)));
        return BufferAtOp::create(
            builder, where, i32, cells, builder.getStringAttr("first"),
            asIndex(where, arith::TruncIOp::create(builder, where, i32, number),
                    cellRow));
      };
      start1 = firstOf(part(0), 0);
      end1 = firstOf(part(1), 1);
      start2 = firstOf(part(2), 0);
      end2 = firstOf(part(3), 1);
    }
    // (After a row: each run from the row after it, where it starts
    // before; one that ends before it has none.)
    if (afterRow) {
      mlir::Value next = arith::AddIOp::create(builder, where, afterRow,
                                               integer(where, i32, 1));
      start1 = arith::MaxSIOp::create(builder, where, start1, next);
      start2 = arith::MaxSIOp::create(builder, where, start2, next);
    }
    // A run's first and end, of the two.
    auto runAt = [&](mlir::Value run) -> std::pair<mlir::Value, mlir::Value> {
      mlir::Value second = arith::CmpIOp::create(
          builder, where, arith::CmpIPredicate::ne, run, zero);
      return {
          arith::IndexCastOp::create(
              builder, where, indexType,
              arith::SelectOp::create(builder, where, second, start2, start1)),
          arith::IndexCastOp::create(
              builder, where, indexType,
              arith::SelectOp::create(builder, where, second, end2, end1))};
    };
    // The runs one after the other, each a loop over its rows, asking of
    // each whether the form takes it: where the statements only read and
    // set vars, the loop is done many rows at once as it is.
    auto plain = [&]() -> LogicalResult {
      return loopWithVars(
          where, zero, two, [&](mlir::Value run) -> LogicalResult {
            auto [first, end] = runAt(run);
            return loopWithVars(
                where, first, end, [&](mlir::Value counted) -> LogicalResult {
                  mlir::Value row = rowOf(counted);
                  NearRows near{symbolName, row, {}};
                  mlir::Value distance;
                  mlir::Value in = seenAt(row, near, &distance);
                  if (found)
                    return found(row, counted, in, distance, near);
                  return ifWithVars(where, in,
                                    [&] { return visit(row, near); });
                });
          });
    };
    if (found)
      return plain();
    // Where they write, a branch for each row would go one way or the
    // other as it may: each run by blocks of 64 rows instead, which of a
    // block's rows the form takes one bit each (a loop without branches,
    // done many rows at once), then the statements for each of those, in
    // the order of the rows.
    auto blocked = [&]() -> LogicalResult {
      mlir::Value one = arith::ConstantIndexOp::create(builder, where, 1);
      mlir::Value block = arith::ConstantIndexOp::create(builder, where, 64);
      return loopWithVars(
          where, zero, two, [&](mlir::Value run) -> LogicalResult {
            auto [first, end] = runAt(run);
            mlir::Value length = arith::MaxSIOp::create(
                builder, where,
                arith::SubIOp::create(builder, where, end, first), zero);
            mlir::Value blocks = arith::DivUIOp::create(
                builder, where,
                arith::AddIOp::create(
                    builder, where, length,
                    arith::ConstantIndexOp::create(builder, where, 63)),
                block);
            return loopWithVars(
                where, zero, blocks, [&](mlir::Value b) -> LogicalResult {
                  mlir::Value low = arith::AddIOp::create(
                      builder, where, first,
                      arith::MulIOp::create(builder, where, b, block));
                  mlir::Value many = arith::MinSIOp::create(
                      builder, where, block,
                      arith::SubIOp::create(builder, where, end, low));
                  auto bits =
                      scf::ForOp::create(builder, where, zero, many, one,
                                         ValueRange{integer(where, i64, 0)});
                  bits->setAttr(kNoGatherAttr, builder.getUnitAttr());
                  {
                    OpBuilder::InsertionGuard guard(builder);
                    builder.setInsertionPointToStart(bits.getBody());
                    mlir::Value i = bits.getInductionVar();
                    NearRows near{symbolName, {}, {}};
                    mlir::Value in = seenAt(
                        rowOf(arith::AddIOp::create(builder, where, low, i)),
                        near);
                    mlir::Value bit = arith::ShLIOp::create(
                        builder, where,
                        arith::ExtUIOp::create(builder, where, i64, in),
                        arith::IndexCastOp::create(builder, where, i64, i));
                    scf::YieldOp::create(
                        builder, where,
                        mlir::Value(arith::OrIOp::create(
                            builder, where, bits.getRegionIterArg(0), bit)));
                  }
                  // (The bits not yet visited go round as a var of a
                  // name no program can write.)
                  ScopeGuard hidden(*this);
                  const char *left = " near bits";
                  bind(left, Variable::ofVar(bits.getResult(0)));
                  mlir::Value hits = arith::IndexCastOp::create(
                      builder, where, indexType,
                      math::CtPopOp::create(builder, where, bits.getResult(0)));
                  return loopWithVars(
                      where, zero, hits, [&](mlir::Value) -> LogicalResult {
                        mlir::Value unvisited = lookup(left)->value;
                        mlir::Value at = math::CountTrailingZerosOp::create(
                            builder, where, unvisited);
                        llvm::find_if(llvm::reverse(scopes),
                                      [&](auto &s) { return s.count(left); })
                            ->find(left)
                            ->second.value = arith::AndIOp::create(
                            builder, where, unvisited,
                            arith::SubIOp::create(builder, where, unvisited,
                                                  integer(where, i64, 1)));
                        mlir::Value row = rowOf(arith::AddIOp::create(
                            builder, where, low,
                            arith::IndexCastOp::create(builder, where,
                                                       indexType, at)));
                        NearRows near{symbolName, row, {}};
                        seenAt(row, near);
                        return visit(row, near);
                      });
                });
          });
    };
    // Which: the statements read as the plain loop first, and again as
    // blocks where they write.
    Lexer lexerBefore = lexer;
    Token tokenBefore = token;
    const char *endBefore = previousEnd;
    SmallVector<VarState> varsBefore = captureVars();
    Block *into = builder.getInsertionBlock();
    Operation *last = builder.getInsertionPoint() == into->begin()
                          ? nullptr
                          : &*std::prev(builder.getInsertionPoint());
    if (failed(plain()))
      return failure();
    auto firstMade = last ? std::next(last->getIterator()) : into->begin();
    auto madeEnd = builder.getInsertionPoint();
    bool writes = false;
    for (Operation &op : llvm::make_range(firstMade, madeEnd))
      op.walk([&](Operation *inner) {
        if (!onlyReads(inner))
          writes = true;
      });
    if (!writes)
      return success();
    SmallVector<Operation *> made;
    for (Operation &op : llvm::make_range(firstMade, madeEnd))
      made.push_back(&op);
    for (Operation *op : llvm::reverse(made))
      op->erase();
    restoreVars(varsBefore);
    lexer = lexerBefore;
    token = tokenBefore;
    previousEnd = endBefore;
    return blocked();
  };
  // How far, at least, the entities of a cell are from the place on side
  // `d`, squared: a row of cells (or a cell) farther than `r` is not
  // visited, which leaves out no entity within it, nor changes the order
  // of those visited.
  auto gapSquared = [&](unsigned d) -> mlir::Value {
    mlir::Value gap = call(
        "_gap",
        {cell[d], at_[d], starts[d], widths[d], size ? size : widths[d]}, t);
    return squared(gap);
  };
  // The cells around on every side but the first, the last side's
  // outermost, those farther than `r` left out (of a distance); `outer`
  // how far, at least, the row of cells is on the sides outside this one.
  std::function<LogicalResult(unsigned, mlir::Value, Found)> sideLoop =
      [&](unsigned d, mlir::Value outer, Found found) -> LogicalResult {
    return loopWithVars(
        where, zero,
        arith::IndexCastOp::create(builder, where, indexType, many[d]),
        [&](mlir::Value counted) -> LogicalResult {
          mlir::Value k =
              arith::IndexCastOp::create(builder, where, i64, counted);
          mlir::Value along = arith::AddIOp::create(builder, where, from[d], k);
          cell[d] = index.wrap ? call("_at", {along, sides[d]}) : along;
          auto inner = [&](mlir::Value gap) {
            return d == 1 ? visitRuns(gap, found) : sideLoop(d - 1, gap, found);
          };
          if (kind == Kind::Between)
            return inner(mlir::Value());
          mlir::Value gap = gapSquared(d);
          if (outer)
            gap = plus(outer, gap);
          return ifWithVars(where, atMost(gap, far),
                            [&] { return inner(gap); });
        });
  };
  // Of a Morton index: what is read of a cell (by its number, an i32),
  // and how far, at least, its entities are from the place, squared (on
  // each side from the place to its box, of the nearest image where the
  // box wraps).
  auto cellAt = [&](mlir::Value number, StringRef field,
                    Type type) -> mlir::Value {
    return BufferAtOp::create(
        builder, where, type, cells, builder.getStringAttr(field),
        asIndex(where, number, IndexOfType::get(context, cells)));
  };
  // (Of fractions by a comparison, which a NaN does not pass: one
  // instruction, where `maximumf` has more for what a NaN gives.)
  auto most = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    if (isa<IntegerType>(a.getType()))
      return arith::MaxSIOp::create(builder, where, a, b);
    return arith::SelectOp::create(
        builder, where,
        arith::CmpFOp::create(builder, where, arith::CmpFPredicate::OGT, a, b),
        a, b);
  };
  auto least = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
    if (isa<IntegerType>(a.getType()))
      return arith::MinSIOp::create(builder, where, a, b);
    return arith::SelectOp::create(
        builder, where,
        arith::CmpFOp::create(builder, where, arith::CmpFPredicate::OLT, a, b),
        a, b);
  };
  auto cellGap = [&](mlir::Value number) -> mlir::Value {
    mlir::Value total;
    for (unsigned d = 0; d < dims; ++d) {
      std::string side = std::to_string(d);
      mlir::Value lo = cellAt(number, "lo" + side, t);
      mlir::Value hi = cellAt(number, "hi" + side, t);
      auto out = [&](mlir::Value a, mlir::Value b) {
        return most(most(minus(a, at_[d]), minus(at_[d], b)),
                    zeroOf(distanceType));
      };
      mlir::Value gap = out(lo, hi);
      if (index.wrap) {
        auto less = [&](mlir::Value a) -> mlir::Value {
          if (whole)
            return arith::SubIOp::create(builder, where, a, size);
          return arith::SubFOp::create(builder, where, a, size);
        };
        gap = least(gap, out(less(lo), less(hi)));
        gap = least(gap, out(plus(lo, size), plus(hi, size)));
      }
      mlir::Value square = squared(gap);
      total = total ? plus(total, square) : square;
    }
    return total;
  };
  // Of `between`: whether a cell's box meets the corners' on every side
  // (or its image a box on, where the box wraps).
  auto cellMeets = [&](mlir::Value number) -> mlir::Value {
    mlir::Value meets;
    for (unsigned d = 0; d < dims; ++d) {
      std::string side = std::to_string(d);
      mlir::Value lo = cellAt(number, "lo" + side, t);
      mlir::Value hi = cellAt(number, "hi" + side, t);
      auto overlaps = [&](mlir::Value a, mlir::Value b) {
        return arith::AndIOp::create(builder, where, atMost(a, highCorner[d]),
                                     atMost(lowCorner[d], b))
            .getResult();
      };
      mlir::Value side_ = overlaps(lo, hi);
      if (index.wrap)
        side_ = arith::OrIOp::create(builder, where, side_,
                                     overlaps(plus(lo, size), plus(hi, size)));
      meets = both(meets, side_);
    }
    return meets;
  };
  // The cells in preorder, from the root: a cell no entity of which the
  // form may take is left with all inside it (on to its `next`), a leaf
  // has its rows visited (and is left so), else the walk goes on to the
  // first cell inside it, the next in preorder. The rows so in the order
  // of the index.
  auto mortonWalk = [&](Found found, function_ref<mlir::Value()> limit =
                                         nullptr) -> LogicalResult {
    ScopeGuard hidden(*this);
    // (Where it is goes round as a var of a name no program can write.)
    const char *cursor = " morton cell";
    bind(cursor, Variable::ofVar(integer(where, i32, 0)));
    auto setAt = [&](mlir::Value value) {
      llvm::find_if(llvm::reverse(scopes),
                    [&](auto &s) { return s.count(cursor); })
          ->find(cursor)
          ->second.value = value;
    };
    return whileWithVars(
        where,
        [&]() -> mlir::Value {
          return arith::CmpIOp::create(
              builder, where, arith::CmpIPredicate::slt, lookup(cursor)->value,
              BufferLenOp::create(builder, where, i32, cells));
        },
        [&]() -> LogicalResult {
          mlir::Value number = lookup(cursor)->value;
          mlir::Value first = cellAt(number, "first", i32);
          mlir::Value count = cellAt(number, "count", i32);
          mlir::Value child = cellAt(number, "child", i32);
          mlir::Value leaf =
              arith::CmpIOp::create(builder, where, arith::CmpIPredicate::slt,
                                    child, integer(where, i32, 0));
          mlir::Value next = cellAt(number, "next", i32);
          mlir::Value end = arith::AddIOp::create(builder, where, first, count);
          mlir::Value reached =
              kind == Kind::Between
                  ? cellMeets(number)
                  : atMost(cellGap(number), limit ? limit() : far);
          if (afterRow)
            reached = arith::AndIOp::create(
                builder, where, reached,
                arith::CmpIOp::create(
                    builder, where, arith::CmpIPredicate::sgt,
                    arith::SubIOp::create(builder, where, end,
                                          integer(where, i32, 1)),
                    afterRow));
          mlir::Value rows_ =
              arith::AndIOp::create(builder, where, reached, leaf);
          if (failed(ifWithVars(where, rows_, [&]() -> LogicalResult {
                // (Its rows from the first, as a grid's runs: after a row
                // from the row after it, as there.)
                mortonRun = {first, end};
                auto reset = llvm::make_scope_exit([&] { mortonRun = {}; });
                return visitRuns(mlir::Value(), found);
              })))
            return failure();
          mlir::Value past = arith::OrIOp::create(
              builder, where, leaf,
              arith::XOrIOp::create(
                  builder, where, reached,
                  arith::ConstantIntOp::create(builder, where, 1, 1)));
          setAt(arith::SelectOp::create(builder, where, past, next, child));
          return success();
        });
  };
  auto walk = [&](Found found) -> LogicalResult {
    if (index.morton)
      return mortonWalk(found);
    return dims == 1 ? visitRuns(mlir::Value(), found)
                     : sideLoop(dims - 1, mlir::Value(), found);
  };
  if (kind != Kind::Nearest)
    return walk(nullptr);

  // `nearest k`: a list of the k nearest so far, in order (by how far,
  // then by row), of the rows within a distance and taken by the
  // `where`; the distance from a cell's width doubled until k are in it,
  // or it is as far as any entity may be (or `within` says).
  auto listType = MemRefType::get({ShapedType::kDynamic}, i32);
  auto farType = MemRefType::get({ShapedType::kDynamic}, distanceType);
  mlir::Value k =
      arith::MaxSIOp::create(builder, where, wanted, integer(where, i32, 0));
  mlir::Value capacity =
      arith::IndexCastOp::create(builder, where, indexType, k);
  mlir::Value listed =
      memref::AllocOp::create(builder, where, listType, ValueRange{capacity});
  mlir::Value distances =
      memref::AllocOp::create(builder, where, farType, ValueRange{capacity});
  auto countType = MemRefType::get({1}, i32);
  mlir::Value counter = memref::AllocOp::create(builder, where, countType);
  mlir::Value c0 = arith::ConstantIndexOp::create(builder, where, 0);
  // Each row within the distance and taken: into the list where it is
  // nearer than its last (or the list is not full), past those farther.
  auto insert = [&](mlir::Value row, mlir::Value counted, mlir::Value in,
                    mlir::Value howFar, NearRows &near) -> LogicalResult {
    FailureOr<mlir::Value> taken = takes(row, near);
    if (failed(taken))
      return failure();
    mlir::Value number =
        arith::IndexCastOp::create(builder, where, i32, counted);
    auto branch = scf::IfOp::create(builder, where, both(in, *taken),
                                    /*withElseRegion=*/false);
    OpBuilder::InsertionGuard inner(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    mlir::Value n =
        memref::LoadOp::create(builder, where, counter, ValueRange{c0});
    // (Nearer than the list's `at`: by how far, then by row.)
    auto nearer = [&](mlir::Value atIndex) -> mlir::Value {
      mlir::Value otherFar = memref::LoadOp::create(builder, where, distances,
                                                    ValueRange{atIndex});
      mlir::Value otherRow =
          memref::LoadOp::create(builder, where, listed, ValueRange{atIndex});
      mlir::Value same = compare(arith::CmpIPredicate::eq,
                                 arith::CmpFPredicate::OEQ, howFar, otherFar);
      mlir::Value earlier = arith::CmpIOp::create(
          builder, where, arith::CmpIPredicate::slt, number, otherRow);
      return arith::OrIOp::create(
          builder, where, below(howFar, otherFar),
          arith::AndIOp::create(builder, where, same, earlier));
    };
    mlir::Value room =
        arith::CmpIOp::create(builder, where, arith::CmpIPredicate::slt, n, k);
    mlir::Value any = arith::CmpIOp::create(
        builder, where, arith::CmpIPredicate::sgt, k, integer(where, i32, 0));
    // Where it goes in from: the end, or the last place of a full list.
    auto goesIn =
        scf::IfOp::create(builder, where, TypeRange{builder.getI1Type()}, room,
                          /*withElseRegion=*/true);
    {
      OpBuilder::InsertionGuard g(builder);
      builder.setInsertionPointToStart(goesIn.thenBlock());
      scf::YieldOp::create(
          builder, where,
          ValueRange{arith::ConstantIntOp::create(builder, where, 1, 1)});
      builder.setInsertionPointToStart(goesIn.elseBlock());
      auto ask =
          scf::IfOp::create(builder, where, TypeRange{builder.getI1Type()}, any,
                            /*withElseRegion=*/true);
      {
        OpBuilder::InsertionGuard g2(builder);
        builder.setInsertionPointToStart(ask.thenBlock());
        mlir::Value lastAt = arith::IndexCastOp::create(
            builder, where, indexType,
            arith::SubIOp::create(builder, where, k, integer(where, i32, 1)));
        scf::YieldOp::create(builder, where, ValueRange{nearer(lastAt)});
        builder.setInsertionPointToStart(ask.elseBlock());
        scf::YieldOp::create(
            builder, where,
            ValueRange{arith::ConstantIntOp::create(builder, where, 0, 1)});
      }
      scf::YieldOp::create(builder, where, ask.getResults());
    }
    auto put = scf::IfOp::create(builder, where, goesIn.getResult(0),
                                 /*withElseRegion=*/false);
    builder.setInsertionPoint(put.thenBlock()->getTerminator());
    mlir::Value slot = arith::SelectOp::create(
        builder, where, room, n,
        arith::SubIOp::create(builder, where, k, integer(where, i32, 1)));
    // Past those farther: each moved one on.
    auto past =
        scf::WhileOp::create(builder, where, TypeRange{i32}, ValueRange{slot});
    {
      OpBuilder::InsertionGuard g(builder);
      Block *ask = builder.createBlock(&past.getBefore(), {}, {i32}, {where});
      mlir::Value j = ask->getArgument(0);
      mlir::Value more = arith::CmpIOp::create(
          builder, where, arith::CmpIPredicate::sgt, j, integer(where, i32, 0));
      auto check = scf::IfOp::create(builder, where,
                                     TypeRange{builder.getI1Type()}, more,
                                     /*withElseRegion=*/true);
      {
        OpBuilder::InsertionGuard g2(builder);
        builder.setInsertionPointToStart(check.thenBlock());
        mlir::Value previous = arith::IndexCastOp::create(
            builder, where, indexType,
            arith::SubIOp::create(builder, where, j, integer(where, i32, 1)));
        scf::YieldOp::create(builder, where, ValueRange{nearer(previous)});
        builder.setInsertionPointToStart(check.elseBlock());
        scf::YieldOp::create(
            builder, where,
            ValueRange{arith::ConstantIntOp::create(builder, where, 0, 1)});
      }
      scf::ConditionOp::create(builder, where, check.getResult(0),
                               ValueRange{j});
      Block *move = builder.createBlock(&past.getAfter(), {}, {i32}, {where});
      mlir::Value to = move->getArgument(0);
      mlir::Value back =
          arith::SubIOp::create(builder, where, to, integer(where, i32, 1));
      mlir::Value toAt =
          arith::IndexCastOp::create(builder, where, indexType, to);
      mlir::Value backAt =
          arith::IndexCastOp::create(builder, where, indexType, back);
      memref::StoreOp::create(
          builder, where,
          memref::LoadOp::create(builder, where, listed, ValueRange{backAt}),
          listed, ValueRange{toAt});
      memref::StoreOp::create(
          builder, where,
          memref::LoadOp::create(builder, where, distances, ValueRange{backAt}),
          distances, ValueRange{toAt});
      scf::YieldOp::create(builder, where, ValueRange{back});
    }
    mlir::Value placed = arith::IndexCastOp::create(builder, where, indexType,
                                                    past.getResult(0));
    memref::StoreOp::create(builder, where, number, listed, ValueRange{placed});
    memref::StoreOp::create(builder, where, howFar, distances,
                            ValueRange{placed});
    memref::StoreOp::create(
        builder, where,
        arith::MinSIOp::create(
            builder, where,
            arith::AddIOp::create(builder, where, n, integer(where, i32, 1)),
            k),
        counter, ValueRange{c0});
    return success();
  };
  if (index.morton) {
    // Of a Morton index: first the rows around the place's own key (k on
    // either side, by halving), near it as the Morton order goes; then the
    // cells in preorder as `near` walks them, a cell farther than the
    // list's last (where it is full, or than `within`) left with all inside
    // it, a leaf's other rows each into the list as above.
    memref::StoreOp::create(builder, where, integer(where, i32, 0), counter,
                            ValueRange{c0});
    far = given ? squared(given) : mlir::Value();
    mlir::Value endless =
        whole ? integer(where, i64, std::numeric_limits<int64_t>::max())
              : arith::ConstantOp::create(
                    builder, where,
                    builder.getFloatAttr(
                        distanceType, std::numeric_limits<double>::infinity()))
                    .getResult();
    mlir::Value cap = far ? far : endless;
    auto bound = [&]() -> mlir::Value {
      mlir::Value n =
          memref::LoadOp::create(builder, where, counter, ValueRange{c0});
      mlir::Value full = arith::CmpIOp::create(builder, where,
                                               arith::CmpIPredicate::sge, n, k);
      mlir::Value lastAt = arith::IndexCastOp::create(
          builder, where, indexType,
          arith::MaxSIOp::create(
              builder, where,
              arith::SubIOp::create(builder, where, k, integer(where, i32, 1)),
              integer(where, i32, 0)));
      mlir::Value last =
          memref::LoadOp::create(builder, where, distances, ValueRange{lastAt});
      return arith::SelectOp::create(builder, where, full, least(last, cap),
                                     cap);
    };
    SmallVector<VarState> varsBefore = captureVars();
    // The place's key, and the first row whose key is not below it.
    SmallVector<mlir::Value> keyArgs(at_.begin(), at_.end());
    for (unsigned d = 0; d < dims; ++d)
      keyArgs.push_back(read("lo" + std::to_string(d), t));
    keyArgs.push_back(read("scale", builder.getF64Type()));
    mlir::Value placeKey = call("_key", keyArgs, i64);
    Function &find = functions.ofSymbol(index.find);
    mlir::Value held =
        BufferOfOp::create(builder, where, find.params.front(), rows)
            .getResult();
    mlir::Value own =
        InvokeOp::create(builder, where, i32,
                         FlatSymbolRefAttr::get(context, index.find),
                         ValueRange{held, placeKey}, UnitAttr())
            ->getResult(0);
    mlir::Value many_ = BufferLenOp::create(builder, where, i32, rows);
    mlir::Value wantedRows =
        arith::MaxSIOp::create(builder, where, k, integer(where, i32, 0));
    mlir::Value seedFrom = arith::IndexCastOp::create(
        builder, where, indexType,
        arith::MaxSIOp::create(
            builder, where, integer(where, i32, 0),
            arith::SubIOp::create(builder, where, own, wantedRows)));
    mlir::Value seedTo = arith::IndexCastOp::create(
        builder, where, indexType,
        arith::MinSIOp::create(
            builder, where, many_,
            arith::AddIOp::create(builder, where, own, wantedRows)));
    if (failed(loopWithVars(where, seedFrom, seedTo,
                            [&](mlir::Value counted) -> LogicalResult {
                              mlir::Value row = rowOf(counted);
                              NearRows near{symbolName, row, {}};
                              mlir::Value distance;
                              mlir::Value in = seenAt(row, near, &distance);
                              return insert(row, counted, in, distance, near);
                            })))
      return failure();
    // (Those rows into the list once.)
    auto others = [&](mlir::Value row, mlir::Value counted, mlir::Value in,
                      mlir::Value howFar, NearRows &near) -> LogicalResult {
      mlir::Value apart = arith::OrIOp::create(
          builder, where,
          arith::CmpIOp::create(builder, where, arith::CmpIPredicate::ult,
                                counted, seedFrom),
          arith::CmpIOp::create(builder, where, arith::CmpIPredicate::uge,
                                counted, seedTo));
      return insert(row, counted, both(in, apart), howFar, near);
    };
    // The cells from the root, depth first, the nearest of those inside a
    // cell first: a stack in memory (a cell's cells at most for each
    // level), each with how far its box is, those inside a cell asked
    // together (side by side) and each not farther than the list's last
    // put in past those nearer than it, so that the nearest is on top.
    int64_t room = (index.bits + 1) * (int64_t(1) << dims) + 1;
    mlir::Value stack =
        memref::AllocOp::create(builder, where, MemRefType::get({room}, i32));
    mlir::Value gaps = memref::AllocOp::create(
        builder, where, MemRefType::get({room}, distanceType));
    auto at32 = [&](mlir::Value v) -> mlir::Value {
      return arith::IndexCastOp::create(builder, where, indexType, v);
    };
    mlir::Value start = arith::AndIOp::create(
        builder, where,
        arith::CmpIOp::create(builder, where, arith::CmpIPredicate::sgt, k,
                              integer(where, i32, 0)),
        arith::CmpIOp::create(builder, where, arith::CmpIPredicate::sgt,
                              BufferLenOp::create(builder, where, i32, cells),
                              integer(where, i32, 0)));
    memref::StoreOp::create(builder, where, integer(where, i32, 0), stack,
                            ValueRange{c0});
    memref::StoreOp::create(builder, where, zeroOf(distanceType), gaps,
                            ValueRange{c0});
    auto search = scf::WhileOp::create(
        builder, where, TypeRange{i32},
        ValueRange{arith::ExtUIOp::create(builder, where, i32, start)});
    {
      OpBuilder::InsertionGuard guard(builder);
      Block *ask = builder.createBlock(&search.getBefore(), {}, {i32}, {where});
      mlir::Value height = ask->getArgument(0);
      scf::ConditionOp::create(
          builder, where,
          arith::CmpIOp::create(builder, where, arith::CmpIPredicate::sgt,
                                height, integer(where, i32, 0)),
          ValueRange{height});
      Block *step = builder.createBlock(&search.getAfter(), {}, {i32}, {where});
      mlir::Value top = arith::SubIOp::create(
          builder, where, step->getArgument(0), integer(where, i32, 1));
      mlir::Value number =
          memref::LoadOp::create(builder, where, stack, ValueRange{at32(top)});
      mlir::Value gap =
          memref::LoadOp::create(builder, where, gaps, ValueRange{at32(top)});
      auto reached = scf::IfOp::create(builder, where, TypeRange{i32},
                                       atMost(gap, bound()),
                                       /*withElseRegion=*/true);
      {
        OpBuilder::InsertionGuard inner(builder);
        builder.setInsertionPointToStart(reached.elseBlock());
        scf::YieldOp::create(builder, where, ValueRange{top});
        builder.setInsertionPointToStart(reached.thenBlock());
        mlir::Value child = cellAt(number, "child", i32);
        mlir::Value leaf =
            arith::CmpIOp::create(builder, where, arith::CmpIPredicate::slt,
                                  child, integer(where, i32, 0));
        auto split = scf::IfOp::create(builder, where, TypeRange{i32}, leaf,
                                       /*withElseRegion=*/true);
        builder.setInsertionPointToStart(split.thenBlock());
        {
          mlir::Value first = cellAt(number, "first", i32);
          mlir::Value end = arith::AddIOp::create(builder, where, first,
                                                  cellAt(number, "count", i32));
          mortonRun = {first, end};
          auto reset = llvm::make_scope_exit([&] { mortonRun = {}; });
          if (failed(visitRuns(mlir::Value(), others)))
            return failure();
          scf::YieldOp::create(builder, where, ValueRange{top});
        }
        builder.setInsertionPointToStart(split.elseBlock());
        mlir::Value kids = cellAt(number, "kids", i32);
        auto inside = scf::ForOp::create(
            builder, where, at32(child),
            at32(arith::AddIOp::create(builder, where, child, kids)),
            arith::ConstantIndexOp::create(builder, where, 1), ValueRange{top});
        {
          OpBuilder::InsertionGuard g(builder);
          builder.setInsertionPointToStart(inside.getBody());
          mlir::Value cell = arith::IndexCastOp::create(
              builder, where, i32, inside.getInductionVar());
          mlir::Value height = inside.getRegionIterArg(0);
          mlir::Value g2 = cellGap(cell);
          auto keep = scf::IfOp::create(builder, where, TypeRange{i32},
                                        atMost(g2, bound()),
                                        /*withElseRegion=*/true);
          {
            OpBuilder::InsertionGuard g3(builder);
            builder.setInsertionPointToStart(keep.elseBlock());
            scf::YieldOp::create(builder, where, ValueRange{height});
            builder.setInsertionPointToStart(keep.thenBlock());
            // (Past those of this cell's nearer than it, from the top
            // down: they move up one.)
            auto past = scf::WhileOp::create(builder, where, TypeRange{i32},
                                             ValueRange{height});
            {
              OpBuilder::InsertionGuard g4(builder);
              Block *check =
                  builder.createBlock(&past.getBefore(), {}, {i32}, {where});
              mlir::Value j = check->getArgument(0);
              mlir::Value above = arith::CmpIOp::create(
                  builder, where, arith::CmpIPredicate::sgt, j, top);
              auto nearer = scf::IfOp::create(
                  builder, where, TypeRange{builder.getI1Type()}, above,
                  /*withElseRegion=*/true);
              {
                OpBuilder::InsertionGuard g5(builder);
                builder.setInsertionPointToStart(nearer.thenBlock());
                mlir::Value under = memref::LoadOp::create(
                    builder, where, gaps,
                    ValueRange{at32(arith::SubIOp::create(
                        builder, where, j, integer(where, i32, 1)))});
                scf::YieldOp::create(builder, where,
                                     ValueRange{below(under, g2)});
                builder.setInsertionPointToStart(nearer.elseBlock());
                scf::YieldOp::create(builder, where,
                                     ValueRange{arith::ConstantIntOp::create(
                                         builder, where, 0, 1)});
              }
              scf::ConditionOp::create(builder, where, nearer.getResult(0),
                                       ValueRange{j});
              Block *move =
                  builder.createBlock(&past.getAfter(), {}, {i32}, {where});
              mlir::Value to = move->getArgument(0);
              mlir::Value from_ = arith::SubIOp::create(builder, where, to,
                                                        integer(where, i32, 1));
              memref::StoreOp::create(
                  builder, where,
                  memref::LoadOp::create(builder, where, stack,
                                         ValueRange{at32(from_)}),
                  stack, ValueRange{at32(to)});
              memref::StoreOp::create(
                  builder, where,
                  memref::LoadOp::create(builder, where, gaps,
                                         ValueRange{at32(from_)}),
                  gaps, ValueRange{at32(to)});
              scf::YieldOp::create(builder, where, ValueRange{from_});
            }
            mlir::Value placed = at32(past.getResult(0));
            memref::StoreOp::create(builder, where, cell, stack,
                                    ValueRange{placed});
            memref::StoreOp::create(builder, where, g2, gaps,
                                    ValueRange{placed});
            scf::YieldOp::create(
                builder, where,
                ValueRange{arith::AddIOp::create(builder, where, height,
                                                 integer(where, i32, 1))});
          }
          scf::YieldOp::create(builder, where, keep.getResults());
        }
        scf::YieldOp::create(builder, where, inside.getResults());
        builder.setInsertionPointAfter(split);
        scf::YieldOp::create(builder, where, split.getResults());
      }
      scf::YieldOp::create(builder, where, reached.getResults());
    }
    memref::DeallocOp::create(builder, where, stack);
    memref::DeallocOp::create(builder, where, gaps);
    restoreVars(varsBefore);
  } else {
    // As far as any entity may be, squared: half the box on each side where
    // it wraps, else the farther end of the grid from the place.
    mlir::Value farthest;
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value span;
      if (index.wrap) {
        span = whole ? arith::DivSIOp::create(builder, where, size,
                                              integer(where, t, 2))
                           .getResult()
                     : arith::MulFOp::create(
                           builder, where, size,
                           arith::ConstantOp::create(
                               builder, where, builder.getFloatAttr(t, 0.5)))
                           .getResult();
        // (Half of an odd whole size rounds down: one more.)
        if (whole)
          span =
              arith::AddIOp::create(builder, where, span, integer(where, t, 1));
      } else {
        mlir::Value top =
            whole
                ? arith::AddIOp::create(builder, where, wide(starts[d]),
                                        arith::MulIOp::create(builder, where,
                                                              sides[d],
                                                              wide(widths[d])))
                      .getResult()
                : arith::AddFOp::create(
                      builder, where, starts[d],
                      arith::MulFOp::create(
                          builder, where, widths[d],
                          arith::SIToFPOp::create(builder, where, t, sides[d])))
                      .getResult();
        if (whole) {
          mlir::Value p = wide(at_[d]);
          mlir::Value a =
              arith::SubIOp::create(builder, where, p, wide(starts[d]));
          mlir::Value b = arith::SubIOp::create(builder, where, top, p);
          mlir::Value absA = math::AbsIOp::create(builder, where, a);
          mlir::Value absB = math::AbsIOp::create(builder, where, b);
          span = arith::MaxSIOp::create(builder, where, absA, absB);
        } else {
          mlir::Value a = math::AbsFOp::create(
              builder, where,
              arith::SubFOp::create(builder, where, at_[d], starts[d]));
          mlir::Value b = math::AbsFOp::create(
              builder, where,
              arith::SubFOp::create(builder, where, top, at_[d]));
          span = arith::MaximumFOp::create(builder, where, a, b);
        }
      }
      mlir::Value square = squared(span);
      farthest = farthest ? plus(farthest, square) : square;
    }
    // The first distance: where the entities are as many everywhere, the
    // one that half as many again as k are within (in the area, or volume,
    // the grid is over: a ball of it as big as k and a half of the
    // entities' shares), no farther than `within`.
    Type f64 = builder.getF64Type();
    auto real = [&](mlir::Value v) -> mlir::Value {
      if (v.getType() == f64)
        return v;
      if (isa<IntegerType>(v.getType()))
        return arith::SIToFPOp::create(builder, where, f64, v);
      return arith::ExtFOp::create(builder, where, f64, v);
    };
    mlir::Value volume;
    for (unsigned d = 0; d < dims; ++d) {
      mlir::Value side = arith::MulFOp::create(builder, where, real(sides[d]),
                                               real(widths[d]));
      volume =
          volume
              ? arith::MulFOp::create(builder, where, volume, side).getResult()
              : side;
    }
    mlir::Value entities = arith::MaxSIOp::create(
        builder, where, BufferLenOp::create(builder, where, i32, rows),
        integer(where, i32, 1));
    // (A ball of radius 1: pi, of three sides 4 pi / 3.)
    double ball = dims == 2 ? M_PI : dims == 3 ? 4.0 * M_PI / 3.0 : 2.0;
    mlir::Value share = arith::DivFOp::create(
        builder, where,
        arith::MulFOp::create(
            builder, where, volume,
            arith::MulFOp::create(
                builder, where, real(k),
                arith::ConstantOp::create(builder, where,
                                          builder.getF64FloatAttr(1.5)))),
        arith::MulFOp::create(
            builder, where, real(entities),
            arith::ConstantOp::create(builder, where,
                                      builder.getF64FloatAttr(ball))));
    // (A guess: of three sides from the fourth root, three steps of
    // Newton's towards the third, without a library for it.)
    mlir::Value guess = share;
    if (dims >= 2)
      guess = math::SqrtOp::create(builder, where, share);
    if (dims == 3) {
      guess = math::SqrtOp::create(builder, where, guess);
      mlir::Value third = arith::ConstantOp::create(
          builder, where, builder.getF64FloatAttr(1.0 / 3.0));
      // (Of nothing, nothing: not a share over a root of nothing.)
      mlir::Value tiny = arith::ConstantOp::create(
          builder, where, builder.getF64FloatAttr(1e-300));
      for (int step = 0; step < 3; ++step) {
        mlir::Value y = arith::MaximumFOp::create(builder, where, guess, tiny);
        guess = arith::MulFOp::create(
            builder, where, third,
            arith::AddFOp::create(
                builder, where, arith::AddFOp::create(builder, where, y, y),
                arith::DivFOp::create(
                    builder, where, share,
                    arith::MulFOp::create(builder, where, y, y))));
      }
    }
    mlir::Value first;
    if (whole)
      first = arith::AddIOp::create(
          builder, where, arith::FPToSIOp::create(builder, where, t, guess),
          integer(where, t, 1));
    else
      first =
          t == f64
              ? guess
              : arith::TruncFOp::create(builder, where, t, guess).getResult();
    if (given) {
      first =
          whole
              ? arith::MinSIOp::create(builder, where, first, given).getResult()
              : arith::MinimumFOp::create(builder, where, first, given)
                    .getResult();
      mlir::Value cap = squared(given);
      farthest = whole
                     ? arith::MinSIOp::create(builder, where, farthest, cap)
                           .getResult()
                     : arith::MinimumFOp::create(builder, where, farthest, cap)
                           .getResult();
    }
    auto whileLoop =
        scf::WhileOp::create(builder, where, TypeRange{t}, ValueRange{first});
    {
      OpBuilder::InsertionGuard guard(builder);
      Block *before =
          builder.createBlock(&whileLoop.getBefore(), {}, {t}, {where});
      mlir::Value distance = before->getArgument(0);
      memref::StoreOp::create(builder, where, integer(where, i32, 0), counter,
                              ValueRange{c0});
      aim(distance);
      SmallVector<VarState> varsBefore = captureVars();
      if (failed(walk(insert)))
        return failure();
      restoreVars(varsBefore);
      mlir::Value n =
          memref::LoadOp::create(builder, where, counter, ValueRange{c0});
      mlir::Value short_ = arith::CmpIOp::create(
          builder, where, arith::CmpIPredicate::slt, n, k);
      // (An index with no rows has no grid to be as far as.)
      mlir::Value some =
          arith::CmpIOp::create(builder, where, arith::CmpIPredicate::sgt,
                                BufferLenOp::create(builder, where, i32, rows),
                                integer(where, i32, 0));
      mlir::Value more = arith::AndIOp::create(
          builder, where, arith::AndIOp::create(builder, where, short_, some),
          below(far, farthest));
      mlir::Value twice = plus(distance, distance);
      mlir::Value next =
          given
              ? (whole ? arith::MinSIOp::create(builder, where, twice, given)
                             .getResult()
                       : arith::MinimumFOp::create(builder, where, twice, given)
                             .getResult())
              : twice;
      scf::ConditionOp::create(builder, where, more, ValueRange{next});
      Block *after =
          builder.createBlock(&whileLoop.getAfter(), {}, {t}, {where});
      scf::YieldOp::create(builder, where, ValueRange{after->getArgument(0)});
    }
  }
  // Then the statements for each, nearest first (where they are seen
  // from, again; how far was asked already).
  far = mlir::Value();
  mlir::Value found = arith::IndexCastOp::create(
      builder, where, indexType,
      memref::LoadOp::create(builder, where, counter, ValueRange{c0}));
  if (failed(
          loopWithVars(where, zero, found, [&](mlir::Value i) -> LogicalResult {
            mlir::Value row = rowOf(arith::IndexCastOp::create(
                builder, where, indexType,
                memref::LoadOp::create(builder, where, listed, ValueRange{i})));
            NearRows near{symbolName, row, {}};
            seenAt(row, near);
            return visit(row, near, /*asked=*/true);
          })))
    return failure();
  memref::DeallocOp::create(builder, where, listed);
  memref::DeallocOp::create(builder, where, distances);
  memref::DeallocOp::create(builder, where, counter);
  return success();
}

LogicalResult Parser::parseEdges(llvm::SMLoc at) {
  if (inEdges)
    return error(at, "edge loops cannot be nested");
  if (!token.is(Token::LParen)) {
    if (peek().is(Token::Colon))
      return error(at, "a 'for' cannot be nested in another 'for'; inside "
                       "one, 'for (name)-[R]->(other) { ... }' visits the "
                       "entity's edges");
    return error(at, "the edges of the entity a 'for' visits are visited "
                     "with an arrow: 'for (name)-[s: R]->(other) { ... }' "
                     "for those out of it, 'for (other)-[s: R]->(name) "
                     "{ ... }' for those into it");
  }
  // A node: `(name)`, `()`, or `([name,] binding: Component, ...)`.
  struct End {
    llvm::SMLoc loc;
    std::string name;
    struct Bound {
      std::string name, component;
      bool mut;
    };
    SmallVector<Bound> bindings;
  };
  auto parseEnd = [&]() -> FailureOr<End> {
    End end;
    end.loc = token.loc;
    if (failed(expect(Token::LParen, "'('")))
      return failure();
    if (consumeIf(Token::RParen))
      return end;
    FailureOr<std::string> first = identifier("a name");
    if (failed(first))
      return failure();
    std::string pending = *first;
    bool binds = token.is(Token::Colon);
    if (!binds) {
      end.name = pending;
      if (consumeIf(Token::Comma)) {
        FailureOr<std::string> next = identifier("a binding");
        if (failed(next))
          return failure();
        pending = *next;
        binds = true;
      }
    }
    while (binds) {
      if (failed(expect(Token::Colon, "':' and a component")))
        return failure();
      // (`mut`: a value can be sent to its fields, with `+=` and such.)
      bool mut = consumeKeyword("mut");
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component))
        return failure();
      auto record = components.find(*component);
      if (record == components.end())
        return error(componentAt, "unknown component '" + *component + "'");
      if (record->second.fields.empty())
        return error(componentAt, "'" + *component + "' has no fields to "
                                  "read of the entity at the other end");
      end.bindings.push_back({pending, *component, mut});
      if (!consumeIf(Token::Comma))
        break;
      FailureOr<std::string> next = identifier("a binding");
      if (failed(next))
        return failure();
      pending = *next;
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    return end;
  };
  FailureOr<End> left = parseEnd();
  if (failed(left))
    return failure();
  FailureOr<ArrowStart> arrow = parseArrowStart();
  if (failed(arrow))
    return failure();
  if (arrow->sibling)
    return error(arrow->loc, "'~' is the arrow from the sibling before, "
                             "which is bound in the head of the 'for'; a "
                             "loop visits edges, '-[R]->'");
  // [mut] [name:] Relation
  bool mut = consumeKeyword("mut");
  llvm::SMLoc relationAt = token.loc;
  FailureOr<std::string> relation = identifier("a relation");
  if (failed(relation))
    return failure();
  std::string edge;
  if (consumeIf(Token::Colon)) {
    edge = *relation;
    relationAt = token.loc;
    relation = identifier("a relation");
    if (failed(relation))
      return failure();
  } else if (mut) {
    return error(relationAt, "'mut' is for an edge that is named: '[mut s: " +
                                 *relation + "]'");
  }
  if (!relations.count(*relation))
    return error(relationAt, "unknown relation '" + *relation + "'");
  if (failed(parseArrowEnd(*arrow)))
    return failure();
  FailureOr<End> right = parseEnd();
  if (failed(right))
    return failure();
  auto isOwn = [&](const End &end) {
    return end.bindings.empty() && !end.name.empty() &&
           queryNames.contains(end.name);
  };
  if (isOwn(*left) == isOwn(*right))
    return error(at, "one end of the arrow is the entity the 'for' visits, "
                     "by one of its names (" +
                         (queryNames.empty()
                              ? std::string("name it: 'for e, ...'")
                              : "'" + queryNames.begin()->getKey().str() +
                                    "'") +
                         "), and the other is the entity at the other end");
  bool leftIsOwn = isOwn(*left);
  End &other = leftIsOwn ? *right : *left;
  // Out of the visited entity if the arrow starts at it.
  StringRef direction = leftIsOwn != arrow->reversed ? "out" : "in";

  // `by s.field`: the edges in the order of a field of theirs, then a
  // slice of that order (`from a`, `to b`: a <= value < b; `first k` or
  // `last k`), worked out before the loop, as a `for` over entities has it.
  std::string orderField;
  SmallVector<Attribute> sliceOrder;
  SmallVector<mlir::Value> sliceValues;
  if (token.isKeyword("by")) {
    llvm::SMLoc byAt = token.loc;
    advance();
    llvm::SMLoc nameAt = token.loc;
    FailureOr<std::string> name = identifier("the edge's name");
    if (failed(name))
      return failure();
    if (edge.empty() || *name != edge)
      return error(nameAt, "the order is by a field of the edge: name it, "
                           "'[s: " +
                               *relation + "]', and say 'by s.field'");
    if (failed(expect(Token::Dot, "'.' and a field of the edge")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = relations[*relation].fieldType(*field);
    if (!type)
      return error(fieldAt, "relation '" + *relation + "' has no field '" +
                                *field + "'");
    if ((!type.isIntOrIndex() || type.isInteger(1)) && !type.isF32() &&
        !type.isF64())
      return error(fieldAt, "the order is by a whole number or a fraction "
                            "(f32, f64); '" +
                                *field + "' is " + typeWords(type));
    (void)byAt;
    orderField = *field;
    std::map<std::string, bool> had;
    for (StringRef part : {"from", "to", "first", "last"}) {
      llvm::SMLoc partAt = token.loc;
      if (!consumeKeyword(part))
        continue;
      if (part == "last" && had.count("first"))
        return error(partAt, "'first' or 'last': one of them");
      Type wanted =
          part == "from" || part == "to" ? type : Type(builder.getI32Type());
      FailureOr<ExprPtr> bound = parseExpr();
      if (failed(bound))
        return failure();
      FailureOr<mlir::Value> value = emit(**bound, wanted);
      if (failed(value))
        return failure();
      if (value->getType() != wanted)
        return mismatch((*bound)->loc, value->getType(), wanted);
      had[part.str()] = true;
      sliceOrder.push_back(builder.getStringAttr(part));
      sliceValues.push_back(*value);
    }
  }
  // `where cond`: the body runs for the edges where it holds. Where it
  // says the other end is an entity worked out the same for every edge
  // (`where v == b`, or one of the conditions `&&` joins), that entity is
  // an operand too, which an index by the other end finds the edges of;
  // the condition stays in the body.
  ExprPtr where;
  if (consumeKeyword("where")) {
    FailureOr<ExprPtr> condition = parseExpr();
    if (failed(condition))
      return failure();
    where = std::move(*condition);
  }
  if (where && !other.name.empty()) {
    std::function<bool(const Expr &)> outside = [&](const Expr &expr) {
      switch (expr.kind) {
      case Expr::Int:
      case Expr::Float:
      case Expr::Bool:
      case Expr::String:
      case Expr::Const:
        return true;
      case Expr::Name: {
        if (expr.name == edge || expr.name == other.name)
          return false;
        for (const End::Bound &bound : other.bindings)
          if (bound.name == expr.name)
            return false;
        const Variable *variable = lookup(expr.name);
        return variable && variable->kind == Variable::Value &&
               !variable->isVar;
      }
      case Expr::Field: {
        if (expr.name == edge || expr.name == other.name)
          return false;
        for (const End::Bound &bound : other.bindings)
          if (bound.name == expr.name)
            return false;
        // (A unique's field, or a case of an enum.)
        return !lookup(expr.name) &&
               (uniques.count(expr.name) || enums.count(expr.name));
      }
      case Expr::Unary:
      case Expr::Binary:
      case Expr::Cast:
        return llvm::all_of(expr.operands, [&](const ExprPtr &operand) {
          return outside(*operand);
        });
      default:
        return false;
      }
    };
    SmallVector<const Expr *> conditions{where.get()};
    bool found = false;
    for (size_t i = 0; i < conditions.size() && !found; ++i) {
      const Expr &condition = *conditions[i];
      if (condition.kind != Expr::Binary)
        continue;
      if (condition.op == Token::AndAnd) {
        for (const ExprPtr &operand : condition.operands)
          conditions.push_back(operand.get());
        continue;
      }
      if (condition.op != Token::Equal)
        continue;
      for (int side : {0, 1}) {
        const Expr &name = *condition.operands[side];
        const Expr &value = *condition.operands[1 - side];
        if (found || name.kind != Expr::Name || name.name != other.name ||
            !outside(value))
          continue;
        FailureOr<mlir::Value> entity = emit(value, EntityType::get(context));
        if (failed(entity))
          return failure();
        if (!isa<EntityType>(entity->getType()))
          continue;
        sliceOrder.push_back(builder.getStringAttr("other"));
        sliceValues.push_back(*entity);
        found = true;
      }
    }
  }

  // (A `break` in it sets its flag, which the loop ends at: see
  // EdgesOp's `break_flag`.)
  ScopeGuard breakScope(*this);
  std::string flag = openBreak(loc(at));
  llvm::scope_exit closeBreak([&] { breakFlags.pop_back(); });
  // Every var goes round: what one edge leaves in it the next starts
  // with, and the loop gives what the last one left.
  SmallVector<VarState> vars = captureVars();
  OperationState state(loc(at), EdgesOp::getOperationName());
  state.addAttribute("relation", symbol(*relation));
  state.addAttribute("direction", builder.getStringAttr(direction));
  state.addOperands(sliceValues);
  for (const VarState &var : vars) {
    state.addOperands(var.value);
    state.addTypes(var.value.getType());
  }
  state.addAttribute(EdgesOp::getOperandSegmentSizesAttrName(state.name),
                     builder.getDenseI32ArrayAttr(
                         {int32_t(sliceValues.size()), int32_t(vars.size())}));
  if (!sliceOrder.empty())
    state.addAttribute(QueryOp::kSliceAttr, builder.getArrayAttr(sliceOrder));
  if (!orderField.empty())
    state.addAttribute("order_field", builder.getStringAttr(orderField));
  for (auto [k, var] : llvm::enumerate(vars))
    if (var.name == flag)
      state.addAttribute("break_flag", builder.getI64IntegerAttr(k));
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  block->addArgument(RefType::get(context, symbol(*relation), mut), loc(at));
  block->addArgument(EntityType::get(context), loc(at));
  for (const VarState &var : vars)
    block->addArgument(var.value.getType(), loc(at));
  Operation *edges = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  edgesScopes = scopes.size();
  ScopeGuard scope(*this);
  if (!edge.empty())
    bind(edge, {Variable::Ref, block->getArgument(0), *relation, mut});
  if (!other.name.empty())
    bind(other.name, Variable::ofValue(block->getArgument(1)));
  SmallVector<VarState> round = vars;
  for (auto [var, arg] : llvm::zip(round, block->getArguments().drop_front(2)))
    var.value = arg;
  restoreVars(round);
  // What is bound of the other entity: the body runs where it has all of
  // it, and a field is looked up where it is read.
  mlir::Value has;
  for (auto &[name, component, mutOther] : other.bindings) {
    Record &record = components[component];
    auto lookup = LookupOp::create(
        builder, loc(other.loc), record.fields.front().second,
        builder.getI1Type(), block->getArgument(1), symbol(component),
        builder.getStringAttr(record.fields.front().first));
    has = has ? arith::AndIOp::create(builder, loc(other.loc), has,
                                      lookup.getFound())
                    .getResult()
              : lookup.getFound();
    bind(name, {Variable::Other, block->getArgument(1), component, mutOther});
  }
  inEdges = true;
  llvm::scope_exit leave([&] { inEdges = false; });
  // (The body where `cond` holds: what it leaves in a var there, or the
  // var as it was.)
  auto parseWhere = [&]() -> LogicalResult {
    if (!where)
      return parseLoopBody();
    FailureOr<mlir::Value> holds = emit(*where, builder.getI1Type());
    if (failed(holds))
      return failure();
    if (!holds->getType().isInteger(1))
      return mismatch(where->loc, holds->getType(), builder.getI1Type());
    auto branch = scf::IfOp::create(builder, loc(where->loc), *holds,
                                    /*withElseRegion=*/true);
    SmallVector<VarState> before = captureVars();
    {
      OpBuilder::InsertionGuard inner(builder);
      builder.setInsertionPoint(branch.thenBlock()->getTerminator());
      if (failed(parseLoopBody()))
        return failure();
    }
    SmallVector<VarState> thenVars = captureVars();
    restoreVars(before);
    branch.getElseRegion().getBlocks().clear();
    mergeBranches(branch, before, thenVars, before);
    return success();
  };
  if (has) {
    // (What the body leaves in a var where the other end has it all, or
    // the var as it was.)
    auto branch = scf::IfOp::create(builder, loc(other.loc), has,
                                    /*withElseRegion=*/true);
    SmallVector<VarState> before = captureVars();
    {
      OpBuilder::InsertionGuard inner(builder);
      builder.setInsertionPoint(branch.thenBlock()->getTerminator());
      if (failed(parseWhere()))
        return failure();
    }
    SmallVector<VarState> thenVars = captureVars();
    restoreVars(before);
    branch.getElseRegion().getBlocks().clear();
    mergeBranches(branch, before, thenVars, before);
  } else if (failed(parseWhere())) {
    return failure();
  }
  SmallVector<mlir::Value> ends;
  for (const VarState &var : captureVars())
    if (ends.size() < vars.size())
      ends.push_back(var.value);
  YieldOp::create(builder, loc(at), ends);
  builder.setInsertionPointAfter(edges);
  // (A var no edge changes holds after the loop what it held before; and
  // where no `break` set the flag, the loop does not carry it.)
  for (auto [k, var, result] : llvm::enumerate(vars, edges->getResults()))
    if (ends[k] != block->getArgument(2 + k))
      var.value = result;
  for (auto [k, var] : llvm::enumerate(vars))
    if (var.name == flag && ends[k] == block->getArgument(2 + k)) {
      Operation *yield = block->getTerminator();
      yield->eraseOperand(k);
      // (Read where a `continue` may end a turn: false all along.)
      {
        OpBuilder::InsertionGuard start(builder);
        builder.setInsertionPointToStart(block);
        block->getArgument(2 + k).replaceAllUsesWith(
            arith::ConstantIntOp::create(builder, loc(at), 0, 1));
      }
      block->eraseArgument(2 + k);
      OperationState again(edges->getLoc(), EdgesOp::getOperationName());
      again.addAttributes(edges->getAttrs());
      again.attributes.erase("break_flag");
      SmallVector<mlir::Value> operands(edges->getOperands());
      unsigned slices = sliceValues.size();
      operands.erase(operands.begin() + slices + k);
      again.addOperands(operands);
      SmallVector<Type> types(edges->getResultTypes());
      types.erase(types.begin() + k);
      again.addTypes(types);
      again.attributes.set(EdgesOp::getOperandSegmentSizesAttrName(again.name),
                           builder.getDenseI32ArrayAttr(
                               {int32_t(slices), int32_t(vars.size() - 1)}));
      Region *region = again.addRegion();
      region->takeBody(edges->getRegion(0));
      Operation *rebuilt = builder.create(again);
      unsigned next = 0;
      for (auto [j, result] : llvm::enumerate(edges->getResults()))
        if (j != k)
          result.replaceAllUsesWith(rebuilt->getResult(next++));
      for (VarState &state : vars)
        for (auto [j, result] : llvm::enumerate(edges->getResults()))
          if (state.value == result)
            state.value = j < k   ? rebuilt->getResult(j)
                          : j > k ? rebuilt->getResult(j - 1)
                                  : state.value;
      edges->erase();
      edges = rebuilt;
      break;
    }
  restoreVars(vars);
  return success();
}

// connect (source)-[Relation { field: value, ... }]->(target), and on:
// ...->(target)<-[Relation]-(another), every arrow an edge.
LogicalResult Parser::parseConnect(llvm::SMLoc at) {
  if (inFunction)
    return error(at, "a fn only computes; a system connects");
  auto parseNode = [&]() -> FailureOr<mlir::Value> {
    if (!token.is(Token::LParen))
      return error("an edge is connected with an arrow: 'connect "
                   "(source)-[Relation]->(target)'");
    advance();
    llvm::SMLoc nodeAt = token.loc;
    FailureOr<ExprPtr> node = parseExpr();
    if (failed(node))
      return failure();
    if (token.is(Token::Comma))
      return error("an edge is connected with an arrow: 'connect "
                   "(source)-[Relation]->(target)'");
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    FailureOr<mlir::Value> value = emit(**node, EntityType::get(context));
    if (failed(value))
      return failure();
    if (!isa<EntityType>(value->getType()))
      return error(nodeAt, "'connect' goes from an entity to an entity");
    return *value;
  };
  FailureOr<mlir::Value> last = parseNode();
  if (failed(last))
    return failure();
  if (!atArrow())
    return error("expected an arrow: 'connect (source)-[Relation]->(target)'");
  while (atArrow()) {
    FailureOr<ArrowStart> arrow = parseArrowStart();
    if (failed(arrow))
      return failure();
    if (arrow->sibling)
      return error(arrow->loc, "'~' is the arrow to the sibling after, which "
                               "the tree's order gives; an edge is connected "
                               "with '-[Relation]->'");
    llvm::SMLoc relationAt = token.loc;
    FailureOr<std::string> relation = identifier("a relation");
    if (failed(relation))
      return failure();
    auto record = relations.find(*relation);
    if (record == relations.end())
      return error(relationAt, "unknown relation '" + *relation + "'");
    // The field values, by name, as in a component's initialiser.
    SmallVector<std::pair<std::string, ExprPtr>> fields;
    if (consumeIf(Token::LBrace)) {
      while (!token.is(Token::RBrace)) {
        FailureOr<std::string> field = identifier("a field");
        if (failed(field) || failed(expect(Token::Colon, "':'")))
          return failure();
        FailureOr<ExprPtr> value = parseExpr();
        if (failed(value))
          return failure();
        fields.push_back({*field, std::move(*value)});
        if (!consumeIf(Token::Comma))
          break;
      }
      if (failed(expect(Token::RBrace, "'}'")))
        return failure();
    }
    if (failed(parseArrowEnd(*arrow)))
      return failure();
    FailureOr<mlir::Value> next = parseNode();
    if (failed(next))
      return failure();
    SmallVector<mlir::Value> values;
    for (auto &[field, type] : record->second.fields) {
      const Expr *value = nullptr;
      for (auto &[name, expr] : fields)
        if (name == field)
          value = expr.get();
      if (!value)
        return error(relationAt, "'" + *relation + "' needs a value for '" +
                                     field + "'");
      FailureOr<mlir::Value> emitted = emit(*value, type);
      if (failed(emitted))
        return failure();
      if (emitted->getType() != type)
        return error(value->loc, "value for '" + field +
                                     "' has a different type than the field");
      values.push_back(*emitted);
    }
    for (auto &[name, expr] : fields)
      if (!record->second.fieldType(name))
        return error(expr->loc, "relation '" + *relation +
                                    "' has no field '" + name + "'");
    if (inEach)
      return error(at, "connecting is done after the 'for' that goes through "
                       "other entities, not in it: that one only reads "
                       "them");
    ConnectOp::create(builder, loc(at), symbol(*relation),
                      arrow->reversed ? *next : *last,
                      arrow->reversed ? *last : *next, values);
    last = *next;
  }
  return success();
}

/// `loop` as a loop that also ends where its carried value `flag` (an i1)
/// is true after a round: an `scf.while` over the induction variable and
/// the carried values. Gives what it ends with, as the loop gave.
static SmallVector<mlir::Value> untilBroken(OpBuilder &builder, scf::ForOp loop,
                                            unsigned flag) {
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(loop);
  Location where = loop.getLoc();
  SmallVector<mlir::Value> inits{loop.getLowerBound()};
  llvm::append_range(inits, loop.getInitArgs());
  SmallVector<Type> types;
  for (mlir::Value init : inits)
    types.push_back(init.getType());
  auto run = scf::WhileOp::create(
      builder, where, types, inits,
      [&](OpBuilder &b, Location l, ValueRange args) {
        mlir::Value counting = arith::CmpIOp::create(
            b, l, arith::CmpIPredicate::slt, args[0], loop.getUpperBound());
        mlir::Value goesOn = arith::AndIOp::create(
            b, l, counting,
            arith::XOrIOp::create(b, l, args[1 + flag],
                                  arith::ConstantIntOp::create(b, l, 1, 1)));
        scf::ConditionOp::create(b, l, goesOn, args);
      },
      [&](OpBuilder &b, Location l, ValueRange args) {
        IRMapping mapping;
        mapping.map(loop.getInductionVar(), args[0]);
        for (auto [arg, value] :
             llvm::zip(loop.getRegionIterArgs(), args.drop_front()))
          mapping.map(arg, value);
        for (Operation &op : loop.getBody()->without_terminator())
          b.clone(op, mapping);
        SmallVector<mlir::Value> ends{
            arith::AddIOp::create(b, l, args[0], loop.getStep())};
        for (mlir::Value yielded :
             loop.getBody()->getTerminator()->getOperands())
          ends.push_back(mapping.lookupOrDefault(yielded));
        scf::YieldOp::create(b, l, ends);
      });
  SmallVector<mlir::Value> results(run.getResults().drop_front());
  loop.replaceAllUsesWith(results);
  loop.erase();
  return results;
}

// for i in a..b { statements }: i takes a, a + 1, ... b - 1, outside a
// `for` over entities.
LogicalResult Parser::parseCountedFor(llvm::SMLoc at) {
  FailureOr<std::string> name = identifier("a name");
  if (failed(name))
    return failure();
  // for number, row in table { }: the row's number (or its case) too.
  std::string numbered;
  llvm::SMLoc numberedAt = token.loc;
  if (consumeIf(Token::Comma)) {
    numbered = *name;
    name = identifier("a name");
    if (failed(name))
      return failure();
  }
  if (failed(expectKeyword("in")))
    return failure();
  Location where = loc(at);
  // for case in Enum { }: every case of it, in their order.
  EnumType cases;
  Type type;
  FailureOr<mlir::Value> low = failure(), high = failure();
  // (Over the rows of a table: which, by its name or the value holding it.)
  bool overRows = false;
  std::string overTable;
  mlir::Value overWhich;
  if (token.is(Token::Identifier) && peek().is(Token::LBrace) &&
      !lookup(token.spelling) && enums.count(token.spelling)) {
    cases = EnumType::get(context, symbol(token.spelling));
    type = cases.getStorageType();
    low = integer(where, builder.getI32Type(), 0);
    high = integer(where, builder.getI32Type(),
                   enums[token.spelling].size());
    advance();
  } else if (FailureOr<ExprPtr> first = parseExpr();
             succeeded(first) && token.is(Token::LBrace)) {
    // for row in table { }: every row of it, in their order; of a table
    // by its name, or of the one a value holds.
    const Expr &over = **first;
    StringRef key;
    if (over.kind == Expr::Name && !lookup(over.name) &&
        tables.count(over.name)) {
      std::string target = resolve(over.name);
      overTable = over.name;
      high = integer(where, builder.getI32Type(), tableRows[target]);
      auto keyed = tableKeys.find(target);
      if (keyed != tableKeys.end())
        key = keyed->second;
    } else if (RowsShape *shape = rowsOf(typeOf(over))) {
      FailureOr<mlir::Value> which = emit(over, typeOf(over));
      if (failed(which))
        return failure();
      overWhich = *which;
      shape->counted = true;
      high = InvokeOp::create(
                 builder, where, builder.getI32Type(),
                 FlatSymbolRefAttr::get(context, "rows_" + shape->name),
                 ValueRange{*which}, UnitAttr())
                 ->getResult(0);
      key = shape->key;
    } else {
      return error(over.loc, "a 'for' counts from a number to another "
                             "('a..b'), or goes over the cases of an enum "
                             "or the rows of a table");
    }
    overRows = true;
    if (!key.empty())
      cases = EnumType::get(context, FlatSymbolRefAttr::get(context, key));
    type = cases ? cases.getStorageType() : Type(builder.getI32Type());
    low = integer(where, builder.getI32Type(), 0);
  } else {
    if (failed(first) || failed(expect(Token::DotDot, "'..'")))
      return failure();
    FailureOr<ExprPtr> end = parseExpr();
    if (failed(end))
      return failure();
    // Literal bounds take the other bound's type; two literals count in
    // i32.
    type = typeOf(**end);
    if (!type)
      type = typeOf(**first);
    if (!type)
      type = builder.getI32Type();
    if (!type.isSignlessInteger() || type.isInteger(1))
      return error((*first)->loc, "a 'for' counts over integers");
    low = emit(**first, type);
    high = emit(**end, type);
    if (failed(low) || failed(high))
      return failure();
    if (low->getType() != type || high->getType() != type)
      return error((*first)->loc, "the bounds of a 'for' must have the same "
                                  "integer type");
  }
  Type index = builder.getIndexType();
  mlir::Value lower = arith::IndexCastOp::create(builder, where, index, *low);
  mlir::Value upper = arith::IndexCastOp::create(builder, where, index, *high);
  mlir::Value step = arith::ConstantIndexOp::create(builder, where, 1);
  // (A `break` in it sets its flag; then it is a loop that ends there, or
  // at the end of the count.)
  ScopeGuard breakScope(*this);
  std::string flag = openBreak(where);
  llvm::scope_exit closeBreak([&] { breakFlags.pop_back(); });
  auto loop = scf::ForOp::create(builder, where, lower, upper, step);
  // Which vars the body assigns is known after it: inside, each var is a
  // placeholder for what it holds when a round starts.
  SmallVector<VarState> before = captureVars(), after;
  SmallVector<Operation *> placeholders;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(loop.getBody()->getTerminator());
    ScopeGuard scope(*this);
    mlir::Value counted = arith::IndexCastOp::create(
        builder, where, type, loop.getInductionVar());
    // (A case: its number, as the enum's type.)
    if (cases)
      counted = UnrealizedConversionCastOp::create(builder, where,
                                                   Type(cases), counted)
                    .getResult(0);
    if (!numbered.empty()) {
      if (!overRows)
        return error(numberedAt, "a 'for' has a number and a row ('for i, "
                                 "row in table') over the rows of a table");
      bind(numbered, Variable::ofValue(counted));
    }
    if (overRows) {
      // (A row with fields is read by them; of a plain list it is the
      // value.)
      Variable row = Variable::ofRow(overTable, overWhich, counted);
      Expr whole = fieldOfRow(row, "", at);
      if (overWhich) {
        whole.kind = Expr::Index;
        std::swap(whole.operands[0], whole.operands[1]);
      }
      if (isWholeRow(whole)) {
        bind(*name, row);
      } else {
        FailureOr<mlir::Value> value = emit(whole, Type());
        if (failed(value))
          return failure();
        bind(*name, Variable::ofValue(*value));
      }
    } else {
      bind(*name, Variable::ofValue(counted));
    }
    SmallVector<VarState> inside = before;
    for (VarState &state : inside) {
      auto placeholder = UnrealizedConversionCastOp::create(
          builder, where, state.value.getType(), state.value);
      placeholders.push_back(placeholder);
      state.value = placeholder.getResult(0);
    }
    restoreVars(inside);
    if (failed(parseLoopBody()))
      return failure();
    after = captureVars();
  }
  restoreVars(before);
  // The vars the body assigned are carried from round to round: what a
  // round ends with the next one starts with, and the loop gives the last.
  SmallVector<unsigned> carried;
  for (auto [i, state] : llvm::enumerate(after))
    if (state.value != placeholders[i]->getResult(0))
      carried.push_back(i);
  if (!carried.empty()) {
    SmallVector<mlir::Value> inits, ends;
    for (unsigned i : carried) {
      inits.push_back(before[i].value);
      ends.push_back(after[i].value);
    }
    auto carrying =
        scf::ForOp::create(builder, where, lower, upper, step, inits);
    Block *from = loop.getBody(), *to = carrying.getBody();
    from->getTerminator()->erase();
    loop.getInductionVar().replaceAllUsesWith(carrying.getInductionVar());
    to->getOperations().splice(to->end(), from->getOperations());
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToEnd(to);
      scf::YieldOp::create(builder, where, ends);
    }
    loop.erase();
    for (auto [k, i] : llvm::enumerate(carried)) {
      placeholders[i]->getResult(0).replaceAllUsesWith(
          carrying.getRegionIterArg(k));
      before[i].value = carrying.getResult(k);
    }
    restoreVars(before);
    for (Operation *placeholder : placeholders) {
      placeholder->getResult(0).replaceAllUsesWith(placeholder->getOperand(0));
      placeholder->erase();
    }
    // (Where a `break` set its flag, a loop that ends there.)
    for (auto [k, i] : llvm::enumerate(carried))
      if (before[i].name == flag) {
        SmallVector<mlir::Value> results = untilBroken(builder, carrying, k);
        for (auto [kk, ii] : llvm::enumerate(carried))
          before[ii].value = results[kk];
        restoreVars(before);
        break;
      }
    return success();
  }
  for (Operation *placeholder : placeholders) {
    placeholder->getResult(0).replaceAllUsesWith(placeholder->getOperand(0));
    placeholder->erase();
  }
  return success();
}

// while condition { statements }: the statements, for as long as the
// condition holds before them (not at all if it does not at first).
LogicalResult Parser::parseWhile(llvm::SMLoc at) {
  Location where = loc(at);
  // (A `break` in it sets its flag, which ends it.)
  ScopeGuard breakScope(*this);
  std::string flag = openBreak(where);
  llvm::scope_exit closeBreak([&] { breakFlags.pop_back(); });
  SmallVector<VarState> vars = captureVars();
  SmallVector<mlir::Value> starts;
  SmallVector<Type> types;
  for (const VarState &state : vars) {
    starts.push_back(state.value);
    types.push_back(state.value.getType());
  }
  Operation *around = builder.getInsertionBlock()->getParentOp();
  while (around && isa<scf::ForOp, scf::WhileOp, scf::IfOp>(around))
    around = around->getParentOp();
  llvm::SaveAndRestore inLoop(
      inSystemLoop, around ? isa<SystemOp>(around) : inSystemLoop);
  LogicalResult parsed = success();
  auto bindTo = [&](ValueRange values) {
    SmallVector<VarState> inside = vars;
    for (auto [state, value] : llvm::zip(inside, values))
      state.value = value;
    restoreVars(inside);
  };
  auto loop = scf::WhileOp::create(
      builder, where, types, starts,
      [&](OpBuilder &, Location, ValueRange arguments) {
        ScopeGuard scope(*this);
        bindTo(arguments);
        mlir::Value goesOn;
        FailureOr<ExprPtr> condition = parseStatementValue();
        FailureOr<mlir::Value> value =
            failed(condition) ? FailureOr<mlir::Value>(failure())
                              : emit(**condition, builder.getI1Type());
        if (failed(value))
          parsed = failure();
        else if (!value->getType().isInteger(1))
          parsed = error((*condition)->loc,
                         "a 'while' condition must be a bool");
        else
          goesOn = arith::AndIOp::create(
              builder, where, *value,
              arith::XOrIOp::create(
                  builder, where, lookup(flag)->value,
                  arith::ConstantIntOp::create(builder, where, 1, 1)));
        if (!goesOn)
          goesOn = arith::ConstantIntOp::create(builder, where, 0, 1);
        scf::ConditionOp::create(builder, where, goesOn, arguments);
      },
      [&](OpBuilder &, Location, ValueRange arguments) {
        ScopeGuard scope(*this);
        bindTo(arguments);
        SmallVector<mlir::Value> ends(arguments.begin(), arguments.end());
        if (succeeded(parsed)) {
          if (failed(parseLoopBody())) {
            parsed = failure();
          } else {
            ends.clear();
            for (const VarState &state : captureVars())
              if (ends.size() < vars.size())
                ends.push_back(state.value);
          }
        }
        scf::YieldOp::create(builder, where, ends);
      });
  if (failed(parsed))
    return failure();
  // (A var no round changes holds after the loop what it held before.)
  Block *after = loop.getAfterBody();
  Operation *yield = after->getTerminator();
  for (auto [k, state, result] : llvm::enumerate(vars, loop.getResults()))
    if (yield->getOperand(k) != after->getArgument(k))
      state.value = result;
  restoreVars(vars);
  return success();
}

// loop { statements } until condition: the statements, again and again
// until, after them, the condition holds. In a system the statements may
// be `for`s over entities, which then run in rounds.
LogicalResult Parser::parseLoop(llvm::SMLoc at) {
  Location where = loc(at);
  // Every var goes round with the loop: what a round leaves in it the
  // next one starts with, and the loop gives what the last one left.
  SmallVector<VarState> vars = captureVars();
  SmallVector<mlir::Value> starts;
  SmallVector<Type> types;
  for (const VarState &state : vars) {
    starts.push_back(state.value);
    types.push_back(state.value.getType());
  }
  Operation *around = builder.getInsertionBlock()->getParentOp();
  while (around && isa<scf::ForOp, scf::WhileOp, scf::IfOp>(around))
    around = around->getParentOp();
  llvm::SaveAndRestore inLoop(
      inSystemLoop, around ? isa<SystemOp>(around) : inSystemLoop);
  LogicalResult parsed = success();
  auto loop = scf::WhileOp::create(
      builder, where, types, starts,
      [&](OpBuilder &, Location, ValueRange arguments) {
        ScopeGuard scope(*this);
        SmallVector<VarState> inside = vars;
        for (auto [state, argument] : llvm::zip(inside, arguments))
          state.value = argument;
        restoreVars(inside);
        mlir::Value done;
        // (What the statements name, the condition after them can ask.)
        if (failed(parseBlock(/*ownScope=*/false))) {
          parsed = failure();
        } else if (!consumeKeyword("until")) {
          parsed = error("a 'loop' ends with 'until' and what is to hold "
                         "then: 'loop { ... } until done'");
        } else {
          FailureOr<ExprPtr> condition = parseStatementValue();
          FailureOr<mlir::Value> value =
              failed(condition) ? FailureOr<mlir::Value>(failure())
                                : emit(**condition, builder.getI1Type());
          if (failed(value))
            parsed = failure();
          else if (!value->getType().isInteger(1))
            parsed = error((*condition)->loc,
                           "an 'until' condition must be a bool");
          else
            done = *value;
        }
        SmallVector<mlir::Value> ends;
        if (succeeded(parsed)) {
          for (const VarState &state : captureVars())
            if (ends.size() < vars.size())
              ends.push_back(state.value);
        } else {
          ends.assign(arguments.begin(), arguments.end());
          done = arith::ConstantIntOp::create(builder, where, 1, 1);
        }
        scf::ConditionOp::create(
            builder, where,
            arith::XOrIOp::create(
                builder, where, done,
                arith::ConstantIntOp::create(builder, where, 1, 1)),
            ends);
      },
      [&](OpBuilder &, Location, ValueRange arguments) {
        scf::YieldOp::create(builder, where, arguments);
      });
  if (failed(parsed))
    return failure();
  for (auto [state, result] : llvm::zip(vars, loop.getResults()))
    state.value = result;
  restoreVars(vars);
  return success();
}

// var name [: type] = value
LogicalResult Parser::parseVar() {
  FailureOr<std::string> name = identifier("a name");
  if (failed(name))
    return failure();
  Type type;
  if (consumeIf(Token::Colon)) {
    FailureOr<Type> declared = parseType();
    if (failed(declared))
      return failure();
    type = *declared;
  }
  if (failed(expect(Token::Assign, "'=': a var starts with a value")))
    return failure();

  FailureOr<ExprPtr> value = parseStatementValue();
  if (failed(value))
    return failure();
  FailureOr<mlir::Value> emitted = emit(**value, type);
  if (failed(emitted))
    return failure();
  if (type && emitted->getType() != type)
    return error((*value)->loc, "value has a different type than the var");
  if (isa<StringType>(emitted->getType()))
    return error((*value)->loc, "a var does not hold a text of any length; "
                                "one of a capacity it does ('as text[N]')");
  bind(*name, Variable::ofVar(*emitted));
  return success();
}

/// Every `var` in scope with what it holds here, outermost first.
SmallVector<VarState> Parser::captureVars() {
  SmallVector<VarState> states;
  for (auto [index, scope] : llvm::enumerate(scopes)) {
    SmallVector<StringRef> names;
    for (auto &entry : scope)
      if (entry.second.isVar)
        names.push_back(entry.first());
    llvm::sort(names);
    for (StringRef name : names)
      states.push_back({unsigned(index), name.str(), scope[name].value});
  }
  return states;
}

void Parser::restoreVars(ArrayRef<VarState> states) {
  for (const VarState &state : states)
    scopes[state.scope][state.name].value = state.value;
}

/// After an `if` whose branches left the vars as `thenVars` and `elseVars`
/// (from `before`): a var a branch assigned holds, from here on, what the
/// branch that ran left in it, which the `if` gives as a result.
void Parser::mergeBranches(scf::IfOp branch, ArrayRef<VarState> before,
                           ArrayRef<VarState> thenVars,
                           ArrayRef<VarState> elseVars) {
  SmallVector<unsigned> assigned;
  SmallVector<mlir::Value> thenValues, elseValues;
  for (unsigned i = 0, e = before.size(); i < e; ++i) {
    if (thenVars[i].value == before[i].value &&
        elseVars[i].value == before[i].value)
      continue;
    assigned.push_back(i);
    thenValues.push_back(thenVars[i].value);
    elseValues.push_back(elseVars[i].value);
  }
  if (assigned.empty())
    return;
  scf::IfOp merged = giveFromBranches(branch, thenValues, elseValues);
  for (auto [k, i] : llvm::enumerate(assigned))
    scopes[before[i].scope][before[i].name].value = merged.getResult(k);
}

/// Replace `branch`, an `if` without results that was just made, by one
/// with the same branches that gives `thenValues` or `elseValues`.
scf::IfOp Parser::giveFromBranches(scf::IfOp branch,
                                   ArrayRef<mlir::Value> thenValues,
                                   ArrayRef<mlir::Value> elseValues) {
  Location at = branch.getLoc();
  SmallVector<Type> types;
  for (mlir::Value value : thenValues)
    types.push_back(value.getType());
  auto merged = scf::IfOp::create(builder, at, types, branch.getCondition(),
                                  /*withElseRegion=*/true);
  auto fill = [&](Block *from, Block *to, ArrayRef<mlir::Value> values) {
    if (from) {
      from->getTerminator()->erase();
      to->getOperations().splice(to->end(), from->getOperations());
    }
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(to);
    scf::YieldOp::create(builder, at, values);
  };
  fill(branch.thenBlock(), merged.thenBlock(), thenValues);
  fill(branch.getElseRegion().empty() ? nullptr : branch.elseBlock(),
       merged.elseBlock(), elseValues);
  branch.erase();
  return merged;
}

// for [e,] [p: [mut] P [up R], ...] [with A, any(B, C)] [without D]
//     [top down R | bottom up R | [in turn] by name.field] [where cond]
//     [on trigger, ...] { }
LogicalResult Parser::parseFor() {
  llvm::SMLoc at = token.loc;
  // for [e,] c in name { }, over the entities of a component that was
  // given: a `for` over entities, as one inside another is.
  if (atCountedFor()) {
    Lexer ahead = lexer;
    Token next = ahead.next();
    if (next.is(Token::Comma)) {
      ahead.next();
      next = ahead.next();
    }
    next = ahead.next();
    if (next.is(Token::Identifier) && componentGiven(next.spelling))
      return parseEach(at);
  }
  if (atCountedFor()) {
    return parseCountedFor(at);
  }
  // Inside a `for` over entities: its edges, with an arrow, or other
  // entities, for each of which the body runs.
  if (inQuery && !token.is(Token::LParen))
    return parseEach(at);
  if (inQuery)
    return parseEdges(at);
  if (inFunction)
    return error(at, "a fn only computes: it counts ('for i in a..b'), and "
                     "a system visits entities");
  // In a system, or in a counted `for` or a `loop` of one, which runs it
  // so many times, or in an `if` there, which runs it or not.
  Operation *around = builder.getInsertionBlock()->getParentOp();
  while (around && isa<scf::ForOp, scf::WhileOp, scf::IfOp>(around))
    around = around->getParentOp();
  if (around ? !isa<SystemOp>(around) : !inSystemLoop)
    return error(at, "a 'for' over entities is at the top level of a system, "
                     "or in a counted 'for', a 'loop' or an 'if' there");

  struct Binding {
    std::string name, component;
    bool mut;
    /// `up R`: the component is an ancestor's along this tree; `before
    /// R`: that of the sibling before in it.
    FlatSymbolRefAttr via;
    bool before = false;
    /// `optional`: entities without that ancestor or sibling are visited
    /// too, and the binding is read with `if let`.
    bool optional = false;
    /// The parent's itself (`-[R]->`), not the nearest ancestor's that
    /// has the component (`-[R*]->`).
    bool direct = false;
    /// The sibling after's (`(name)~[R]~>(next: C)`).
    bool after = false;
    /// How many arrows up: `(name)-[R]->()-[R]->(far: C)` is two.
    unsigned hops = 1;
    /// Or the steps to it, where they are not all to a parent along one
    /// tree (see RefType).
    ArrayAttr path = {};
  };
  // A relation `up` or `cascade` follows: a tree. (`treeIsOrdered`:
  // whether the one last parsed has its entities in an order.)
  bool treeIsOrdered = false;
  std::string treeEnds[2];
  auto parseTree = [&](StringRef word) -> FailureOr<FlatSymbolRefAttr> {
    llvm::SMLoc relationAt = token.loc;
    FailureOr<std::string> relation = identifier("a relation");
    if (failed(relation))
      return failure();
    auto record = relations.find(*relation);
    if (record == relations.end())
      return error(relationAt, "unknown relation '" + *relation + "'");
    treeIsOrdered = record->second.ordered;
    treeEnds[0] = record->second.ends[0];
    treeEnds[1] = record->second.ends[1];
    if (!record->second.tree)
      return error(relationAt, word.str() + " follows a tree, and '" +
                                   *relation + "' is not declared one "
                                   "('relation ... tree capacity N')");
    return symbol(*relation);
  };
  SmallVector<Binding> bindings;
  std::string entity;
  auto startsFilter = [&] {
    return token.isKeyword("with") || token.isKeyword("without");
  };
  // Windows: `w: [mut] Buffer[start, count]`, rows of a buffer the
  // visited entity has, read (and with `mut` taken and written) through
  // `w[i].field`; start and count are emitted where the body starts.
  struct Window {
    std::string name, symbol;
    bool mut;
    ExprPtr start, count;
    llvm::SMLoc loc;
  };
  SmallVector<Window> windows;
  // name: [mut] Component, one or more, into `into`; the first name was
  // read (`pending`).
  // `optional` before a name: the entity may be without the component,
  // and the binding is read with `if let`. With `windowsHere` a name may
  // be a window too (of the visited entity's own bindings, not in a
  // pattern).
  auto parseBindings = [&](std::string pending,
                           SmallVectorImpl<Binding> &into,
                           bool optional = false,
                           bool windowsHere = false) -> LogicalResult {
    while (true) {
      if (failed(expect(Token::Colon, "':' and a component")))
        return failure();
      llvm::SMLoc mutAt = token.loc;
      bool mut = consumeKeyword("mut");
      if (mut && optional)
        return error(mutAt, "a component the entity may be without is only "
                            "read: to change it, visit the entities that "
                            "have it in a 'for' of their own");
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component))
        return failure();
      if (token.is(Token::LBracket)) {
        // name: [mut] Buffer[start, count]: a window.
        std::optional<BufferUse> held = bufferNamed(*component);
        if (!held || held->given)
          return error(componentAt, "'" + *component + "' is not a buffer "
                                    "of the program: a window is 'name: mut "
                                    "Buffer[start, count]'");
        if (mut && !held->mut)
          return error(componentAt, "'" + *component + "' is an index: its "
                                    "rows are only read");
        if (!windowsHere)
          return error(componentAt, "a window is a binding of the entity "
                                    "a 'for' visits, without a pattern: "
                                    "'for c: Colony, w: mut " + *component +
                                        "[start, count]'");
        if (optional)
          return error(componentAt, "a window is not optional: the entity "
                                    "has its rows");
        for (const Window &other : windows)
          if (other.symbol == held->symbol)
            return error(componentAt, "'" + *component + "' has a window "
                                      "in this 'for' already: an entity has "
                                      "one window of a buffer");
        advance();
        FailureOr<ExprPtr> start = parseExpr();
        if (failed(start) ||
            failed(expect(Token::Comma, "',' and how many rows: '[start, "
                                        "count]'")))
          return failure();
        FailureOr<ExprPtr> count = parseExpr();
        if (failed(count) || failed(expect(Token::RBracket, "']'")))
          return failure();
        windows.push_back({pending, held->symbol, mut, std::move(*start),
                           std::move(*count), componentAt});
        if (!consumeIf(Token::Comma))
          return success();
        optional = consumeKeyword("optional");
        FailureOr<std::string> next = identifier("a binding");
        if (failed(next))
          return failure();
        pending = *next;
        continue;
      }
      if (!components.count(*component))
        return error(componentAt, "unknown component '" + *component + "'");
      if (token.isKeyword("up") || token.isKeyword("before"))
        return error("another entity's component is bound with an arrow: "
                     "'for (b: Box)-[Relation]->(outer: Box)' for the "
                     "parent's, '-[Relation*]->' for the nearest ancestor's "
                     "that has it, '(prev: Box)~[Relation]~>(b)' for the "
                     "sibling before's");
      into.push_back({pending, *component, mut, {}});
      into.back().optional = optional;
      if (!consumeIf(Token::Comma))
        return success();
      optional = consumeKeyword("optional");
      FailureOr<std::string> next = identifier("a binding");
      if (failed(next))
        return failure();
      pending = *next;
    }
  };
  // A node of a pattern: `(name)`, an entity named before; `(e, a: A, b:
  // B)`, an entity (named or not) and components of it; `()`.
  struct Node {
    llvm::SMLoc loc;
    std::string name; // the entity's, or the one name of a bare node
    bool bare = false;
    SmallVector<Binding> bindings;
  };
  auto parseNode = [&]() -> FailureOr<Node> {
    Node node;
    node.loc = token.loc;
    if (failed(expect(Token::LParen, "'(' and a node of the pattern")))
      return failure();
    if (consumeIf(Token::RParen))
      return node;
    FailureOr<std::string> first = identifier("a name");
    if (failed(first))
      return failure();
    if (token.is(Token::RParen)) {
      node.name = *first;
      node.bare = true;
    } else {
      std::string pending = *first;
      bool optional = false;
      if (consumeIf(Token::Comma)) {
        node.name = pending;
        optional = consumeKeyword("optional");
        FailureOr<std::string> next = identifier("a binding");
        if (failed(next))
          return failure();
        pending = *next;
      }
      if (failed(parseBindings(pending, node.bindings, optional)))
        return failure();
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    return node;
  };
  bool patterns =
      token.is(Token::LParen) ||
      (token.isKeyword("optional") && peek().is(Token::LParen));
  if (!patterns && !startsFilter()) {
    // The entity's own: `for e`, `for e, a: A`, `for a: A, b: mut B`. The
    // first name is the entity if no ':' follows it.
    FailureOr<std::string> first = identifier("a binding");
    if (failed(first))
      return failure();
    std::string pending = *first;
    bool hasBindings = true, firstOptional = false;
    if (startsFilter()) {
      entity = pending;
      hasBindings = false;
    } else if (consumeIf(Token::Comma)) {
      entity = pending;
      if (token.is(Token::LParen) ||
          (token.isKeyword("optional") && peek().is(Token::LParen)))
        return error("the entity a 'for' visits is the first node of its "
                     "pattern: 'for (" + entity + ", b: Box)-[Relation]->"
                     "(outer: Box)'");
      firstOptional = consumeKeyword("optional");
      FailureOr<std::string> next = identifier("a binding");
      if (failed(next))
        return failure();
      pending = *next;
    }
    if (hasBindings &&
        failed(parseBindings(pending, bindings, firstOptional,
                             /*windowsHere=*/true)))
      return failure();
  }
  // Patterns: the first node of the first is the entity the `for`
  // visits; an arrow leads from it to its parent (`-[R]->`, `-[R*]->`
  // for the nearest ancestor that has what is bound), or to it from the
  // sibling before (`~[R]~>`). Later patterns name the visited entity by
  // one of its names.
  llvm::StringSet<> ownNames;
  // The entities at other ends that are named, and a binding of each to
  // reach it by.
  SmallVector<std::pair<std::string, unsigned>> otherNames;
  bool first = true;
  while (patterns) {
    llvm::SMLoc patternAt = token.loc;
    bool optional = consumeKeyword("optional");
    FailureOr<Node> left = parseNode();
    if (failed(left))
      return failure();
    if (first) {
      if (left->bare)
        entity = left->name;
      else
        entity = left->name;
      for (Binding &binding : left->bindings)
        bindings.push_back(binding);
      if (!entity.empty())
        ownNames.insert(entity);
      for (Binding &binding : left->bindings)
        ownNames.insert(binding.name);
    }
    if (!atArrow()) {
      if (!first)
        return error(patternAt, "a pattern after the first leads to another "
                                "entity: '(name)-[Relation]->(outer: C)'");
      if (optional)
        return error(patternAt, "the entity a 'for' visits is there: only a "
                                "pattern to another entity can be optional");
    } else {
      FailureOr<ArrowStart> arrow = parseArrowStart();
      if (failed(arrow))
        return failure();
      FailureOr<FlatSymbolRefAttr> relation =
          parseTree(arrow->sibling ? "the arrow from a sibling" : "an arrow in the head of a 'for'");
      if (failed(relation))
        return failure();
      bool ordered = treeIsOrdered;
      bool nearest = consumeIf(Token::Star);
      if (failed(parseArrowEnd(*arrow)))
        return failure();
      FailureOr<Node> right = parseNode();
      if (failed(right))
        return failure();
      // Which end is the visited entity.
      bool leftIsOwn = first;
      if (!first) {
        bool leftOwn = left->bare && ownNames.contains(left->name);
        bool rightOwn = right->bare && ownNames.contains(right->name);
        if (leftOwn == rightOwn)
          return error(patternAt,
                       "one end of a pattern is the entity the 'for' visits, "
                       "by one of its names, and the other is what is bound "
                       "of another: '(name)-[Relation]->(outer: C)'");
        leftIsOwn = leftOwn;
      }
      // Does the arrow point from the visited entity?
      bool fromOwn = leftIsOwn != arrow->reversed;
      std::string relationName = relation->getValue().str();
      if (arrow->sibling) {
        if (!ordered)
          return error(arrow->loc,
                       "the entities of '" + relationName +
                           "' are in no order: 'relation ... tree ordered by "
                           "C.f capacity N'");
        if (nearest)
          return error(arrow->loc, "'*' is for ancestors: a sibling is the "
                                   "one before or the one after");
      } else if (!fromOwn) {
        return error(arrow->loc,
                     "an arrow to the entity a 'for' visits comes from its "
                     "children, of which there are many: they are visited in "
                     "a loop in the body, 'for (child: C)-[" + relationName +
                         "]->(name) { ... }'");
      }
      // The arrows of the pattern and the nodes they lead to: one, or,
      // up a tree from the visited entity, several, each a step from the
      // node before it, along any tree, to the parent or (`*`) to the
      // nearest ancestor that has what its node binds.
      // (Or, along a tree whose children are in an order, to the sibling
      // after or before: `sibling` 1 or -1.)
      struct Link {
        FlatSymbolRefAttr relation;
        std::string end; // what the relation says its targets have
        bool nearest;
        Node node;
        int sibling = 0;
      };
      SmallVector<Link, 2> links;
      links.push_back({*relation, treeEnds[arrow->sibling ? 0 : 1], nearest,
                       leftIsOwn ? *right : *left,
                       arrow->sibling ? (fromOwn ? 1 : -1) : 0});
      while (atArrow()) {
        FailureOr<ArrowStart> next = parseArrowStart();
        if (failed(next))
          return failure();
        llvm::SMLoc nextAt = token.loc;
        FailureOr<FlatSymbolRefAttr> nextRelation =
            parseTree("an arrow in the head of a 'for'");
        if (failed(nextRelation))
          return failure();
        if (!leftIsOwn || (!next->sibling && next->reversed))
          return error(nextAt, "arrows go on from the visited entity, up "
                               "or to a sibling: '(name)-[R]->(mid: C)-[R]->"
                               "(far: C)', '(name)-[R]->(mid: C)~[R]~>(next: "
                               "C)'");
        if (next->sibling && !treeIsOrdered)
          return error(nextAt,
                       "the entities of '" + nextRelation->getValue().str() +
                           "' are in no order: 'relation ... tree ordered by "
                           "C.f capacity N'");
        std::string end = treeEnds[next->sibling ? 0 : 1];
        bool star = consumeIf(Token::Star);
        if (star && next->sibling)
          return error(nextAt, "'*' is for ancestors: a sibling is the one "
                               "before or the one after");
        if (failed(parseArrowEnd(*next)))
          return failure();
        FailureOr<Node> further = parseNode();
        if (failed(further))
          return failure();
        links.push_back({*nextRelation, end, star, *further,
                         next->sibling ? (next->reversed ? -1 : 1) : 0});
      }
      // A node that only names its entity is reached by a component the
      // relation says that end has.
      for (Link &link : links) {
        Node &node = link.node;
        if (node.name.empty() || !node.bindings.empty())
          continue;
        if (link.end.empty())
          return error(node.loc,
                       "'" + link.relation.getValue().str() +
                           "' does not say what the entity at this end has; "
                           "bind a component of it to reach it by: '(" +
                           node.name + ", c: C)'");
        node.bindings.push_back({"$" + node.name, link.end, false, {}});
      }
      Node &last = links.back().node;
      if (last.bindings.empty())
        return error(last.loc, "the other end of an arrow binds what is "
                               "read of it, or names it: '(outer: C)', "
                               "'(parent)'");
      // Plain arrows along one tree are so many steps to a parent. With a
      // `*` among several arrows or to a node that binds several
      // components, or with more than one tree, the way is a path: its
      // steps, and for `*` what the ancestor is to have.
      SmallVector<Attribute> steps;
      bool plain = true;
      for (auto [index, link] : llvm::enumerate(links)) {
        Node &node = link.node;
        if (link.nearest && node.bindings.empty())
          return error(node.loc, "'*' leads to the nearest ancestor that "
                                 "has what its node binds, and this one "
                                 "binds nothing");
        SmallVector<Attribute, 4> step{
            builder.getStringAttr(link.sibling > 0   ? "after"
                                  : link.sibling < 0 ? "before"
                                  : link.nearest     ? "up"
                                                     : "parent"),
            link.relation};
        if (link.nearest)
          for (Binding &binding : node.bindings)
            step.push_back(symbol(binding.component));
        steps.push_back(builder.getArrayAttr(step));
        plain &= link.relation == *relation &&
                 (!link.nearest ||
                  (links.size() == 1 && node.bindings.size() == 1)) &&
                 (!link.sibling || links.size() == 1);
        for (Binding &binding : node.bindings) {
          if (link.sibling && binding.mut)
            return error(node.loc, "a sibling is only read");
          if (binding.optional)
            return error(node.loc, "what another entity may be without is "
                                   "said before the pattern: 'optional "
                                   "(name)-[Relation]->(outer: C)'");
          binding.via = *relation;
          binding.optional = optional;
          if (plain) {
            binding.before = arrow->sibling && !fromOwn;
            binding.after = arrow->sibling && fromOwn;
            binding.direct = !arrow->sibling && !link.nearest;
            binding.hops = index + 1;
          } else {
            if (binding.mut)
              return error(node.loc, "what a path of several arrows or a "
                                     "'*' to several components leads to "
                                     "is only read");
            binding.direct = true;
            binding.path = builder.getArrayAttr(steps);
          }
          bindings.push_back(binding);
        }
        if (!node.name.empty())
          otherNames.push_back({node.name, unsigned(bindings.size() - 1)});
      }
    }
    first = false;
    if (!consumeIf(Token::Comma))
      break;
  }
  auto parseComponent = [&]() -> FailureOr<Attribute> {
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component))
      return failure();
    if (!components.count(*component))
      return error(componentAt, "unknown component '" + *component + "'");
    return Attribute(symbol(*component));
  };
  // with A, any(B, C)   without D, E
  SmallVector<Attribute> with, without, anyGroups;
  if (consumeKeyword("with")) {
    do {
      if (token.isKeyword("any")) {
        advance();
        if (failed(expect(Token::LParen, "'('")))
          return failure();
        SmallVector<Attribute> group;
        do {
          FailureOr<Attribute> component = parseComponent();
          if (failed(component))
            return failure();
          group.push_back(*component);
        } while (consumeIf(Token::Comma));
        if (failed(expect(Token::RParen, "')'")))
          return failure();
        anyGroups.push_back(builder.getArrayAttr(group));
        continue;
      }
      FailureOr<Attribute> component = parseComponent();
      if (failed(component))
        return failure();
      with.push_back(*component);
    } while (consumeIf(Token::Comma));
  }
  if (consumeKeyword("without")) {
    do {
      FailureOr<Attribute> component = parseComponent();
      if (failed(component))
        return failure();
      without.push_back(*component);
    } while (consumeIf(Token::Comma));
  }
  // top down Relation / bottom up Relation: along a tree, parents before
  // their children or after them; with `bfs` or `dfs` before the relation,
  // in exactly that order.
  FlatSymbolRefAttr cascade;
  bool leavesFirst = false;
  StringRef traversal;
  if (token.isKeyword("cascade"))
    return error("the order along a tree is 'top down Relation' (parents "
                 "first) or 'bottom up Relation' (children first), with "
                 "'bfs' or 'dfs' before the relation where the order is to "
                 "be exactly that");
  bool topDown = token.isKeyword("top") && peek().isKeyword("down");
  bool bottomUp = token.isKeyword("bottom") && peek().isKeyword("up");
  if (topDown || bottomUp) {
    advance();
    advance();
    leavesFirst = bottomUp;
    for (StringRef kind : {"bfs", "dfs"})
      if (consumeKeyword(kind))
        traversal = kind;
    FailureOr<FlatSymbolRefAttr> relation =
        parseTree(bottomUp ? "'bottom up'" : "'top down'");
    if (failed(relation))
      return failure();
    cascade = *relation;
  }
  // in Buffer[.field]: the entities the buffer's rows name (by its field
  // of entities, the one it has or the one named), in the rows' order.
  Attribute listed;
  FlatSymbolRefAttr inSet;
  llvm::SMLoc inSetAt;
  if (token.isKeyword("in") && !peek().isKeyword("turn") &&
      peek().is(Token::Identifier) && sets.count(peek().spelling)) {
    // in Set: those of its entities the set holds, in no order the
    // program sees (a query like any other).
    inSetAt = token.loc;
    advance();
    inSet = symbol(token.spelling);
    advance();
    if (cascade)
      return error(inSetAt, "a 'for' goes along a tree or over the "
                            "entities of a set: not both");
  }
  if (!inSet && token.isKeyword("in") && !peek().isKeyword("turn")) {
    llvm::SMLoc inAt = token.loc;
    advance();
    llvm::SMLoc bufferAt = token.loc;
    FailureOr<std::string> buffer = identifier("a buffer");
    if (failed(buffer))
      return failure();
    if (!buffers.count(*buffer))
      return error(bufferAt, "a 'for' visits the entities the rows of a "
                             "buffer name: '" +
                                 *buffer + "' is no buffer");
    const Record &record = buffers[*buffer];
    std::string field;
    if (consumeIf(Token::Dot)) {
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> named = identifier("a field");
      if (failed(named))
        return failure();
      Type type = record.fieldType(*named);
      if (!type || !isa<EntityType>(type))
        return error(fieldAt, "buffer '" + *buffer + "' has no field '" +
                                  *named + "' of entities");
      field = *named;
    } else {
      for (auto &[name, type] : record.fields)
        if (isa<EntityType>(type)) {
          if (!field.empty())
            return error(bufferAt, "buffer '" + *buffer +
                                       "' has more than "
                                       "one field of entities: say which, '" +
                                       *buffer + "." + field + "'");
          field = name;
        }
      if (field.empty())
        return error(bufferAt, "buffer '" + *buffer +
                                   "' has no field of "
                                   "entities to name the entities by");
    }
    if (cascade)
      return error(inAt, "a 'for' goes along a tree or over the entities a "
                         "buffer lists: it has one order");
    listed =
        builder.getArrayAttr({symbol(*buffer), builder.getStringAttr(field)});
  }
  // [in turn] by name.field: in the order of a whole number the entities
  // have, one after another; `in turn`, each as a `for` of its own.
  bool turn = false;
  llvm::StringMap<mlir::Value> slice;
  SmallVector<Attribute> sliceOrder;
  SmallVector<mlir::Value> sliceValues;
  Attribute by;
  llvm::SMLoc byAt = token.loc;
  if (token.isKeyword("in") && peek().isKeyword("turn")) {
    advance();
    advance();
    turn = true;
    if (inSet)
      return error(inSetAt, "the entities of a set take no turns: they "
                            "have no order to take them in (a buffer of "
                            "them does, 'in Name in turn')");
    if (!listed && !token.isKeyword("by"))
      return error("'in turn' needs an order: 'in turn by name.field', a "
                   "whole number the entities have, or the rows of a "
                   "buffer, 'in Name in turn'");
  }
  if (consumeKeyword("by")) {
    if (cascade)
      return error(byAt, "a 'for' goes along a tree or by a field: it has "
                         "one order");
    if (listed)
      return error(byAt, "a 'for' over the entities a buffer lists goes in "
                         "the order of its rows");
    if (inSet)
      return error(byAt, "a 'for' over the entities of a set goes by a "
                         "field: not yet");
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> name = identifier("a component or a binding");
    if (failed(name))
      return failure();
    const Binding *bound = nullptr;
    for (const Binding &binding : bindings)
      if (binding.name == *name)
        bound = &binding;
    std::string component = bound ? bound->component : *name;
    if (!bound && !components.count(component))
      return error(componentAt, "unknown component or binding '" + *name +
                                    "'");
    if (bound && (bound->via || bound->optional))
      return error(componentAt, "the order is by a field every entity of "
                                "the 'for' has itself");
    if (!bound && !llvm::is_contained(with, Attribute(symbol(component))))
      return error(componentAt, "the order is by a field every entity of "
                                "the 'for' has: bind '" + component +
                                    "' or name it after 'with'");
    if (failed(expect(Token::Dot, "'.' and a field: the order is by a "
                                  "whole number")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = components[component].fieldType(*field);
    if (!type)
      return error(fieldAt, "component '" + component + "' has no field '" +
                                *field + "'");
    if ((!type.isIntOrIndex() || type.isInteger(1)) && !type.isF32() &&
        !type.isF64())
      return error(fieldAt, "the order is by a whole number or a fraction "
                            "(f32, f64); '" +
                                *field + "' is " + typeWords(type));
    by = builder.getArrayAttr(
        {symbol(component), builder.getStringAttr(*field)});
    for (const Binding &binding : bindings)
      if (binding.via)
        return error(byAt, "a 'for' that goes by a field binds what its "
                           "own entities have, not along a tree");
    // A slice of the order: `from a` and `to b` (a <= value < b), then
    // `first k` or `last k`; worked out before the `for` starts.
    for (StringRef part : {"from", "to", "first", "last"}) {
      llvm::SMLoc partAt = token.loc;
      if (!consumeKeyword(part))
        continue;
      if (turn)
        return error(partAt, "a 'for' in turn visits all its entities: no '" +
                                 part.str() + "' yet");
      if (part == "last" && slice.count("first"))
        return error(partAt, "'first' or 'last': one of them");
      Type wanted =
          part == "from" || part == "to" ? type : Type(builder.getI32Type());
      FailureOr<ExprPtr> bound = parseExpr();
      if (failed(bound))
        return failure();
      FailureOr<mlir::Value> value = emit(**bound, wanted);
      if (failed(value))
        return failure();
      if (value->getType() != wanted)
        return mismatch((*bound)->loc, value->getType(), wanted);
      slice[part] = *value;
      sliceOrder.push_back(builder.getStringAttr(part));
      sliceValues.push_back(*value);
    }
  }
  ExprPtr where;
  if (consumeKeyword("where")) {
    FailureOr<ExprPtr> condition = parseExpr();
    if (failed(condition))
      return failure();
    where = std::move(*condition);
  }
  SmallVector<Attribute> triggers;
  if (by && token.isKeyword("on"))
    return error("a 'for' that goes by a field does not react ('on') yet");
  if (consumeKeyword("on")) {
    do {
      llvm::SMLoc triggerAt = token.loc;
      FailureOr<std::string> kind = identifier("a trigger");
      if (failed(kind))
        return failure();
      if (*kind != "added" && *kind != "removed" && *kind != "changed")
        return error(triggerAt, "unknown trigger '" + *kind +
                                    "'; expected added, removed or changed");
      // What had the event: a component of the visited entity, by its
      // name or by a binding of it; of another entity, by its binding
      // (`changed outer.x`: the direction is the binding's); or of any
      // child, by a pattern, `changed (: C.f)-[R]->(name)`.
      SmallVector<Attribute, 4> entry;
      if (token.is(Token::LParen)) {
        llvm::SMLoc patternAt = token.loc;
        if (*kind != "changed")
          return error(patternAt, "only 'changed' can be asked of a child");
        advance();
        if (failed(expect(Token::Colon, "':' and a component: the event of "
                                        "a child is written '(: C.f)-[R]->"
                                        "(name)'")))
          return failure();
        llvm::SMLoc componentAt = token.loc;
        FailureOr<std::string> component = identifier("a component");
        if (failed(component))
          return failure();
        if (!components.count(*component))
          return error(componentAt, "unknown component '" + *component + "'");
        std::string field;
        if (consumeIf(Token::Dot)) {
          FailureOr<std::string> name = identifier("a field");
          if (failed(name))
            return failure();
          field = *name;
        }
        if (failed(expect(Token::RParen, "')'")))
          return failure();
        FailureOr<ArrowStart> arrow = parseArrowStart();
        if (failed(arrow))
          return failure();
        if (arrow->sibling || arrow->reversed)
          return error(arrow->loc, "the event of a child is written '(: "
                                   "C.f)-[R]->(name)'");
        FailureOr<FlatSymbolRefAttr> relation = parseTree("the arrow from a child");
        if (failed(relation) || failed(parseArrowEnd(*arrow)) ||
            failed(expect(Token::LParen, "'('")))
          return failure();
        llvm::SMLoc ownAt = token.loc;
        FailureOr<std::string> own = identifier("the visited entity");
        if (failed(own) || failed(expect(Token::RParen, "')'")))
          return failure();
        if (!ownNames.contains(*own) && *own != entity &&
            llvm::none_of(bindings, [&](const Binding &binding) {
              return !binding.via && binding.name == *own;
            }))
          return error(ownAt, "'" + *own + "' is not a name of the entity "
                              "the 'for' visits");
        entry = {builder.getStringAttr(*kind), symbol(*component),
                 builder.getStringAttr(field), builder.getStringAttr("down"),
                 *relation};
      } else {
        llvm::SMLoc componentAt = token.loc;
        FailureOr<std::string> name = identifier("a component or a binding");
        if (failed(name))
          return failure();
        const Binding *bound = nullptr;
        for (const Binding &binding : bindings)
          if (binding.name == *name)
            bound = &binding;
        std::string component = bound ? bound->component : *name;
        if (!bound && !components.count(component))
          return error(componentAt, "unknown component or binding '" + *name +
                                        "'");
        std::string field;
        if (*kind == "changed" && consumeIf(Token::Dot)) {
          FailureOr<std::string> fieldName = identifier("a field");
          if (failed(fieldName))
            return failure();
          field = *fieldName;
        }
        entry = {builder.getStringAttr(*kind), symbol(component),
                 builder.getStringAttr(field)};
        if (bound && bound->via) {
          if (*kind != "changed")
            return error(componentAt, "only 'changed' can be asked of "
                                      "another entity");
          if (bound->after)
            entry.push_back(builder.getStringAttr("after"));
          if (bound->before)
            entry.push_back(builder.getStringAttr("before"));
          entry.push_back(bound->via);
          // (Further than one arrow: which of the bindings up the tree.)
          if (bound->path)
            entry.push_back(bound->path);
          else if (bound->hops != 1)
            entry.push_back(builder.getArrayAttr(
                {builder.getI64IntegerAttr(bound->hops)}));
        }
        if (token.isKeyword("up") || token.isKeyword("down") ||
            token.isKeyword("before"))
          return error("another entity's event is asked by its binding "
                       "('changed outer.x'), a child's by a pattern "
                       "('changed (: C.f)-[R]->(name)')");
      }
      if (consumeKeyword("log")) {
        FailureOr<int64_t> capacity = integer("a log capacity");
        if (failed(capacity))
          return failure();
        entry.push_back(builder.getI64IntegerAttr(*capacity));
      }
      triggers.push_back(builder.getArrayAttr(entry));
    } while (consumeIf(Token::Comma));
  }

  // where b.key == value (or one of the conditions `&&` joins): the value
  // worked out once, before the `for`, where it is the same for every
  // entity (literals, consts and locals not declared `var`), so that a
  // hash index by that field can say which entities have it (see
  // QueryOp::kKeyEqualsAttr); the `where` stays in the body.
  SmallVector<Attribute> keyEquals;
  if (where && !by && !cascade && triggers.empty()) {
    std::function<bool(const Expr &)> outside = [&](const Expr &expr) {
      switch (expr.kind) {
      case Expr::Int:
      case Expr::Float:
      case Expr::Bool:
      case Expr::String:
      case Expr::Const:
        return true;
      case Expr::Name: {
        if (expr.name == entity || ownNames.contains(expr.name))
          return false;
        const Variable *variable = lookup(expr.name);
        return variable && variable->kind == Variable::Value &&
               !variable->isVar;
      }
      case Expr::Field: // (a case of an enum)
        return !lookup(expr.name) && enums.count(expr.name);
      case Expr::Unary:
      case Expr::Binary:
      case Expr::Cast:
        return llvm::all_of(expr.operands, [&](const ExprPtr &operand) {
          return outside(*operand);
        });
      default:
        return false;
      }
    };
    SmallVector<const Expr *> conditions{where.get()};
    for (size_t i = 0; i < conditions.size(); ++i) {
      const Expr &condition = *conditions[i];
      if (condition.kind != Expr::Binary)
        continue;
      if (condition.op == Token::AndAnd) {
        for (const ExprPtr &operand : condition.operands)
          conditions.push_back(operand.get());
        continue;
      }
      if (condition.op != Token::Equal || !keyEquals.empty())
        continue;
      for (int side : {0, 1}) {
        const Expr &field = *condition.operands[side];
        const Expr &value = *condition.operands[1 - side];
        if (field.kind != Expr::Field || !outside(value))
          continue;
        const Binding *bound = nullptr;
        for (const Binding &binding : bindings)
          if (binding.name == field.name)
            bound = &binding;
        if (!bound || bound->via || bound->optional)
          continue;
        Type type = components[bound->component].fieldType(field.field);
        if (!type)
          continue;
        bool text = isa<TextType, StringType>(type);
        FailureOr<mlir::Value> equals = emit(value, type);
        if (failed(equals))
          return failure();
        if (!text && equals->getType() != type)
          return mismatch(value.loc, equals->getType(), type);
        sliceValues.push_back(*equals);
        sliceOrder.push_back(builder.getStringAttr("equals"));
        if (text) {
          // A text key is held by two hashes of it (see `index ... hash`).
          for (bool check : {false, true}) {
            auto node = [&](Expr::Kind kind) {
              auto expr = std::make_unique<Expr>();
              expr->kind = kind;
              expr->loc = value.loc;
              return expr;
            };
            ExprPtr hash = node(Expr::Call);
            hash->name = "hash_of";
            ExprPtr given = node(Expr::Given);
            given->given = *equals;
            hash->operands.push_back(std::move(given));
            if (check) {
              ExprPtr seed = node(Expr::Int);
              seed->intValue = int64_t(0x9e3779b97f4a7c15ull);
              seed->isHex = true;
              hash->operands.push_back(std::move(seed));
            }
            FailureOr<mlir::Value> hashed =
                emit(*hash, builder.getIntegerType(64, /*isSigned=*/false));
            if (failed(hashed))
              return failure();
            sliceValues.push_back(*hashed);
            sliceOrder.push_back(
                builder.getStringAttr(check ? "equals_check" : "equals_hash"));
          }
        }
        keyEquals = {symbol(bound->component),
                     builder.getStringAttr(field.field)};
        break;
      }
    }
  }

  OperationState state(loc(at), QueryOp::getOperationName());
  if (!triggers.empty())
    state.addAttribute(QueryOp::kTriggersAttr, builder.getArrayAttr(triggers));
  if (!with.empty())
    state.addAttribute(QueryOp::kWithAttr, builder.getArrayAttr(with));
  if (!without.empty())
    state.addAttribute(QueryOp::kWithoutAttr, builder.getArrayAttr(without));
  if (!anyGroups.empty())
    state.addAttribute(QueryOp::kAnyAttr, builder.getArrayAttr(anyGroups));
  if (cascade)
    state.addAttribute(QueryOp::kCascadeAttr, cascade);
  if (leavesFirst)
    state.addAttribute(QueryOp::kLeavesFirstAttr, builder.getUnitAttr());
  if (!traversal.empty())
    state.addAttribute(QueryOp::kTraversalAttr,
                       builder.getStringAttr(traversal));
  if (by)
    state.addAttribute(QueryOp::kByAttr, by);
  if (listed)
    state.addAttribute(QueryOp::kListedAttr, listed);
  if (inSet)
    state.addAttribute(QueryOp::kInSetAttr, inSet);
  if (turn)
    state.addAttribute(QueryOp::kTurnAttr, builder.getUnitAttr());
  if (!keyEquals.empty())
    state.addAttribute(QueryOp::kKeyEqualsAttr,
                       builder.getArrayAttr(keyEquals));
  if (!sliceValues.empty()) {
    state.addOperands(sliceValues);
    state.addAttribute(QueryOp::kSliceAttr, builder.getArrayAttr(sliceOrder));
  }
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  for (const Binding &binding : bindings)
    block->addArgument(
        RefType::get(context, symbol(binding.component), binding.mut,
                     binding.via, binding.before, binding.optional,
                     binding.direct, binding.after, binding.hops,
                     binding.path),
        loc(at));
  Operation *query = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  queryScopes = scopes.size();
  ScopeGuard scope(*this);
  for (auto [binding, arg] : llvm::zip(bindings, block->getArguments()))
    bind(binding.name, {Variable::Ref, arg, binding.component, binding.mut});
  for (auto &[name, index] : otherNames)
    bind(name, Variable::ofValue(
                   OtherOp::create(builder, loc(at), EntityType::get(context),
                                   block->getArgument(index))
                       .getResult()));
  inQuery = true;
  llvm::SaveAndRestore floor(breakFloor, breakFlags.size());
  llvm::SaveAndRestore turns(inTurn, turn);
  queryEntity = entity;
  queryNames.clear();
  if (!entity.empty())
    queryNames.insert(entity);
  for (const Binding &binding : bindings)
    if (!binding.via)
      queryNames.insert(binding.name);
  llvm::scope_exit leave([&] {
    inQuery = false;
    queryEntity.clear();
  });
  // The visited entity's id is created where it is used as a value.
  if (!entity.empty())
    bind(entity, Variable::ofEntity(mlir::Value()));
  if (where) {
    FailureOr<mlir::Value> condition = emit(*where, builder.getI1Type());
    if (failed(condition))
      return failure();
    if (!condition->getType().isInteger(1))
      return error(where->loc, "a 'where' condition must be a bool");
    auto branch = scf::IfOp::create(builder, loc(where->loc), *condition);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
  }
  // (The windows: where each starts and how many rows it has; those to
  // change are taken, see BufferClaimOp.)
  for (Window &window : windows) {
    if (turn)
      return error(window.loc, "a window is of a 'for' whose entities run "
                               "at once: taking turns, each reads and "
                               "writes the buffer as it is ('" +
                                   window.symbol + "[start + i].field')");
    FailureOr<mlir::Value> start = emit(*window.start, builder.getI32Type());
    if (failed(start))
      return failure();
    if (!start->getType().isInteger(32))
      return error(window.start->loc, "where a window starts is an i32");
    FailureOr<mlir::Value> count = emit(*window.count, builder.getI32Type());
    if (failed(count))
      return failure();
    if (!count->getType().isInteger(32))
      return error(window.count->loc, "how many rows a window has is an i32");
    if (window.mut)
      BufferClaimOp::create(builder, loc(window.loc),
                            FlatSymbolRefAttr::get(context, window.symbol),
                            *start, *count);
    Variable bound{Variable::Window, *start, window.symbol, window.mut};
    bound.which = *count;
    bind(window.name, bound);
  }
  FlatSymbolRefAttr counter = countsInto;
  countsInto = FlatSymbolRefAttr();
  if (failed(parseBlock()))
    return failure();
  // (One more that the body ran for, where the `for` is asked how many.)
  if (counter)
    AccumulateOp::create(builder, loc(at), counter,
                         builder.getStringAttr("value"),
                         builder.getStringAttr("add"),
                         arith::ConstantIntOp::create(builder, loc(at), 1, 32));
  QueryOp::ensureTerminator(query->getRegion(0), builder, loc(at));
  // (Turn by turn, everything is read as it is then.)
  if (!turn && failed(checkEaches(query)))
    return failure();
  return success();
}

// if cond { } [else { } | else if ...]  /  if let x = C(id).f { } [else { }]
LogicalResult Parser::parseIf() {
  llvm::SMLoc at = token.loc;
  if (consumeKeyword("let"))
    return parseIfLet(at);
  FailureOr<ExprPtr> condition = parseExpr();
  if (failed(condition))
    return failure();
  FailureOr<mlir::Value> value = emit(**condition, builder.getI1Type());
  if (failed(value))
    return failure();
  if (!value->getType().isInteger(1))
    return error(at, "an 'if' condition must be a bool");
  bool hasElse = false;
  auto branch = scf::IfOp::create(builder, loc(at), *value,
                                  /*withElseRegion=*/true);
  SmallVector<VarState> before = captureVars();
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    if (failed(parseBlock()))
      return failure();
  }
  SmallVector<VarState> thenVars = captureVars();
  restoreVars(before);
  if (consumeKeyword("else")) {
    hasElse = true;
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.elseBlock()->getTerminator());
    if (consumeKeyword("if")) {
      ScopeGuard scope(*this);
      if (failed(parseIf()))
        return failure();
    } else if (failed(parseBlock())) {
      return failure();
    }
  }
  if (!hasElse)
    branch.getElseRegion().getBlocks().clear();
  SmallVector<VarState> elseVars = captureVars();
  restoreVars(before);
  mergeBranches(branch, before, thenVars, elseVars);
  return success();
}

// if let x = Component(id).field { found } [else { not found }]
LogicalResult Parser::parseIfLet(llvm::SMLoc at) {
  if (inFunction)
    return error(at, "a fn only computes; 'if let' reads another entity's "
                     "field, which a system does");
  // One or more of `name = Component(entity).field` (another entity's
  // field, which may find nothing) and `name = binding.field` (through an
  // optional binding, which may lead nowhere), with `, let` between them:
  // the first block runs where all are there.
  SmallVector<std::pair<std::string, mlir::Value>> found;
  mlir::Value all;
  do {
    FailureOr<std::string> name = identifier("a name");
    if (failed(name) || failed(expect(Token::Assign, "'='")))
      return failure();
    llvm::SMLoc sourceAt = token.loc;
    FailureOr<std::string> source = identifier("a component or a binding");
    if (failed(source))
      return failure();
    mlir::Value value, there;
    const Variable *variable = lookup(*source);
    auto optional = variable && variable->kind == Variable::Ref &&
                            variable->value
                        ? dyn_cast<RefType>(variable->value.getType())
                        : RefType();
    if (optional && optional.getIsOptional()) {
      if (failed(expect(Token::Dot, "'.' and a field")))
        return failure();
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      Type type = recordOf(*variable).fieldType(*field);
      if (!type)
        return error(fieldAt, "component '" + variable->component +
                                  "' has no field '" + *field + "'");
      there = BoundOp::create(builder, loc(sourceAt), builder.getI1Type(),
                              variable->value)
                  .getResult();
      value = GetOp::create(builder, loc(sourceAt), type, variable->value,
                            builder.getStringAttr(*field))
                  .getResult();
    } else {
      auto record = components.find(*source);
      if (record == components.end())
        return error(sourceAt,
                     "'if let' reads what may not be there: expected "
                     "'Component(entity).field' or a field of an optional "
                     "binding");
      if (failed(expect(Token::LParen, "'('")))
        return failure();
      FailureOr<ExprPtr> id = parseExpr();
      if (failed(id) || failed(expect(Token::RParen, "')'")) ||
          failed(expect(Token::Dot, "'.'")))
        return failure();
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      Type type = record->second.fieldType(*field);
      if (!type)
        return error(fieldAt, "component '" + *source + "' has no field '" +
                                  *field + "'");
      FailureOr<mlir::Value> entity = emit(**id, EntityType::get(context));
      if (failed(entity))
        return failure();
      auto lookupOp =
          LookupOp::create(builder, loc(at), type, builder.getI1Type(),
                           *entity, symbol(*source),
                           builder.getStringAttr(*field));
      value = lookupOp.getValue();
      there = lookupOp.getFound();
    }
    found.push_back({*name, value});
    all = all ? arith::AndIOp::create(builder, loc(at), all, there).getResult()
              : there;
  } while (consumeIf(Token::Comma) && succeeded(expectKeyword("let")));
  auto branch = scf::IfOp::create(builder, loc(at), all,
                                  /*withElseRegion=*/true);
  SmallVector<VarState> before = captureVars();
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    ScopeGuard scope(*this);
    for (auto &[name, value] : found)
      bind(name, Variable::ofValue(value));
    if (failed(parseBlock()))
      return failure();
  }
  SmallVector<VarState> thenVars = captureVars();
  restoreVars(before);
  if (consumeKeyword("else")) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.elseBlock()->getTerminator());
    if (failed(parseBlock()))
      return failure();
  } else {
    branch.getElseRegion().getBlocks().clear();
  }
  SmallVector<VarState> elseVars = captureVars();
  restoreVars(before);
  mergeBranches(branch, before, thenVars, elseVars);
  return success();
}

// `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`,
// `min=`, `max=`
FailureOr<std::pair<Token::Kind, std::optional<StringRef>>>
Parser::parseAssignOp() {
  Token::Kind kind = token.kind;
  if (kind == Token::Assign || computedBy(kind) != Token::Eof) {
    advance();
    return std::make_pair(kind, std::optional<StringRef>());
  }
  if (token.isKeyword("min") || token.isKeyword("max")) {
    StringRef rule = token.isKeyword("min") ? "min" : "max";
    advance();
    if (failed(expect(Token::Assign, "'='")))
      return failure();
    return std::make_pair(Token::Assign, std::optional<StringRef>(rule));
  }
  return error("expected an assignment ('=', '+=', '-=', '*=', '/=', '%=', "
               "'&=', '|=', '^=', '<<=', '>>=', 'min=', 'max='), found '" +
               token.spelling + "'");
}

/// Emit `target op value`: a plain store for `=`, or load, combine, store.
/// A type in the words of a program.
std::string Parser::typeWords(Type type) {
  if (rowOf(type))
    return "a row of a table";
  if (rowsOf(type))
    return "a table";
  if (callableOf(type))
    return "a fn or proc";
  if (auto named = dyn_cast<EnumType>(type))
    return ("a '" + named.getName().getValue() + "'").str();
  if (auto text = dyn_cast<TextType>(type))
    return ("a text[" + Twine(text.getCapacity()) + "]").str();
  if (isa<StringType>(type))
    return "a text";
  if (isa<EntityType>(type))
    return "an entity";
  if (auto index = dyn_cast<IndexOfType>(type))
    return ("a row number of '" + index.getBuffer().getValue() +
            "' (as i32 is a number)")
        .str();
  if (type.isInteger(1))
    return "a bool";
  if (isUnsigned(type))
    return ("a u" + Twine(type.getIntOrFloatBitWidth())).str();
  std::string words;
  llvm::raw_string_ostream(words) << type;
  return "an " + words;
}

/// What is said where a value is not of the type of what it goes into.
LogicalResult Parser::mismatch(llvm::SMLoc at, Type value, Type target) {
  return error(at, "value has a different type than the target: it is " +
                       typeWords(value) + ", and the target holds " +
                       typeWords(target));
}

LogicalResult Parser::emitAssignment(
    llvm::SMLoc at, Token::Kind op, std::optional<StringRef> rule,
    function_ref<Type()> targetType, function_ref<mlir::Value()> load,
    function_ref<LogicalResult(mlir::Value)> store, const Expr &value) {
  Type type = targetType();
  // (A row number is moved on by an i32: `c += 2`.)
  Type valueType = type;
  if (isa<IndexOfType>(type) && rule)
    return error(at, "a row number is assigned and moved on ('+=', '-='); "
                     "it is combined as an i32 ('as i32')");
  if (isa<IndexOfType>(type) && op != Token::Assign) {
    valueType = typeOf(value);
    if (!valueType)
      valueType = builder.getI32Type();
  }
  FailureOr<mlir::Value> rhs = emit(value, valueType);
  if (failed(rhs))
    return failure();
  if (rhs->getType() != valueType)
    return mismatch(value.loc, rhs->getType(), valueType);
  if (isa<StringType>(type)) {
    // What it holds is its own: a copy of the value, and what it held
    // before is given back (after: the value may be a view of that).
    if ((op != Token::Assign && op != Token::PlusAssign) || rule)
      return error(at, "a text can be assigned ('=') or added to ('+=')");
    mlir::Value old = load();
    mlir::Value value = *rhs;
    if (op == Token::PlusAssign)
      value = TextJoinOp::create(builder, loc(at), type, old, value)
                  .getResult();
    if (failed(store(TextOwnOp::create(builder, loc(at), type, value)
                         .getResult())))
      return failure();
    TextDropOp::create(builder, loc(at), old);
    return success();
  }
  if (op == Token::Assign && !rule)
    return store(*rhs);
  if (auto text = dyn_cast<TextType>(type)) {
    // Joined, then cut to what the target holds.
    if (op != Token::PlusAssign || rule)
      return error(at, "a text can be assigned ('=') or added to ('+=')");
    return store(textResize(loc(at), textConcat(at, load(), *rhs), text));
  }
  mlir::Value old = load();
  if (rule)
    return store(combine(loc(at), *rule, old, *rhs));
  FailureOr<mlir::Value> result =
      arithmetic(loc(at), computedBy(op), old, *rhs);
  if (failed(result))
    return failure();
  if (result->getType() != type)
    return mismatch(value.loc, result->getType(), type);
  return store(*result);
}

/// A statement starting with a name: an assignment to a field, a unique or
/// another entity's field, or a method on the visited entity.
LogicalResult Parser::parseNameStatement() {
  llvm::SMLoc at = token.loc;
  std::string name = token.spelling.str();
  advance();

  // A buffer, also one a proc was given: a row more, none, or another
  // value for one.
  if (const Variable *window = lookup(name);
      window && window->kind == Variable::Window)
    return parseWindowStatement(name, *window, at);
  if (bufferNamed(name) && !token.is(Token::LParen) && !token.is(Token::Dot))
    return parseBufferStatement(name, at);
  if (sets.count(name) && !lookup(name))
    return parseSetStatement(name, at);
  if (componentGiven(name) && token.is(Token::LBracket))
    return parseGivenStatement(name, at);

  // Component(id).field op value: combine into another entity's field.
  if (token.is(Token::LParen) && components.count(name)) {
    advance();
    FailureOr<ExprPtr> id = parseExpr();
    if (failed(id) || failed(expect(Token::RParen, "')'")) ||
        failed(expect(Token::Dot, "'.'")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = components[name].fieldType(*field);
    if (!type)
      return error(fieldAt, "component '" + name + "' has no field '" +
                                *field + "'");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseStatementValue();
    if (failed(value))
      return failure();
    // Turn by turn it is written as any field is: read as it is now,
    // and set.
    if (inTurn) {
      if (isa<TextType, StringType>(type))
        return error(fieldAt, "a text of another entity is not set from "
                              "here: that entity sets its own");
      FailureOr<mlir::Value> entity = emit(**id, EntityType::get(context));
      if (failed(entity))
        return failure();
      StringAttr fieldName = builder.getStringAttr(*field);
      return emitAssignment(
          at, op->first, op->second, [&] { return type; },
          [&]() -> mlir::Value {
            return LookupOp::create(builder, loc(at), type,
                                    builder.getI1Type(), *entity,
                                    symbol(name), fieldName)
                .getValue();
          },
          [&](mlir::Value v) -> LogicalResult {
            ApplyOp::create(builder, loc(at), *entity, symbol(name),
                            fieldName, builder.getStringAttr("set"), v);
            return success();
          },
          **value);
    }
    StringRef rule;
    bool negate = false;
    if (op->second)
      rule = *op->second;
    else if (op->first == Token::PlusAssign)
      rule = "add";
    else if (op->first == Token::MinusAssign)
      rule = "add", negate = true;
    else
      return error(at, "another entity's field can only be combined into "
                       "('+=', '-=', 'min=', 'max='): which write lands "
                       "last would depend on the order");
    FailureOr<mlir::Value> entity = emit(**id, EntityType::get(context));
    FailureOr<mlir::Value> rhs = emit(**value, type);
    if (failed(entity) || failed(rhs))
      return failure();
    mlir::Value v = *rhs;
    if (negate) {
      FailureOr<mlir::Value> negated = arithmetic(
          loc(at), Token::Minus,
          noughtOf(loc(at), type),
          v);
      if (failed(negated))
        return failure();
      v = *negated;
    }
    ApplyOp::create(builder, loc(at), *entity, symbol(name),
                    builder.getStringAttr(*field), builder.getStringAttr(rule),
                    v);
    return success();
  }

  const Variable *variable = lookup(name);
  // binding.field op value, for a component of the entity at the other
  // end of an edge a loop visits: combined into, like `C(id).f += v`.
  if (variable && variable->kind == Variable::Other) {
    if (failed(expect(Token::Dot, "'.' and a field")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    Type type = recordOf(*variable).fieldType(*field);
    if (!type)
      return error(fieldAt, "component '" + variable->component +
                                "' has no field '" + *field + "'");
    if (!variable->mut)
      return error(at, "'" + name + "' is not 'mut': bind it as '(" + name +
                           ": mut " + variable->component +
                           ")' to send a value to it");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseStatementValue();
    if (failed(value))
      return failure();
    // (Turn by turn, as any field: read as it is now, and set.)
    if (inTurn) {
      if (isa<TextType, StringType>(type))
        return error(fieldAt, "a text of another entity is not set from "
                              "here: that entity sets its own");
      StringAttr fieldName = builder.getStringAttr(*field);
      return emitAssignment(
          at, op->first, op->second, [&] { return type; },
          [&]() -> mlir::Value {
            return LookupOp::create(builder, loc(at), type, builder.getI1Type(),
                                    variable->value,
                                    symbol(variable->component), fieldName)
                .getValue();
          },
          [&](mlir::Value v) -> LogicalResult {
            ApplyOp::create(builder, loc(at), variable->value,
                            symbol(variable->component), fieldName,
                            builder.getStringAttr("set"), v);
            return success();
          },
          **value);
    }
    StringRef rule;
    bool negate = false;
    if (op->second)
      rule = *op->second;
    else if (op->first == Token::PlusAssign)
      rule = "add";
    else if (op->first == Token::MinusAssign)
      rule = "add", negate = true;
    else
      return error(at, "another entity's field can only be combined into "
                       "('+=', '-=', 'min=', 'max='): which write lands "
                       "last would depend on the order");
    FailureOr<mlir::Value> rhs = emit(**value, type);
    if (failed(rhs))
      return failure();
    if (rhs->getType() != type)
      return mismatch((*value)->loc, rhs->getType(), type);
    mlir::Value v = *rhs;
    if (negate) {
      FailureOr<mlir::Value> negated = arithmetic(
          loc(at), Token::Minus,
          noughtOf(loc(at), type),
          v);
      if (failed(negated))
        return failure();
      v = *negated;
    }
    ApplyOp::create(builder, loc(at), variable->value,
                    symbol(variable->component),
                    builder.getStringAttr(*field), builder.getStringAttr(rule),
                    v);
    return success();
  }
  // e.destroy(), e.add(C { .. }), e.remove(C)
  if (variable && variable->kind == Variable::Entity) {
    if (failed(expect(Token::Dot, "'.' and a method")))
      return failure();
    return parseMethod(name, at);
  }

  // A unique: Unique.field op value, or Unique op value (shorthand).
  auto unique = uniques.find(name);
  if (!variable && unique != uniques.end() && inFunction)
    return error(at, "a fn only computes; it cannot write unique '" + name +
                         "'. Give the value back and let a system write it");
  if (bufferNamed(name))
    return parseBufferStatement(name, at);
  if (!variable && unique != uniques.end()) {
    std::string field = "value";
    if (consumeIf(Token::Dot)) {
      llvm::SMLoc fieldAt = token.loc;
      FailureOr<std::string> named = identifier("a field");
      if (failed(named))
        return failure();
      field = *named;
      if (!unique->second.fieldType(field))
        return error(fieldAt,
                     "unique '" + name + "' has no field '" + field + "'");
    } else if (!unique->second.shorthand) {
      return error(at, "unique '" + name + "' has fields; assign one with '" +
                           name + ".field'");
    }
    Type type = unique->second.fieldType(field);
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseStatementValue();
    if (failed(value))
      return failure();
    if (inQuery && !inTurn) {
      // Inside a query a unique is accumulated into, or set: to what
      // the last entity that sets it gives.
      StringRef rule;
      bool negate = false;
      if (op->second)
        rule = *op->second;
      else if (op->first == Token::PlusAssign)
        rule = "add";
      else if (op->first == Token::MinusAssign)
        rule = "add", negate = true;
      else if (op->first == Token::Assign)
        rule = "set";
      else
        return error(at, "inside a 'for', a unique can only be set or "
                         "accumulated into ('=', '+=', '-=', 'min=', "
                         "'max='): every entity writes the same field");
      FailureOr<mlir::Value> rhs = emit(**value, type);
      if (failed(rhs))
        return failure();
      if (rhs->getType() != type)
        return mismatch((*value)->loc, rhs->getType(), type);
      mlir::Value v = *rhs;
      if (negate) {
        FailureOr<mlir::Value> negated = arithmetic(
            loc(at), Token::Minus,
            noughtOf(loc(at), type),
            v);
        if (failed(negated))
          return failure();
        v = *negated;
      }
      AccumulateOp::create(builder, loc(at), symbol(name),
                           builder.getStringAttr(field),
                           builder.getStringAttr(rule), v);
      return success();
    }
    return emitAssignment(
        at, op->first, op->second, [&] { return type; },
        [&]() -> mlir::Value {
          return ReadOp::create(builder, loc(at), type, symbol(name),
                                builder.getStringAttr(field));
        },
        [&](mlir::Value v) -> LogicalResult {
          WriteOp::create(builder, loc(at), symbol(name),
                          builder.getStringAttr(field), v);
          return success();
        },
        **value);
  }

  // binding.field op value
  if (variable && variable->kind == Variable::Ref) {
    if (failed(expect(Token::Dot, "'.' and a field")))
      return failure();
    llvm::SMLoc fieldAt = token.loc;
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    bool edge = relations.count(variable->component);
    // s.disconnect(): remove the edge the loop visits.
    if (edge && *field == "disconnect" && consumeIf(Token::LParen)) {
      if (failed(expect(Token::RParen, "')'")))
        return failure();
      DisconnectOp::create(builder, loc(at));
      return success();
    }
    Type type = recordOf(*variable).fieldType(*field);
    if (!type)
      return error(fieldAt, std::string(edge ? "relation '" : "component '") +
                                variable->component + "' has no field '" +
                                *field + "'");
    bool ancestor = false;
    if (variable->value)
      if (auto ref = dyn_cast<RefType>(variable->value.getType()))
        ancestor = ref.isUp();
    if (ancestor && !variable->mut)
      return error(at, "'" + name + "' is an ancestor's '" +
                           variable->component + "' and not 'mut': bind it "
                           "as '(" + name + ": mut " + variable->component +
                           ")' to combine into it");
    if (ancestor) {
      // Another entity's field: combined into, like `C(id).f += v`.
      auto op = parseAssignOp();
      if (failed(op))
        return failure();
      FailureOr<ExprPtr> value = parseStatementValue();
      if (failed(value))
        return failure();
      StringRef rule;
      bool negate = false;
      if (op->second)
        rule = *op->second;
      else if (op->first == Token::PlusAssign)
        rule = "add";
      else if (op->first == Token::MinusAssign)
        rule = "add", negate = true;
      else
        return error(at, "an ancestor's field can only be combined into "
                         "('+=', '-=', 'min=', 'max='): which write lands "
                         "last would depend on the order");
      FailureOr<mlir::Value> rhs = emit(**value, type);
      if (failed(rhs))
        return failure();
      if (rhs->getType() != type)
        return mismatch((*value)->loc, rhs->getType(), type);
      mlir::Value v = *rhs;
      if (negate) {
        FailureOr<mlir::Value> negated = arithmetic(
            loc(at), Token::Minus,
            noughtOf(loc(at), type),
            v);
        if (failed(negated))
          return failure();
        v = *negated;
      }
      if (inEach)
        return error(at, "adding into an ancestor is done after the 'for' that goes through "
                         "other entities, not in it: that one only reads "
                         "them");
      CombineOp::create(builder, loc(at), variable->value,
                        builder.getStringAttr(*field),
                        builder.getStringAttr(rule), v);
      return success();
    }
    if (!variable->mut)
      return error(at, edge ? "'" + name + "' is not 'mut': bind it as '[mut " +
                                  name + ": " + variable->component +
                                  "]' to write it"
                            : "'" + name + "' is not 'mut': bind it as '" +
                                  name + ": mut " + variable->component +
                                  "' to write it");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseStatementValue();
    if (failed(value))
      return failure();
    mlir::Value ref = variable->value;
    return emitAssignment(
        at, op->first, op->second, [&] { return type; },
        [&]() -> mlir::Value {
          return GetOp::create(builder, loc(at), type, ref,
                               builder.getStringAttr(*field));
        },
        [&](mlir::Value v) -> LogicalResult {
          SetOp::create(builder, loc(at), ref, builder.getStringAttr(*field),
                        v);
          return success();
        },
        **value);
  }

  // A var: name op value.
  if (variable && variable->isVar) {
    unsigned scope = scopes.size();
    while (!scopes[--scope].count(name))
      ;
    if (inQuery && scope < queryScopes)
      return error(at, "'" + name + "' is a var from outside the 'for': "
                       "every entity would assign it. Accumulate into a "
                       "unique ('+=', '-=', 'min=', 'max=') instead");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseStatementValue();
    if (failed(value))
      return failure();
    Variable &target = scopes[scope][name];
    return emitAssignment(
        at, op->first, op->second, [&] { return target.value.getType(); },
        [&]() -> mlir::Value { return target.value; },
        [&](mlir::Value v) -> LogicalResult {
          target.value = v;
          return success();
        },
        **value);
  }

  if (variable && variable->kind == Variable::Row)
    return error(at, "'" + name + "' is a row of a table, and a table "
                                  "never changes");
  if (variable)
    return error(at, "'" + name + "' is a value and cannot be assigned: "
                                  "declare it with 'var' instead of 'let'");
  if (constants.count(name))
    return error(at, "'" + name + "' is a const and cannot be assigned");
  return error(at, "unknown name '" + name + "'");
}

// e.destroy() / e.add(C { .. }) / e.remove(C)
LogicalResult Parser::parseMethod(const std::string &entity, llvm::SMLoc at) {
  if (inEach)
    return error(at, "an entity is destroyed, and given or rid of a "
                     "component, after the 'for' that goes through other "
                     "entities, not in it: that one only reads them");
  llvm::SMLoc methodAt = token.loc;
  FailureOr<std::string> method = identifier("a method");
  if (failed(method) || failed(expect(Token::LParen, "'('")))
    return failure();
  if (entity != queryEntity)
    return error(at, "only the entity a 'for' visits can be changed: '" +
                         entity + "' is not it");
  if (*method == "destroy") {
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    DespawnOp::create(builder, loc(at));
    return success();
  }
  if (*method == "add" && token.is(Token::Identifier) &&
      peek().is(Token::LParen) && prefabs.count(token.spelling)) {
    // A prefab with what it is given: every component it comes to.
    std::vector<SpawnEntry> one(1);
    one[0].loc = token.loc;
    if (failed(parsePrefabUse(one[0])) ||
        failed(expect(Token::RParen, "')'")))
      return failure();
    return emitAddEntries(one, at);
  }
  if (*method == "add") {
    FailureOr<ComponentInit> init = parseComponentInit();
    if (failed(init) || failed(expect(Token::RParen, "')'")))
      return failure();
    if (failed(keepsNoText(at, init->component)))
      return failure();
    FailureOr<SmallVector<mlir::Value>> values = emitInitValues(*init);
    if (failed(values))
      return failure();
    AddOp::create(builder, loc(at), symbol(init->component), *values);
    return success();
  }
  if (*method == "remove") {
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component) || failed(expect(Token::RParen, "')'")))
      return failure();
    if (!components.count(*component))
      return error(componentAt, "unknown component '" + *component + "'");
    if (failed(keepsNoText(at, *component)))
      return failure();
    RemoveOp::create(builder, loc(at), symbol(*component));
    return success();
  }
  return error(methodAt, "unknown method '" + *method +
                             "'; expected destroy, add or remove");
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

FailureOr<ExprPtr> Parser::parseExpr() { return parseBinary(0); }

static int precedenceOf(Token::Kind kind) {
  switch (kind) {
  case Token::OrOr:
    return 1;
  case Token::AndAnd:
    return 2;
  case Token::Equal:
  case Token::NotEqual:
    return 3;
  case Token::Less:
  case Token::LessEqual:
  case Token::Greater:
  case Token::GreaterEqual:
    return 4;
  case Token::Pipe:
    return 5;
  case Token::Caret:
    return 6;
  case Token::Amp:
    return 7;
  case Token::ShiftLeft:
  case Token::ShiftRight:
    return 8;
  case Token::Plus:
  case Token::Minus:
    return 9;
  case Token::Star:
  case Token::Slash:
  case Token::Percent:
    return 10;
  default:
    return -1;
  }
}

FailureOr<ExprPtr> Parser::parseBinary(int minPrecedence) {
  FailureOr<ExprPtr> lhs = parseUnary();
  if (failed(lhs))
    return failure();
  while (true) {
    // e in Name: whether a set holds the entity (as a comparison).
    if (token.isKeyword("in") && peek().is(Token::Identifier) &&
        sets.count(peek().spelling) && minPrecedence <= 4) {
      llvm::SMLoc inAt = token.loc;
      advance();
      auto node = std::make_unique<Expr>();
      node->kind = Expr::InSet;
      node->loc = inAt;
      node->name = token.spelling.str();
      advance();
      node->operands.push_back(std::move(*lhs));
      lhs = std::move(node);
      continue;
    }
    int precedence = precedenceOf(token.kind);
    if (precedence < 0 || precedence < minPrecedence)
      return lhs;
    Token op = token;
    advance();
    FailureOr<ExprPtr> rhs = parseBinary(precedence + 1);
    if (failed(rhs))
      return failure();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Binary;
    node->loc = op.loc;
    node->op = op.kind;
    node->operands.push_back(std::move(*lhs));
    node->operands.push_back(std::move(*rhs));
    lhs = std::move(node);
  }
}

FailureOr<ExprPtr> Parser::parseUnary() {
  if (token.is(Token::Minus) || token.is(Token::Not) ||
      token.is(Token::Tilde)) {
    Token op = token;
    advance();
    FailureOr<ExprPtr> operand = parseUnary();
    if (failed(operand))
      return failure();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Unary;
    node->loc = op.loc;
    node->op = op.kind;
    node->operands.push_back(std::move(*operand));
    return node;
  }
  FailureOr<ExprPtr> primary = parsePrimary();
  if (failed(primary))
    return failure();
  // text[i]: one byte of a text. table[i], table[i].field: of a row.
  // value.field: of the row a value is (what a fn gives, a field holds).
  while (token.is(Token::LBracket) || token.is(Token::Dot)) {
    llvm::SMLoc at = token.loc;
    if (consumeIf(Token::Dot)) {
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      auto node = std::make_unique<Expr>();
      node->kind = Expr::Of;
      node->loc = at;
      node->field = *field;
      node->operands.push_back(std::move(*primary));
      primary = std::move(node);
      continue;
    }
    advance();
    FailureOr<ExprPtr> index = parseExpr();
    if (failed(index) || failed(expect(Token::RBracket, "']'")))
      return failure();
    const Variable *named = (*primary)->kind == Expr::Name
                                ? lookup((*primary)->name)
                                : nullptr;
    if ((*primary)->kind == Expr::Name &&
        ((!named && tables.count((*primary)->name)) ||
         bufferNamed((*primary)->name) ||
         componentGiven((*primary)->name) ||
         (named && named->kind == Variable::Window))) {
      auto node = std::make_unique<Expr>();
      node->kind = Expr::Row;
      node->loc = at;
      node->name = (*primary)->name;
      node->operands.push_back(std::move(*index));
      if (consumeIf(Token::Dot)) {
        FailureOr<std::string> field = identifier("a field");
        if (failed(field))
          return failure();
        node->field = *field;
      }
      primary = std::move(node);
      continue;
    }
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Index;
    node->loc = at;
    node->operands.push_back(std::move(*primary));
    node->operands.push_back(std::move(*index));
    // (value[i].field: a row of the table a value holds; the row's
    // number first, as for a table by its name.)
    if (token.is(Token::Dot)) {
      advance();
      FailureOr<std::string> field = identifier("a field");
      if (failed(field))
        return failure();
      node->kind = Expr::Row;
      node->field = *field;
      std::swap(node->operands[0], node->operands[1]);
    }
    primary = std::move(node);
  }
  while (token.isKeyword("as")) {
    llvm::SMLoc at = token.loc;
    advance();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Cast;
    node->loc = at;
    node->castType = *type;
    node->operands.push_back(std::move(*primary));
    primary = std::move(node);
  }
  return primary;
}

// After `let` (or `var`): name = value, or (a, b) = values.
// What a statement gives a name: a value, or `for ... { }`, a `for` over
// entities, which gives how many of them its body ran for.
FailureOr<ExprPtr> Parser::parseStatementValue() {
  llvm::SaveAndRestore allowed(forValueAllowed, true);
  return parseExpr();
}

// for ... { }, where a value stands: the `for` runs here, before the rest
// of the value is worked out, and gives its number.
FailureOr<ExprPtr> Parser::parseCountedQuery() {
  llvm::SMLoc at = token.loc;
  advance();
  if (!forValueAllowed)
    return error(at, "a 'for' over entities gives its number to a "
                     "statement ('let n = for ...', 'n += for ...'), not "
                     "where a value may not be asked for at all (in an "
                     "'if' that gives a value)");
  if (inQuery || inFunction || atCountedFor())
    return error(at, "only a 'for' over entities gives a number, how many "
                     "its body ran for: at the top level of a system, or in "
                     "a loop there");
  if (!systemOp)
    return error(at, "only a 'for' over entities gives a number, how many "
                     "its body ran for: at the top level of a system, or in "
                     "a loop there");
  // The count is a unique of its own, which the body adds one to.
  Type i32 = builder.getI32Type();
  StringAttr name;
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(systemOp);
    name = declareSymbol(at, ("for_count_" + Twine(countedFors++)).str());
    ResourceOp::create(builder, loc(at), name,
                       builder.getArrayAttr({builder.getStringAttr("value")}),
                       builder.getArrayAttr({TypeAttr::get(i32)}));
  }
  FlatSymbolRefAttr counter = FlatSymbolRefAttr::get(name);
  StringAttr value = builder.getStringAttr("value");
  // (A system that says what it writes writes this too.)
  auto system = cast<SystemOp>(systemOp);
  if (ArrayAttr writes = system.getWritesAttr()) {
    SmallVector<Attribute> all(writes.begin(), writes.end());
    all.push_back(counter);
    system.setWritesAttr(builder.getArrayAttr(all));
  }
  WriteOp::create(builder, loc(at), counter, value,
                  arith::ConstantIntOp::create(builder, loc(at), 0, 32));
  llvm::SaveAndRestore counting(countsInto, counter);
  if (failed(parseFor()))
    return failure();
  auto expr = std::make_unique<Expr>();
  expr->kind = Expr::Given;
  expr->loc = at;
  expr->given =
      ReadOp::create(builder, loc(at), i32, counter, value).getResult();
  return expr;
}

FailureOr<Let> Parser::parseLet(bool statement) {
  Let let;
  if (consumeIf(Token::LParen)) {
    let.several = true;
    do {
      FailureOr<std::string> name = identifier("a name");
      if (failed(name))
        return failure();
      let.names.push_back(*name);
    } while (consumeIf(Token::Comma));
    if (failed(expect(Token::RParen, "')'")))
      return failure();
  } else {
    FailureOr<std::string> name = identifier("a name");
    if (failed(name))
      return failure();
    let.names.push_back(*name);
  }
  if (failed(expect(Token::Assign, "'='")))
    return failure();
  FailureOr<ExprPtr> value = statement ? parseStatementValue() : parseExpr();
  if (failed(value))
    return failure();
  let.value = std::move(*value);
  return let;
}

LogicalResult Parser::emitLet(const Let &let, bool isVar) {
  SmallVector<mlir::Value> values;
  // let point = road[i]: a row of a table, read by its fields.
  if (!let.several && isWholeRow(*let.value)) {
    if (isVar)
      return error(let.value->loc, "a var does not hold a row of a table; "
                                   "a 'let' stands for one");
    FailureOr<Variable> row = emitRow(*let.value);
    if (failed(row))
      return failure();
    bind(let.names[0], *row);
    return success();
  }
  if (let.several) {
    SmallVector<Type> any(let.names.size());
    FailureOr<SmallVector<mlir::Value>> several = emitSeveral(*let.value, any);
    if (failed(several))
      return failure();
    values = std::move(*several);
  } else {
    FailureOr<mlir::Value> value = emit(*let.value, Type());
    if (failed(value))
      return failure();
    values.push_back(*value);
  }
  for (mlir::Value value : values) {
    if (!isa<StringType>(value.getType()))
      continue;
    if (isVar)
      return error(let.value->loc, "a var does not hold a text of any "
                                   "length; one of a capacity it does ('as "
                                   "text[N]')");
    if (mayBeAssigned(*let.value))
      return error(let.value->loc,
                   "a 'let' does not keep the text a unique or a 'mut' "
                   "binding holds: that may be given another meanwhile. "
                   "Keep a copy of a capacity ('as text[N]')");
  }
  for (auto [name, value] : llvm::zip(let.names, values))
    bind(name, isVar ? Variable::ofVar(value) : Variable::ofValue(value));
  return success();
}

// { [let x = e;]* value }
FailureOr<std::unique_ptr<Branch>> Parser::parseBranch() {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  // (Only one of the branches is worked out: no place for a `for`.)
  llvm::SaveAndRestore lazy(forValueAllowed, false);
  auto branch = std::make_unique<Branch>();
  while (consumeKeyword("let")) {
    FailureOr<Let> let = parseLet();
    if (failed(let))
      return failure();
    consumeIf(Token::Semicolon);
    branch->lets.push_back(std::move(*let));
  }
  if (token.isKeyword("var"))
    return error("'var' is not supported here yet: a branch of an 'if' "
                 "that gives a value is made of 'let's");
  FailureOr<ExprPtr> value = parseExpr();
  if (failed(value) || failed(expect(Token::RBrace, "'}'")))
    return failure();
  branch->value = std::move(*value);
  return branch;
}

// Component { field: value, ... } or a tag: Component [{}]
// if condition { entries } [else if ... | else { entries }], in a list of
// what is spawned (`if` is read).
FailureOr<SpawnEntry> Parser::parseSpawnIf() {
  SpawnEntry entry;
  entry.kind = SpawnEntry::If;
  entry.loc = token.loc;
  FailureOr<ExprPtr> condition = parseExpr();
  if (failed(condition))
    return failure();
  entry.condition = std::move(*condition);
  FailureOr<std::vector<SpawnEntry>> then = parseSpawnEntries();
  if (failed(then))
    return failure();
  entry.then = std::move(*then);
  if (consumeKeyword("else")) {
    if (consumeKeyword("if")) {
      FailureOr<SpawnEntry> nested = parseSpawnIf();
      if (failed(nested))
        return failure();
      entry.otherwise.push_back(std::move(*nested));
    } else {
      FailureOr<std::vector<SpawnEntry>> otherwise = parseSpawnEntries();
      if (failed(otherwise))
        return failure();
      entry.otherwise = std::move(*otherwise);
    }
  }
  return entry;
}

// name(value, ...): a prefab with what it is given, into `entry` (the
// name is the token).
LogicalResult Parser::parsePrefabUse(SpawnEntry &entry) {
  entry.kind = SpawnEntry::Prefab;
  entry.prefab = token.spelling.str();
  advance();
  advance();
  while (!token.is(Token::RParen)) {
    FailureOr<ExprPtr> arg = parseExpr();
    if (failed(arg))
      return failure();
    entry.args.push_back(std::move(*arg));
    if (!consumeIf(Token::Comma))
      break;
  }
  return expect(Token::RParen, "')'");
}

/// Add to the visited entity what the entries list (of a prefab that
/// `e.add` names): a component with its values as `e.add` of it, a
/// prefab's entries where its parameters are what it was given, the
/// entries of an `if` where it holds.
LogicalResult Parser::emitAddEntries(const std::vector<SpawnEntry> &entries,
                                     llvm::SMLoc at) {
  for (const SpawnEntry &entry : entries) {
    switch (entry.kind) {
    case SpawnEntry::Init: {
      if (failed(keepsNoText(at, entry.init.component)))
        return failure();
      FailureOr<SmallVector<mlir::Value>> values = emitInitValues(entry.init);
      if (failed(values))
        return failure();
      AddOp::create(builder, loc(at), symbol(entry.init.component), *values);
      break;
    }
    case SpawnEntry::Prefab: {
      Prefab &prefab = prefabs[entry.prefab];
      if (entry.args.size() != prefab.params.size())
        return error(entry.loc, "'" + entry.prefab + "' takes " +
                                    Twine(prefab.params.size()) +
                                    " argument(s), not " +
                                    Twine(entry.args.size()));
      llvm::StringMap<Variable> given;
      for (auto [arg, param] : llvm::zip(entry.args, prefab.params)) {
        FailureOr<mlir::Value> value = emit(*arg, param.second);
        if (failed(value))
          return failure();
        if (value->getType() != param.second)
          return error(arg->loc, "argument has a different type than the "
                                 "parameter");
        given[param.first] = Variable::ofValue(*value);
      }
      auto here = std::move(scopes);
      SourceModule *module = current;
      scopes.clear();
      scopes.push_back(std::move(given));
      current = prefab.home;
      LogicalResult added = emitAddEntries(prefab.entries, at);
      scopes = std::move(here);
      current = module;
      if (failed(added))
        return failure();
      break;
    }
    case SpawnEntry::If: {
      FailureOr<mlir::Value> condition =
          emit(*entry.condition, builder.getI1Type());
      if (failed(condition))
        return failure();
      if (!condition->getType().isInteger(1))
        return error(entry.condition->loc,
                     "an 'if' condition must be a bool");
      auto branch = scf::IfOp::create(builder, loc(entry.loc), *condition,
                                      /*withElseRegion=*/true);
      for (bool taken : {true, false}) {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPoint(
            (taken ? branch.thenBlock() : branch.elseBlock())
                ->getTerminator());
        if (failed(emitAddEntries(taken ? entry.then : entry.otherwise, at)))
          return failure();
      }
      break;
    }
    }
  }
  return success();
}

// { entry, ... }: a component with its values, a prefab with what it is
// given, or an `if` that picks (which needs no comma after it).
FailureOr<std::vector<SpawnEntry>> Parser::parseSpawnEntries() {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  std::vector<SpawnEntry> entries;
  while (!token.is(Token::RBrace)) {
    if (consumeKeyword("if")) {
      FailureOr<SpawnEntry> entry = parseSpawnIf();
      if (failed(entry))
        return failure();
      entries.push_back(std::move(*entry));
      consumeIf(Token::Comma);
      continue;
    }
    SpawnEntry entry;
    entry.loc = token.loc;
    if (token.is(Token::Identifier) && peek().is(Token::LParen) &&
        prefabs.count(token.spelling)) {
      if (failed(parsePrefabUse(entry)))
        return failure();
    } else {
      FailureOr<ComponentInit> init = parseComponentInit();
      if (failed(init))
        return failure();
      entry.init = std::move(*init);
    }
    entries.push_back(std::move(entry));
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RBrace, "'}'")))
    return failure();
  return entries;
}

// prefab name(parameter: type, ...) { entries }
LogicalResult Parser::parsePrefab() {
  advance();
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a prefab's name");
  if (failed(name) || failed(expect(Token::LParen, "'('")))
    return failure();
  Prefab prefab;
  prefab.home = current;
  while (!token.is(Token::RParen)) {
    FailureOr<std::string> param = identifier("a parameter");
    if (failed(param) || failed(expect(Token::Colon, "':'")))
      return failure();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
    prefab.params.push_back({*param, *type});
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RParen, "')'")))
    return failure();
  if (prefabs.count(*name) || functions.count(*name))
    return error(at, "'" + *name + "' is declared already");
  FailureOr<std::vector<SpawnEntry>> entries = parseSpawnEntries();
  if (failed(entries))
    return failure();
  prefab.entries = std::move(*entries);
  (void)declareSymbol(at, *name);
  prefabs[*name] = std::move(prefab);
  return success();
}

FailureOr<ComponentInit> Parser::parseComponentInit() {
  ComponentInit init;
  init.loc = token.loc;
  FailureOr<std::string> component = identifier("a component");
  if (failed(component))
    return failure();
  if (!components.count(*component))
    return error(init.loc, "unknown component '" + *component + "'");
  init.component = *component;
  if (!consumeIf(Token::LBrace))
    return init;
  while (!token.is(Token::RBrace)) {
    FailureOr<std::string> field = identifier("a field");
    if (failed(field) || failed(expect(Token::Colon, "':'")))
      return failure();
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    init.fields.push_back({*field, std::move(*value)});
    if (!consumeIf(Token::Comma))
      break;
  }
  if (failed(expect(Token::RBrace, "'}'")))
    return failure();
  return init;
}

FailureOr<ExprPtr> Parser::parsePrimary() {
  if (token.isKeyword("for"))
    return parseCountedQuery();
  auto node = std::make_unique<Expr>();
  node->loc = token.loc;
  switch (token.kind) {
  case Token::Integer: {
    node->kind = Expr::Int;
    if (token.spelling.starts_with_insensitive("0x")) {
      uint64_t bits;
      if (token.spelling.drop_front(2).getAsInteger(16, bits))
        return error("a hex literal has at most 16 digits (64 bits)");
      node->intValue = int64_t(bits);
      node->isHex = true;
    } else if (token.spelling.getAsInteger(10, node->intValue))
      return error("integer literal out of range");
    advance();
    return node;
  }
  case Token::Float: {
    node->kind = Expr::Float;
    if (token.spelling.getAsDouble(node->floatValue))
      return error("malformed float literal");
    advance();
    return node;
  }
  case Token::LParen: {
    advance();
    FailureOr<ExprPtr> inner = parseExpr();
    if (failed(inner))
      return failure();
    // (a, b): several values.
    if (token.is(Token::Comma)) {
      node->kind = Expr::Tuple;
      node->operands.push_back(std::move(*inner));
      while (consumeIf(Token::Comma)) {
        FailureOr<ExprPtr> next = parseExpr();
        if (failed(next))
          return failure();
        node->operands.push_back(std::move(*next));
      }
      if (failed(expect(Token::RParen, "')'")))
        return failure();
      return node;
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    return inner;
  }
  case Token::String:
    return parseString();
  case Token::Char: {
    // 'a' or an escape: one byte.
    StringRef rest = token.spelling.drop_front().drop_back();
    char byte = 0;
    if (rest.consume_front("\\")) {
      FailureOr<char> escaped = parseEscape(rest, token.loc);
      if (failed(escaped))
        return failure();
      byte = *escaped;
    } else if (!rest.empty()) {
      byte = rest.front();
      rest = rest.drop_front();
    } else {
      return error("a character literal holds one byte");
    }
    if (!rest.empty())
      return error("a character literal holds one byte; text is written "
                   "in double quotes");
    node->kind = Expr::Int;
    node->isByte = true;
    node->intValue = static_cast<unsigned char>(byte);
    advance();
    return node;
  }
  case Token::Error:
    if (token.spelling.starts_with("\"") || token.spelling.starts_with("'"))
      return error("this literal is not closed on its line");
    [[fallthrough]];
  case Token::Identifier:
    if (token.is(Token::Identifier))
      break;
    [[fallthrough]];
  default:
    return error("expected an expression, found '" + token.spelling + "'");
  }

  if (consumeKeyword("true") || token.isKeyword("false")) {
    node->kind = Expr::Bool;
    node->boolValue = !consumeKeyword("false");
    return node;
  }
  if (consumeKeyword("if")) {
    node->kind = Expr::If;
    FailureOr<ExprPtr> condition = parseExpr();
    if (failed(condition))
      return failure();
    node->operands.push_back(std::move(*condition));
    auto thenBranch = parseBranch();
    if (failed(thenBranch) || failed(expectKeyword("else")))
      return failure();
    node->thenBranch = std::move(*thenBranch);
    // `else if ...`: the branch is that `if`, as if written in braces.
    if (token.isKeyword("if")) {
      FailureOr<ExprPtr> rest = parsePrimary();
      if (failed(rest))
        return failure();
      node->elseBranch = std::make_unique<Branch>();
      node->elseBranch->value = std::move(*rest);
      return node;
    }
    auto elseBranch = parseBranch();
    if (failed(elseBranch))
      return failure();
    node->elseBranch = std::move(*elseBranch);
    return node;
  }
  if (consumeKeyword("spawn")) {
    node->kind = Expr::Spawn;
    FailureOr<std::vector<SpawnEntry>> entries = parseSpawnEntries();
    if (failed(entries))
      return failure();
    node->entries = std::move(*entries);
    return node;
  }

  std::string name = token.spelling.str();
  advance();
  // (`(` on the next line is not a call: `let s = a + b` and then a line
  // `(s, r)` is a sum and then a pair.)
  if (token.is(Token::LParen) && !onNewLine()) {
    if (components.count(name))
      return error(node->loc,
                   "reading another entity's field may find nothing: use "
                   "'if let value = " +
                       name + "(entity).field { ... }'");
    advance();
    node->kind = Expr::Call;
    node->name = name;
    while (!token.is(Token::RParen)) {
      FailureOr<ExprPtr> arg = parseExpr();
      if (failed(arg))
        return failure();
      node->operands.push_back(std::move(*arg));
      if (!consumeIf(Token::Comma))
        break;
    }
    if (failed(expect(Token::RParen, "')'")))
      return failure();
    return node;
  }
  if (consumeIf(Token::Dot)) {
    FailureOr<std::string> field = identifier("a field");
    if (failed(field))
      return failure();
    node->kind = Expr::Field;
    node->name = name;
    node->field = *field;
    if (*field == "has" && consumeIf(Token::LParen)) {
      node->kind = Expr::Has;
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component))
        return failure();
      if (!components.count(*component))
        return error(componentAt, "unknown component '" + *component + "'");
      node->field = *component;
      if (failed(expect(Token::RParen, "')'")))
        return failure();
    } else if (token.is(Token::LParen) && !onNewLine() &&
               consumeIf(Token::LParen)) {
      // binding.field(args): a call of the fn or proc the field holds.
      node->kind = Expr::Call;
      while (!token.is(Token::RParen)) {
        FailureOr<ExprPtr> arg = parseExpr();
        if (failed(arg))
          return failure();
        node->operands.push_back(std::move(*arg));
        if (!consumeIf(Token::Comma))
          break;
      }
      if (failed(expect(Token::RParen, "')'")))
        return failure();
    }
    return node;
  }
  // (A const by its name: its value, written here again.)
  if (!lookup(name) && constants.count(name)) {
    node->kind = Expr::Const;
    node->name = resolve(name);
    return node;
  }
  // (A file by its name: the text that names it.)
  if (!lookup(name) && assetFiles.count(name)) {
    node->kind = Expr::String;
    node->name = assetFiles[name];
    return node;
  }
  node->kind = Expr::Name;
  node->name = name;
  return node;
}

/// The type of `expr` where it does not depend on context; null for
/// literals (and expressions of literals only).
Type Parser::typeOf(const Expr &expr) {
  switch (expr.kind) {
  case Expr::Int:
    return expr.isByte ? Type(builder.getIntegerType(8)) : Type();
  case Expr::Float:
  case Expr::String:
    return {};
  case Expr::Index:
    // (Of the table a value holds, where that is a plain list.)
    if (RowsShape *shape = rowsOf(typeOf(*expr.operands[0])))
      return shape->fields.size() == 1 && shape->fields[0].first.empty()
                 ? shape->fields[0].second
                 : Type();
    return builder.getIntegerType(8);
  case Expr::Row: {
    if (expr.name.empty()) {
      if (RowsShape *shape = rowsOf(typeOf(*expr.operands[1])))
        for (auto &[field, type] : shape->fields)
          if (field == expr.field)
            return type;
      return {};
    }
    if (const Variable *window = lookup(expr.name);
        window && window->kind == Variable::Window)
      return buffers.ofSymbol(window->component).fieldType(expr.field);
    if (std::optional<BufferUse> held = bufferNamed(expr.name))
      return buffers.ofSymbol(held->symbol).fieldType(expr.field);
    if (std::optional<ComponentUse> held = componentGiven(expr.name))
      return components[held->component].fieldType(expr.field);
    auto table = tables.find(expr.name);
    return table == tables.end() ? Type()
                                 : table->second.fieldType(expr.field);
  }
  case Expr::Format: {
    Type type = typeOf(*expr.operands[0]);
    if (!type)
      type = defaultType(*expr.operands[0]);
    if (isa<TextType, StringType>(type))
      return type;
    if (type.isInteger(1))
      return TextType::get(context, 5);
    // An enum as the name of its case.
    if (auto named = dyn_cast<EnumType>(type)) {
      unsigned longest = 1;
      for (const std::string &label :
           enums.ofSymbol(named.getName().getValue()))
        longest = std::max<unsigned>(longest, label.size());
      return TextType::get(context, longest);
    }
    return TextType::get(context, isa<FloatType>(type) ? 28 : 21);
  }
  case Expr::Bool:
    return builder.getI1Type();
  case Expr::Name: {
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind == Variable::Ref)
        return {};
      if (variable->kind == Variable::Entity)
        return EntityType::get(context);
      if (variable->kind == Variable::Row || variable->kind == Variable::Window)
        return {};
      return variable->value.getType();
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end() && unique->second.shorthand)
      return unique->second.fieldType("value");
    return {};
  }
  case Expr::Field: {
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind == Variable::Ref || variable->kind == Variable::Near)
        return recordOf(*variable).fieldType(expr.field);
      if (variable->kind == Variable::Row)
        return typeOf(fieldOfRow(*variable, expr.field, expr.loc));
      if (variable->kind == Variable::Value && variable->value)
        if (RowKind *kind = rowOf(variable->value.getType())) {
          for (auto &[field, type] : kind->fields)
            if (field == expr.field)
              return type;
          return {};
        }
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end() && unique->second.shorthand)
      if (RowKind *kind = rowOf(unique->second.fieldType("value"))) {
        for (auto &[field, type] : kind->fields)
          if (field == expr.field)
            return type;
        return {};
      }
    if (unique != uniques.end())
      return unique->second.fieldType(expr.field);
    // Enum.Case
    if (!lookup(expr.name) && enums.count(expr.name))
      return EnumType::get(context, symbol(expr.name));
    return {};
  }
  case Expr::Unary:
    return expr.op == Token::Not ? builder.getI1Type()
                                 : typeOf(*expr.operands[0]);
  case Expr::Binary:
    switch (expr.op) {
    case Token::Equal:
    case Token::NotEqual:
    case Token::Less:
    case Token::LessEqual:
    case Token::Greater:
    case Token::GreaterEqual:
    case Token::AndAnd:
    case Token::OrOr:
      return builder.getI1Type();
    // (What is shifted, whatever says by how much.)
    case Token::ShiftLeft:
    case Token::ShiftRight:
      return typeOf(*expr.operands[0]);
    default: {
      // Joining texts gives a text that holds both: one of any length
      // where one of them is.
      // (Each side's type is asked for once: asking again for every use
      // takes three times as long with every operator a line is deep.)
      const Expr &left = *expr.operands[0], &right = *expr.operands[1];
      Type type = typeOf(left), other = typeOf(right);
      // A row number and an i32: a row number of the same buffer; two
      // apart: how many rows.
      auto li = dyn_cast_or_null<IndexOfType>(type);
      auto ri = dyn_cast_or_null<IndexOfType>(other);
      if (li || ri) {
        if (li && ri && expr.op == Token::Minus)
          return builder.getI32Type();
        return li ? Type(li) : Type(ri);
      }
      auto text = [&](const Expr &side, Type its) {
        return side.kind == Expr::String ? textTypeOf(side)
               : its && isa<TextType>(its) ? its : Type();
      };
      auto any = [](const Expr &side, Type its) {
        return side.kind != Expr::String && its && isa<StringType>(its);
      };
      Type a = text(left, type), b = text(right, other);
      bool anyA = any(left, type), anyB = any(right, other);
      if (expr.op == Token::Plus && (anyA || anyB) && (a || anyA) &&
          (b || anyB))
        return StringType::get(context);
      if (expr.op == Token::Plus && a && b)
        return TextType::get(
            context, std::min<unsigned>(cast<TextType>(a).getCapacity() +
                                            cast<TextType>(b).getCapacity(),
                                        TextType::kMaxCapacity));
      if (type) {
        if (other)
          if (Type wider = widerInteger(type, other))
            return wider;
        return type;
      }
      return other;
    }
    }
  case Expr::Call:
    if (Callable *through = callableThrough(expr))
      return through->result;
    // (Index.get(key) of a hash index: an entity.)
    if (expr.field == "get")
      if (std::optional<BufferUse> held = bufferNamed(expr.name);
          held && indexes.count(held->symbol))
        return EntityType::get(context);
    // (Index.row(e), Index.find(key): a row of the index.)
    if (expr.field == "row" || expr.field == "find")
      if (std::optional<BufferUse> held = bufferNamed(expr.name);
          held && indexes.count(held->symbol))
        return IndexOfType::get(context,
                                FlatSymbolRefAttr::get(context, held->symbol));
    if (!expr.field.empty())
      return {};
    if (expr.name == "len")
      return builder.getI32Type();
    if (functions.count(expr.name))
      return functions[expr.name].result;
    // (A hash: 64 bits without a sign.)
    if (expr.name == "hash_of")
      return builder.getIntegerType(64, /*isSigned=*/false);
    // (The bits of a float, and the float of bits: of as many bits.)
    if ((expr.name == "bits_of" || expr.name == "float_of") &&
        expr.operands.size() == 1)
      if (Type type = typeOf(*expr.operands[0]))
        if (type.isF32() || type.isF64() || isUnsigned(type)) {
          unsigned width = type.getIntOrFloatBitWidth();
          if (expr.name == "bits_of")
            return builder.getIntegerType(width, /*isSigned=*/false);
          return width == 64 ? Type(builder.getF64Type())
                             : Type(builder.getF32Type());
        }
    for (const ExprPtr &operand : expr.operands)
      if (Type type = typeOf(*operand))
        return type;
    return {};
  case Expr::Cast:
    return expr.castType;
  case Expr::If:
    if (Type type = typeOf(*expr.thenBranch->value))
      return type;
    return typeOf(*expr.elseBranch->value);
  case Expr::Spawn:
    return EntityType::get(context);
  case Expr::Has:
  case Expr::InSet:
    return builder.getI1Type();
  case Expr::Given:
    return expr.given.getType();
  case Expr::Const:
    return constants.ofSymbol(expr.name).type;
  case Expr::Of:
    if (RowKind *kind = rowOf(typeOf(*expr.operands[0])))
      for (auto &[field, type] : kind->fields)
        if (field == expr.field)
          return type;
    return {};
  case Expr::Tuple:
    return {};
  }
  return {};
}

/// The type an expression of literals gets without context: f32 if any of
/// them is a float, i32 otherwise.
Type Parser::defaultType(const Expr &expr) {
  if (expr.kind == Expr::Float)
    return builder.getF32Type();
  if (expr.kind == Expr::Const)
    return constants.ofSymbol(expr.name).type;
  for (const ExprPtr &operand : expr.operands)
    if (defaultType(*operand).isF32())
      return builder.getF32Type();
  return builder.getI32Type();
}

/// Emit a call of a fn or proc that gives at most one value; the value is
/// null if it gives none.
FailureOr<mlir::Value> Parser::emitInvoke(const Expr &expr) {
  size_t count = functions[expr.name].results.size();
  if (count > 1) {
    std::string names = "a";
    for (size_t i = 1; i < count; ++i)
      names += std::string(", ") + char('a' + std::min<size_t>(i, 25));
    return error(expr.loc, "'" + expr.name + "' gives " + Twine(count) +
                               " values; take them with 'let (" + names +
                               ") = " + expr.name + "(...)'");
  }
  FailureOr<SmallVector<mlir::Value>> values = emitCall(expr);
  if (failed(values))
    return failure();
  return values->empty() ? mlir::Value() : values->front();
}

/// Emit a call of a fn or proc, and give what it gives.
FailureOr<SmallVector<mlir::Value>> Parser::emitCall(const Expr &expr) {
  Function &function = functions[expr.name];
  if (!inSystem)
    return error(expr.loc, "'" + expr.name + "' can only be called in a "
                           "system; conditions and 'main' read uniques a "
                           "system has written");
  if (inFunction && function.proc && !inProc)
    return error(expr.loc, "'" + expr.name + "' is a proc: it acts, and a "
                           "fn only computes");
  if (expr.operands.size() != function.params.size())
    return error(expr.loc, "'" + expr.name + "' takes " +
                               Twine(function.params.size()) +
                               " argument(s), not " +
                               Twine(expr.operands.size()));
  SmallVector<mlir::Value> args;
  for (auto [number, operand, type] :
       llvm::enumerate(expr.operands, function.params)) {
    // The entities that have a component are given by its name, or
    // passed on by the name they were given under.
    if (const std::string &component = function.components[number];
        !component.empty()) {
      auto handed = cast<BufferType>(type);
      if (operand->kind != Expr::Name)
        return error(operand->loc, "'" + expr.name + "' is given the "
                                   "component '" + component +
                                       "' here, by its name");
      if (std::optional<ComponentUse> held = componentGiven(operand->name)) {
        if (held->component != component)
          return error(operand->loc, "'" + expr.name + "' is given the "
                                     "component '" + component + "' here");
        if (handed.getMut() && !held->mut)
          return error(operand->loc,
                       "'" + operand->name + "' was given to read, and '" +
                           expr.name + "' changes what it is given");
        args.push_back(held->given);
        continue;
      }
      if (operand->name != component || lookup(operand->name))
        return error(operand->loc, "'" + expr.name + "' is given the "
                                   "component '" + component +
                                       "' here, by its name");
      if (inFunction)
        return error(operand->loc,
                     "a fn or proc passes on a component it was given; a "
                     "system gives one");
      if (inQuery && !inTurn)
        return error(operand->loc,
                     "the entities that have a component are given outside "
                     "a 'for', or in one whose entities take turns ('in "
                     "turn by'): what is read of them would depend on the "
                     "order else");
      args.push_back(BufferOfOp::create(builder, loc(operand->loc), type,
                                        symbol(component))
                         .getResult());
      continue;
    }
    // A buffer is handed by its name.
    if (auto handed = dyn_cast<BufferType>(type)) {
      std::optional<BufferUse> held = operand->kind == Expr::Name
                                          ? bufferNamed(operand->name)
                                          : std::nullopt;
      if (!held || held->symbol != handed.getName().getValue())
        return error(operand->loc,
                     "'" + expr.name + "' is handed the buffer '" +
                         handed.getName().getValue() + "' here, by its name");
      if (failed(notHashRows(*held, operand->name, operand->loc)))
        return failure();
      if (handed.getMut() && !held->mut && indexes.count(held->symbol))
        return error(operand->loc,
                     "'" + operand->name + "' is an index, whose rows are "
                     "only read, and '" + expr.name + "' changes the buffer "
                     "it is given");
      if (handed.getMut() && !held->mut)
        return error(operand->loc,
                     "'" + operand->name + "' was given to read, and '" +
                         expr.name + "' changes the buffer it is given");
      // (One that was given: passed on as it is.)
      if (held->given) {
        args.push_back(held->given);
        continue;
      }
      if (inFunction)
        return error(operand->loc,
                     "'" + operand->name + "' is a buffer, which a system "
                     "hands over: a fn or proc passes on one it is given");
      // One with a body may read it for each entity of a `for`.
      if (inQuery && !inTurn && (function.isExtern || handed.getMut()))
        return error(operand->loc,
                     handed.getMut()
                         ? "a buffer is given to be changed outside a "
                           "'for': not once for each entity"
                         : "a buffer is handed over outside a 'for': not "
                           "once for each entity");
      args.push_back(
          BufferOfOp::create(builder, loc(operand->loc), type,
                             FlatSymbolRefAttr::get(context, held->symbol))
              .getResult());
      continue;
    }
    FailureOr<mlir::Value> arg = emit(*operand, type);
    if (failed(arg))
      return failure();
    if (arg->getType() != type)
      return error(operand->loc, "argument has a different type than the "
                                 "parameter");
    args.push_back(*arg);
  }
  auto invoke = InvokeOp::create(
      builder, loc(expr.loc), function.results, symbol(expr.name), args,
      function.proc ? builder.getUnitAttr() : UnitAttr());
  return SmallVector<mlir::Value>(invoke.getResults());
}

/// Emit an expression that is several values, as many as `expected` has
/// types (a null one leaves the type to the value): `(a, b)`, a call of a
/// fn that gives them, or an `if` whose branches do.
FailureOr<SmallVector<mlir::Value>>
Parser::emitSeveral(const Expr &expr, ArrayRef<Type> expected) {
  Location at = loc(expr.loc);
  auto check = [&](ArrayRef<mlir::Value> values,
                   ArrayRef<const Expr *> from) -> LogicalResult {
    if (values.size() != expected.size())
      return error(expr.loc, "expected " + Twine(expected.size()) +
                                 " values, found " + Twine(values.size()));
    for (auto [index, value] : llvm::enumerate(values))
      if (expected[index] && value.getType() != expected[index])
        return error(from.empty() ? expr.loc : from[index]->loc,
                     "value #" + Twine(index + 1) +
                         " has a different type than expected");
    return success();
  };
  switch (expr.kind) {
  case Expr::Tuple: {
    if (expr.operands.size() != expected.size())
      return error(expr.loc, "expected " + Twine(expected.size()) +
                                 " values, found " +
                                 Twine(expr.operands.size()));
    SmallVector<mlir::Value> values;
    SmallVector<const Expr *> from;
    for (auto [operand, type] : llvm::zip(expr.operands, expected)) {
      FailureOr<mlir::Value> value = emit(*operand, type);
      if (failed(value))
        return failure();
      values.push_back(*value);
      from.push_back(operand.get());
    }
    if (failed(check(values, from)))
      return failure();
    return values;
  }
  case Expr::Call: {
    // `mul_wide(a, b)`: the 128 bits of a u64 times a u64, as its high
    // and its low 64 (Go's bits.Mul64): one instruction on most machines.
    if (expr.field.empty() && expr.name == "mul_wide" &&
        !functions.count(expr.name)) {
      if (expr.operands.size() != 2)
        return error(expr.loc, "'mul_wide' takes two arguments");
      Type word = builder.getIntegerType(64, /*isSigned=*/false);
      FailureOr<mlir::Value> a = emit(*expr.operands[0], word);
      FailureOr<mlir::Value> b = emit(*expr.operands[1], word);
      if (failed(a) || failed(b))
        return failure();
      if (a->getType() != word || b->getType() != word)
        return error(expr.loc, "'mul_wide' multiplies two u64");
      auto product = arith::MulUIExtendedOp::create(
          builder, at, bits(at, *a), bits(at, *b));
      SmallVector<mlir::Value> values = {fromBits(at, product.getHigh(), word),
                                         fromBits(at, product.getLow(), word)};
      if (failed(check(values, {})))
        return failure();
      return values;
    }
    if (!expr.field.empty() || !functions.count(expr.name) ||
        functions[expr.name].results.size() < 2)
      break;
    FailureOr<SmallVector<mlir::Value>> values = emitCall(expr);
    if (failed(values) || failed(check(*values, {})))
      return failure();
    return values;
  }
  case Expr::If: {
    FailureOr<mlir::Value> condition =
        emit(*expr.operands[0], builder.getI1Type());
    if (failed(condition))
      return failure();
    if (!condition->getType().isInteger(1))
      return error(expr.loc, "an 'if' condition must be a bool");
    auto branch = scf::IfOp::create(builder, at, *condition,
                                    /*withElseRegion=*/true);
    // The first branch says what the types are where nothing else does.
    SmallVector<Type> types(expected);
    SmallVector<mlir::Value> thenValues, elseValues;
    for (auto [block, part, values] :
         {std::make_tuple(branch.thenBlock(), expr.thenBranch.get(),
                          &thenValues),
          std::make_tuple(branch.elseBlock(), expr.elseBranch.get(),
                          &elseValues)}) {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPoint(block->getTerminator());
      ScopeGuard scope(*this);
      for (const Let &let : part->lets)
        if (failed(emitLet(let, /*isVar=*/false)))
          return failure();
      FailureOr<SmallVector<mlir::Value>> given =
          emitSeveral(*part->value, types);
      if (failed(given))
        return failure();
      *values = std::move(*given);
      for (auto [type, value] : llvm::zip(types, *values))
        type = value.getType();
    }
    scf::IfOp merged = giveFromBranches(branch, thenValues, elseValues);
    return SmallVector<mlir::Value>(merged.getResults());
  }
  default:
    break;
  }
  return error(expr.loc, "expected " + Twine(expected.size()) +
                             " values: '(a, b)', or a fn that gives them");
}

FailureOr<mlir::Value> Parser::emit(const Expr &expr, Type expected) {
  FailureOr<mlir::Value> value = emitRaw(expr, expected);
  if (failed(value))
    return failure();
  // A text goes where a text of another capacity is expected: widened, or
  // cut to what fits.
  // A whole number goes where one of more bits is expected, as it is.
  if (expected && widerInteger(value->getType(), expected) == expected &&
      value->getType() != expected) {
    Location at = loc(expr.loc);
    if (isUnsigned(expected))
      return fromBits(at,
                      arith::ExtUIOp::create(builder, at, bitsType(expected),
                                             bits(at, *value))
                          .getResult(),
                      expected);
    return arith::ExtSIOp::create(builder, at, expected, *value).getResult();
  }
  auto from = dyn_cast<TextType>(value->getType());
  auto to = dyn_cast_or_null<TextType>(expected);
  if (from && to && from != to)
    return textResize(loc(expr.loc), *value, to);
  // A text of a capacity where one of any length is expected, and the
  // other way round: as it is, or cut to what fits.
  if (from && expected && isa<StringType>(expected)) {
    mlir::Value seen =
        TextOfOp::create(builder, loc(expr.loc), expected, *value).getResult();
    // (What a fn gives back of its own is gone when the fn has run: a
    // copy that is kept while the schedule runs.)
    if (givesText)
      return TextKeepOp::create(builder, loc(expr.loc), expected, seen)
          .getResult();
    return seen;
  }
  if (to && isa<StringType>(value->getType()))
    return TextCutOp::create(builder, loc(expr.loc), to, *value).getResult();
  return value;
}

FailureOr<mlir::Value> Parser::emitRaw(const Expr &expr, Type expected) {
  Location at = loc(expr.loc);
  // A number written out where a row number is expected is one (`-1`,
  // `0`), and `none` is -1: no row.
  if (auto index = dyn_cast_or_null<IndexOfType>(expected)) {
    if (expr.kind == Expr::Name && expr.name == "none" && !lookup(expr.name))
      return asIndex(at, integer(at, builder.getI32Type(), -1), index);
    if (!typeOf(expr) && (expr.kind == Expr::Int || expr.kind == Expr::Unary ||
                          expr.kind == Expr::Binary)) {
      FailureOr<mlir::Value> number = emit(expr, builder.getI32Type());
      if (failed(number))
        return failure();
      if (!number->getType().isInteger(32))
        return error(expr.loc, "a row number is made of an i32");
      return asIndex(at, *number, index);
    }
  }
  // A row of a table, where one is expected as a value.
  if (RowKind *kind = rowOf(expected))
    if (isRowSource(expr))
      return emitRowValue(expr, *kind);
  switch (expr.kind) {
  case Expr::String: {
    // A literal takes the capacity it is put into, where it fits.
    unsigned size = expr.name.size();
    if (expected && isa<StringType>(expected))
      return TextConstantOp::create(builder, at, expected, expr.name)
          .getResult();
    if (auto text = dyn_cast_or_null<TextType>(expected)) {
      if (size > text.getCapacity())
        return error(expr.loc, "this text has " + Twine(size) +
                                   " bytes, but it is put into a text[" +
                                   Twine(text.getCapacity()) + "]");
      return textConstant(at, expr.name, text);
    }
    if (size > TextType::kMaxCapacity)
      return error(expr.loc, "a text holds at most " +
                                 Twine(unsigned(TextType::kMaxCapacity)) +
                                 " bytes");
    return textConstant(at, expr.name,
                        TextType::get(context, std::max(size, 1u)));
  }
  case Expr::Row: {
    if (expr.name.empty()) {
      RowsShape *shape = rowsOf(typeOf(*expr.operands[1]));
      if (!shape)
        return error(expr.loc, "only a row of a table has fields");
      return emitRowThrough(expr, *shape, *expr.operands[1],
                            *expr.operands[0], expr.field);
    }
    // w[i].field, of a window of a `for`: the row the entity took.
    if (const Variable *window = lookup(expr.name);
        window && window->kind == Variable::Window) {
      Variable held = *window;
      Type type = buffers.ofSymbol(held.component).fieldType(expr.field);
      if (!type)
        return error(expr.loc,
                     expr.field.empty()
                         ? "a row of window '" + expr.name +
                               "' is read by its fields: '" + expr.name +
                               "[i].field'"
                         : "the buffer of window '" + expr.name +
                               "' has no field '" + expr.field + "'");
      FailureOr<mlir::Value> row = windowRow(held, *expr.operands[0]);
      if (failed(row))
        return failure();
      auto read = BufferAtOp::create(
          builder, at, type, FlatSymbolRefAttr::get(context, held.component),
          builder.getStringAttr(expr.field), *row);
      read->setAttr(kWindowAttr, builder.getUnitAttr());
      return read.getResult();
    }
    // name[entity].field, of a component that was given: the field of
    // the entity that has it, or nought where it has not.
    if (std::optional<ComponentUse> held = componentGiven(expr.name)) {
      Type type = components[held->component].fieldType(expr.field);
      if (!type)
        return error(expr.loc,
                     expr.field.empty()
                         ? "an entity of '" + expr.name + "' is read by "
                           "its fields: '" + expr.name + "[entity].field'"
                         : "component '" + held->component +
                               "' has no field '" + expr.field + "'");
      if (isa<TextType, StringType>(type))
        return error(expr.loc, "a text of another entity is not read here "
                               "yet");
      FailureOr<mlir::Value> entity =
          emit(*expr.operands[0], EntityType::get(context));
      if (failed(entity))
        return failure();
      if (!isa<EntityType>(entity->getType()))
        return error(expr.operands[0]->loc,
                     "an entity says which: '" + expr.name +
                         "[entity].field'");
      return LookupOp::create(builder, at, type, builder.getI1Type(),
                              *entity, symbol(held->component),
                              builder.getStringAttr(expr.field))
          .getValue();
    }
    if (std::optional<BufferUse> held = bufferNamed(expr.name)) {
      if (failed(notHashRows(*held, expr.name, expr.loc)))
        return failure();
      Type type = buffers.ofSymbol(held->symbol).fieldType(expr.field);
      if (!type)
        return error(expr.loc,
                     expr.field.empty()
                         ? "a row of buffer '" + expr.name +
                               "' is read by its fields: '" + expr.name +
                               "[i].field'"
                         : "buffer '" + expr.name + "' has no field '" +
                               expr.field + "'");
      if (inFunction && !held->given)
        return error(expr.loc, "'" + expr.name + "' is a buffer, which a "
                               "system reads: a fn or proc reads one it is "
                               "given ('name: " + expr.name + "')");
      FailureOr<mlir::Value> index =
          emitRowNumber(*expr.operands[0],
                        FlatSymbolRefAttr::get(context, held->symbol),
                        expr.name);
      if (failed(index))
        return failure();
      return BufferAtOp::create(builder, at, type,
                                FlatSymbolRefAttr::get(context, held->symbol),
                                builder.getStringAttr(expr.field), *index)
          .getResult();
    }
    const Record &table = tables[expr.name];
    Type type = table.fieldType(expr.field);
    if (!type)
      return error(expr.loc,
                   expr.field.empty()
                       ? "a row of table '" + expr.name + "' has fields: '" +
                             expr.name + "[i].field'"
                       : table.fields.size() == 1 &&
                                 table.fields[0].first.empty()
                             ? "table '" + expr.name +
                                   "' is a plain list: '" + expr.name + "[i]'"
                             : "table '" + expr.name + "' has no field '" +
                                   expr.field + "'");
    // A table with a row for each case of an enum is read by a case.
    auto keyed = tableKeys.find(resolve(expr.name));
    if (keyed != tableKeys.end()) {
      auto named = EnumType::get(
          context, FlatSymbolRefAttr::get(context, keyed->second));
      FailureOr<mlir::Value> which = emit(*expr.operands[0], named);
      if (failed(which))
        return failure();
      if (which->getType() != named)
        return error(expr.operands[0]->loc,
                     "table '" + expr.name + "' has a row for each case of "
                     "an enum, and is read by one ('" + expr.name +
                         "[case]'), not by a number");
      mlir::Value byte = UnrealizedConversionCastOp::create(
                             builder, at, named.getStorageType(), *which)
                             .getResult(0);
      mlir::Value row =
          arith::ExtUIOp::create(builder, at, builder.getI32Type(), byte);
      return TableAtOp::create(builder, at, type, symbol(expr.name),
                               builder.getStringAttr(expr.field), row)
          .getResult();
    }
    Type indexTy = typeOf(*expr.operands[0]);
    if (!indexTy)
      indexTy = builder.getI32Type();
    FailureOr<mlir::Value> index = emit(*expr.operands[0], indexTy);
    if (failed(index))
      return failure();
    if (isa<EnumType>(index->getType()))
      return error(expr.operands[0]->loc,
                   "table '" + expr.name + "' has rows that are counted; one "
                   "with a row for each case is declared 'table " +
                       expr.name + "[Enum]'");
    *index = countable(at, *index);
    if (!index->getType().isSignlessInteger() || index->getType().isInteger(1))
      return error(expr.operands[0]->loc, "a row's number is an integer");
    return TableAtOp::create(builder, at, type, symbol(expr.name),
                             builder.getStringAttr(expr.field), *index)
        .getResult();
  }
  case Expr::Index: {
    if (RowsShape *shape = rowsOf(typeOf(*expr.operands[0])))
      return emitRowThrough(expr, *shape, *expr.operands[0],
                            *expr.operands[1], "");
    Type textTy = textTypeOf(*expr.operands[0]);
    if (isAnyText(*expr.operands[0])) {
      FailureOr<mlir::Value> text =
          emit(*expr.operands[0], StringType::get(context));
      Type indexTy = typeOf(*expr.operands[1]);
      if (!indexTy)
        indexTy = builder.getI32Type();
      FailureOr<mlir::Value> index = emit(*expr.operands[1], indexTy);
      if (failed(text) || failed(index))
        return failure();
      *index = countable(at, *index);
      if (!index->getType().isSignlessInteger() ||
          index->getType().isInteger(1))
        return error(expr.operands[1]->loc, "an index is an integer");
      return TextAtOp::create(builder, at, builder.getIntegerType(8), *text,
                              *index)
          .getResult();
    }
    if (!textTy)
      return error(expr.loc, "only a text can be indexed");
    FailureOr<mlir::Value> text = emit(*expr.operands[0], textTy);
    Type indexTy = typeOf(*expr.operands[1]);
    if (!indexTy)
      indexTy = builder.getI32Type();
    FailureOr<mlir::Value> index = emit(*expr.operands[1], indexTy);
    if (failed(text) || failed(index))
      return failure();
    *index = countable(at, *index);
    if (!index->getType().isSignlessInteger() || index->getType().isInteger(1))
      return error(expr.operands[1]->loc, "an index is an integer");
    return textIndex(at, *text, *index);
  }
  case Expr::Format: {
    Type type = typeOf(*expr.operands[0]);
    if (!type)
      type = defaultType(*expr.operands[0]);
    // (One of any length as it is: what it is joined with makes the
    // text.)
    if (isa<StringType>(type))
      return emit(*expr.operands[0], type);
    FailureOr<mlir::Value> value = emit(*expr.operands[0], type);
    if (failed(value))
      return failure();
    return formatValue(expr.loc, *value);
  }
  case Expr::Int: {
    if (expr.isByte)
      return integer(at, builder.getIntegerType(8), expr.intValue);
    Type type = expected ? expected : builder.getI32Type();
    if (expr.isHex && isa<FloatType>(type))
      return error(expr.loc, "a hex literal is the bits of an integer; "
                             "it cannot be a float");
    if (isa<FloatType>(type))
      return arith::ConstantOp::create(
                 builder, at,
                 builder.getFloatAttr(type, double(expr.intValue)))
          .getResult();
    if (!type.isIntOrIndex() || type.isInteger(1))
      return error(expr.loc, "an integer cannot be used here");
    if (expr.isHex) {
      // The bits of the value, whatever number they make: 0xff is an i8.
      unsigned width = type.isIndex() ? 64 : type.getIntOrFloatBitWidth();
      uint64_t bits = uint64_t(expr.intValue);
      if (width < 64 && bits >> width != 0)
        return error(expr.loc, "this hex literal has more than the " +
                                   Twine(width) + " bits of its type");
      return integer(at, type, bits);
    }
    if (isUnsigned(type)) {
      // (A literal has no sign of its own: '-' before it is computed.)
      unsigned width = type.getIntOrFloatBitWidth();
      if (width < 64 && uint64_t(expr.intValue) >> width != 0)
        return error(expr.loc, "this number is more than a u" + Twine(width) +
                                   " holds");
      return integer(at, type, uint64_t(expr.intValue));
    }
    return arith::ConstantOp::create(
               builder, at, builder.getIntegerAttr(type, expr.intValue))
        .getResult();
  }
  case Expr::Float: {
    Type type = expected && isa<FloatType>(expected) ? expected
                                                     : builder.getF32Type();
    if (expected && !isa<FloatType>(expected))
      return error(expr.loc, "a float cannot be used here");
    return arith::ConstantOp::create(
               builder, at, builder.getFloatAttr(type, expr.floatValue))
        .getResult();
  }
  case Expr::Bool:
    return arith::ConstantOp::create(builder, at,
                                     builder.getBoolAttr(expr.boolValue))
        .getResult();
  case Expr::Name: {
    // Rows of a buffer, where a fn or proc takes them: a window's, or all
    // a buffer has.
    if (auto rows = dyn_cast_or_null<RowsType>(expected)) {
      auto target = rows.getName();
      std::string wanted = target.getValue().str();
      const Variable *window = lookup(expr.name);
      if (window && window->kind == Variable::Window) {
        if (window->component != wanted)
          return error(expr.loc, "'" + expr.name + "' is a window of '" +
                                     window->component + "', and rows of '" +
                                     wanted + "' are taken here");
        if (rows.getMut() && !window->mut)
          return error(expr.loc, "'" + expr.name + "' is a window to read, "
                                 "and the rows are taken here to change: "
                                 "'name: mut " + wanted + "[start, count]'");
        return BufferRowsOp::create(builder, at, rows, target, window->value,
                                    window->which)
            .getResult();
      }
      std::optional<BufferUse> held = bufferNamed(expr.name);
      if (!held || held->symbol != wanted)
        return error(expr.loc, "rows of '" + wanted + "' are taken here: a "
                               "window of it, or the buffer itself");
      if (rows.getMut() && !held->mut)
        return error(expr.loc, "'" + expr.name + "' was given to read, and "
                               "its rows are taken here to change");
      if (rows.getMut() && inQuery && !inTurn)
        return error(expr.loc, "a 'for' changes rows of a buffer through "
                               "windows of its entities ('w: mut " + wanted +
                                   "[start, count]'), not all of it");
      mlir::Value count =
          BufferLenOp::create(builder, at, builder.getI32Type(), target)
              .getResult();
      return BufferRowsOp::create(builder, at, rows, target,
                                  integer(at, builder.getI32Type(), 0), count)
          .getResult();
    }
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind == Variable::Ref)
        return error(expr.loc, "'" + expr.name +
                                   "' is a component; read a field of it: '" +
                                   expr.name + ".field'");
      if (variable->kind == Variable::Entity)
        return EntityOp::create(builder, at, EntityType::get(context))
            .getResult();
      if (variable->kind == Variable::Row)
        return error(expr.loc, "'" + expr.name + "' is a row of a table; "
                               "read a field of it: '" + expr.name +
                                   ".field'");
      if (variable->kind == Variable::Window)
        return error(expr.loc, "'" + expr.name + "' is a window of a "
                               "buffer; read a row of it: '" + expr.name +
                                   "[i].field'");
      return variable->value;
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end() && inFunction)
      return error(expr.loc, "a fn computes from its parameters only; pass "
                             "it what it needs of unique '" +
                                 expr.name + "'");
    if (unique != uniques.end()) {
      if (!unique->second.shorthand)
        return error(expr.loc, "unique '" + expr.name +
                                   "' has fields; read one with '" +
                                   expr.name + ".field'");
      return ReadOp::create(builder, at, unique->second.fieldType("value"),
                            symbol(expr.name), builder.getStringAttr("value"))
          .getResult();
    }
    // A table, or `none`, where one of a shape is expected.
    if (RowsShape *shape = rowsOf(expected)) {
      if (tables.count(expr.name))
        return emitTableValue(expr, *shape);
      if (expr.name == "none")
        return noughtOf(at, expected);
    }
    // `none`, where an entity is expected: no entity.
    if (expr.name == "none" && expected && isa<EntityType>(expected))
      return NobodyOp::create(builder, at, expected).getResult();
    // `none`, where a fn or proc is expected: no function. Calling it
    // does nothing, and gives nought.
    if (expr.name == "none")
      if (Callable *callable = callableOf(expected)) {
        auto type = EnumType::get(
            context, FlatSymbolRefAttr::get(context, callable->name));
        return UnrealizedConversionCastOp::create(
                   builder, at, type, integer(at, type.getStorageType(), 0))
            .getResult(0);
      }
    // A fn or proc by its name, where one of its shape is expected.
    if (functions.count(expr.name)) {
      if (Callable *callable = callableOf(expected))
        return emitFunctionValue(expr, *callable);
      return error(expr.loc, "'" + expr.name + "' is a " +
                                 (functions[expr.name].proc ? "proc" : "fn") +
                                 ": call it, or give it where a field or a "
                                 "parameter says what it takes and gives "
                                 "('fn(f32) -> f32', 'proc(i32)')");
    }
    return error(expr.loc, "unknown name '" + expr.name + "'");
  }
  case Expr::Field: {
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind == Variable::Row)
        return emit(fieldOfRow(*variable, expr.field, expr.loc), expected);
      if (variable->kind == Variable::Value && variable->value &&
          rowOf(variable->value.getType()))
        return readRowValue(expr.loc, variable->value, expr.field);
      if (variable->kind == Variable::Near) {
        Type type = recordOf(*variable).fieldType(expr.field);
        if (!type)
          return error(expr.loc, "component '" + variable->component +
                                     "' has no field '" + expr.field + "'");
        std::string component = resolve(variable->component);
        // Where it is: as seen from the place. What the index copies:
        // from its row. Else looked up (it has the component).
        for (auto &[of, field, seen] : nearRows->seen)
          if (of == component && field == expr.field)
            return seen;
        for (auto &[of, field, name] : indexes[nearRows->index].copied)
          if (of == component && field == expr.field)
            return BufferAtOp::create(
                       builder, at, type,
                       FlatSymbolRefAttr::get(context, nearRows->index),
                       builder.getStringAttr(name), nearRows->row)
                .getResult();
        return LookupOp::create(builder, at, type, builder.getI1Type(),
                                variable->value, symbol(variable->component),
                                builder.getStringAttr(expr.field))
            .getValue();
      }
      if (variable->kind == Variable::Other) {
        Type type = recordOf(*variable).fieldType(expr.field);
        if (!type)
          return error(expr.loc, "component '" + variable->component +
                                     "' has no field '" + expr.field + "'");
        // (The loop's body runs where the entity has the component.)
        return LookupOp::create(builder, at, type, builder.getI1Type(),
                                variable->value, symbol(variable->component),
                                builder.getStringAttr(expr.field))
            .getValue();
      }
      if (variable->kind != Variable::Ref)
        return error(expr.loc, "'" + expr.name + "' has no fields");
      Type type = recordOf(*variable).fieldType(expr.field);
      if (!type)
        return error(expr.loc, std::string(relations.count(variable->component)
                                               ? "relation '"
                                               : "component '") +
                                   variable->component + "' has no field '" +
                                   expr.field + "'");
      if (variable->value)
        if (auto ref = dyn_cast<RefType>(variable->value.getType());
            ref && ref.getIsOptional())
          return error(expr.loc, "'" + expr.name + "' is optional and may "
                                 "lead nowhere: read it with 'if let x = " +
                                     expr.name + "." + expr.field +
                                     " { ... } else { ... }'");
      return GetOp::create(builder, at, type, variable->value,
                           builder.getStringAttr(expr.field))
          .getResult();
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end() && inFunction)
      return error(expr.loc, "a fn computes from its parameters only; pass "
                             "it what it needs of unique '" +
                                 expr.name + "'");
    // (A unique that is a row: a field of the row.)
    if (unique != uniques.end() && unique->second.shorthand &&
        rowOf(unique->second.fieldType("value"))) {
      mlir::Value row =
          ReadOp::create(builder, at, unique->second.fieldType("value"),
                         symbol(expr.name), builder.getStringAttr("value"))
              .getResult();
      return readRowValue(expr.loc, row, expr.field);
    }
    if (unique != uniques.end()) {
      Type type = unique->second.fieldType(expr.field);
      if (!type)
        return error(expr.loc, "unique '" + expr.name + "' has no field '" +
                                   expr.field + "'");
      return ReadOp::create(builder, at, type, symbol(expr.name),
                            builder.getStringAttr(expr.field))
          .getResult();
    }
    // Enum.Case: the byte that numbers the case, as the enum's type.
    auto named = enums.find(expr.name);
    if (named != enums.end()) {
      auto label = llvm::find(named->second, expr.field);
      if (label == named->second.end())
        return error(expr.loc, "enum '" + expr.name + "' has no case '" +
                                   expr.field + "'");
      auto type = EnumType::get(context, symbol(expr.name));
      mlir::Value number = integer(at, type.getStorageType(),
                                   label - named->second.begin());
      return UnrealizedConversionCastOp::create(builder, at, type, number)
          .getResult(0);
    }
    return error(expr.loc, "unknown name '" + expr.name + "'");
  }
  case Expr::Unary: {
    if (expr.op == Token::Not) {
      FailureOr<mlir::Value> operand =
          emit(*expr.operands[0], builder.getI1Type());
      if (failed(operand))
        return failure();
      if (!operand->getType().isInteger(1))
        return error(expr.loc, "'!' needs a bool");
      return arith::XOrIOp::create(
                 builder, at, *operand,
                 arith::ConstantOp::create(builder, at,
                                           builder.getBoolAttr(true)))
          .getResult();
    }
    Type type = typeOf(*expr.operands[0]);
    if (!type)
      type = expected ? expected : defaultType(*expr.operands[0]);
    FailureOr<mlir::Value> operand = emit(*expr.operands[0], type);
    if (failed(operand))
      return failure();
    // (A row number: computed with as its i32.)
    if (isa<IndexOfType>(operand->getType())) {
      mlir::Value i32 = integer(at, builder.getI32Type(),
                                expr.op == Token::Tilde ? -1 : 0);
      return expr.op == Token::Tilde
                 ? arithmetic(at, Token::Caret, *operand, i32)
                 : arithmetic(at, Token::Minus, i32, *operand);
    }
    if (expr.op == Token::Tilde) {
      // Every bit turned over.
      if (!isWhole(operand->getType()) && !operand->getType().isIndex())
        return error(expr.loc, "'~' turns over the bits of a whole number");
      return arithmetic(at, Token::Caret, *operand,
                        integer(at, operand->getType(), ~uint64_t(0)));
    }
    if (isa<FloatType>(operand->getType()))
      return arith::NegFOp::create(builder, at, *operand).getResult();
    if (!operand->getType().isIntOrIndex() || operand->getType().isInteger(1))
      return error(expr.loc, "'-' needs a number");
    return arithmetic(at, Token::Minus, integer(at, operand->getType(), 0),
                      *operand);
  }
  case Expr::Binary:
    return emitBinary(expr, expected);
  case Expr::Call: {
    if (expr.field == "row" || expr.field == "find" || expr.field == "get")
      if (std::optional<BufferUse> held = bufferNamed(expr.name);
          held && indexes.count(held->symbol))
        return emitIndexCall(expr, held->symbol, held->given);
    // (A name or a field that holds a fn or proc: a call of that one.)
    if (!expr.field.empty() && !callableThrough(expr))
      return error(expr.loc, "'" + expr.name + "." + expr.field +
                                 "' is not a fn or proc to call");
    if (Callable *through = callableThrough(expr)) {
      if (inFunction && through->proc && !inProc)
        return error(expr.loc, "'" + expr.name + "' is a proc: it acts, and "
                               "a fn only computes");
      if (!through->result)
        return error(expr.loc, "'" + expr.name + "' gives no value");
      FailureOr<SmallVector<mlir::Value>> values =
          emitCallThrough(expr, *through);
      if (failed(values))
        return failure();
      return values->front();
    }
    // (Of the table a value holds: asked of the fn that knows them.)
    if (expr.name == "len" && expr.operands.size() == 1)
      if (RowsShape *shape = rowsOf(typeOf(*expr.operands[0]))) {
        auto held = EnumType::get(
            context, FlatSymbolRefAttr::get(context, shape->name));
        FailureOr<mlir::Value> which = emit(*expr.operands[0], held);
        if (failed(which))
          return failure();
        shape->counted = true;
        return InvokeOp::create(
                   builder, at, builder.getI32Type(),
                   FlatSymbolRefAttr::get(context, "rows_" + shape->name),
                   ValueRange{*which}, UnitAttr())
            ->getResult(0);
      }
    // (Of a window: how many rows it has.)
    if (expr.name == "len" && expr.operands.size() == 1 &&
        expr.operands[0]->kind == Expr::Name)
      if (const Variable *window = lookup(expr.operands[0]->name);
          window && window->kind == Variable::Window)
        return window->which;
    // (Of a buffer: how many rows it has now.)
    if (expr.name == "len" && expr.operands.size() == 1 &&
        expr.operands[0]->kind == Expr::Name &&
        bufferNamed(expr.operands[0]->name)) {
      BufferUse held = *bufferNamed(expr.operands[0]->name);
      if (failed(notHashRows(held, expr.operands[0]->name, expr.loc)))
        return failure();
      if (inFunction && !held.given)
        return error(expr.loc, "'" + expr.operands[0]->name + "' is a "
                               "buffer, which a system reads: a fn or proc "
                               "reads one it is given ('name: " +
                                   expr.operands[0]->name + "')");
      return BufferLenOp::create(builder, at, builder.getI32Type(),
                                 FlatSymbolRefAttr::get(context, held.symbol))
          .getResult();
    }
    // (Of a set: how many entities it holds now.)
    if (expr.name == "len" && expr.operands.size() == 1 &&
        expr.operands[0]->kind == Expr::Name &&
        !lookup(expr.operands[0]->name) && sets.count(expr.operands[0]->name)) {
      if (inFunction)
        return error(expr.loc,
                     "'" + expr.operands[0]->name +
                         "' is a set, "
                         "which a system reads; a fn or proc does not");
      return BufferLenOp::create(builder, at, builder.getI32Type(),
                                 symbol(expr.operands[0]->name))
          .getResult();
    }
    // (Of a table: how many rows it has, which is known.)
    if (expr.name == "len" && expr.operands.size() == 1 &&
        expr.operands[0]->kind == Expr::Name &&
        !lookup(expr.operands[0]->name) &&
        tables.count(expr.operands[0]->name))
      return integer(at, builder.getI32Type(),
                     tableRows[resolve(expr.operands[0]->name)]);
    if (expr.name == "len" && expr.operands.size() == 1 &&
        isAnyText(*expr.operands[0])) {
      FailureOr<mlir::Value> text =
          emit(*expr.operands[0], StringType::get(context));
      if (failed(text))
        return failure();
      return TextLengthOp::create(builder, at, builder.getI32Type(), *text)
          .getResult();
    }
    if (expr.name == "len") {
      Type textTy = expr.operands.size() == 1
                        ? textTypeOf(*expr.operands[0])
                        : Type();
      if (!textTy)
        return error(expr.loc, "'len' takes one text");
      FailureOr<mlir::Value> text = emit(*expr.operands[0], textTy);
      if (failed(text))
        return failure();
      return textLength(at, *text);
    }
    if (functions.count(expr.name)) {
      if (inFunction && functions[expr.name].proc && !inProc)
        return error(expr.loc, "'" + expr.name + "' is a proc: it acts, and "
                               "a fn only computes");
      if (functions[expr.name].results.empty())
        return error(expr.loc, "'" + expr.name + "' gives no value");
      return emitInvoke(expr);
    }
    if (expr.name == "mul_wide")
      return error(expr.loc, "'mul_wide' gives 2 values, the high and the "
                             "low bits; take them with 'let (hi, lo) = "
                             "mul_wide(a, b)'");
    // (In what a unique index declares: a second entity of a key stops
    // the program, naming them.)
    if (expr.name == "index_duplicate" && buildingIndex &&
        expr.operands.size() == 3) {
      SmallVector<mlir::Value, 3> values;
      for (const ExprPtr &operand : expr.operands) {
        FailureOr<mlir::Value> value = emit(*operand, typeOf(*operand));
        if (failed(value))
          return failure();
        values.push_back(*value);
      }
      IndexDuplicateOp::create(
          builder, at, FlatSymbolRefAttr::get(context, buildingIndexName),
          values[0], values[1], values[2]);
      return mlir::Value();
    }
    // `hash_of(x)`: 64 bits mixed of a whole number, an enum, an entity
    // or a row number (ent.hash), or of a text's bytes: 16 at a time, as
    // two words of 8 (the bytes from the lowest), mixed as rapidhash's
    // blocks are (`rapid_mix(a ^ secret, b ^ hash)`), then the length.
    // The same on every machine and every run; for an index's slots.
    if (expr.name == "hash_of") {
      if (expr.operands.size() != 1 && expr.operands.size() != 2)
        return error(expr.loc, "'hash_of' takes a value, and a seed (u64)");
      Type i64 = builder.getI64Type();
      auto constant = [&](uint64_t value) -> mlir::Value {
        return arith::ConstantIntOp::create(builder, at, i64, int64_t(value));
      };
      // rapidhash's secrets and its seed.
      mlir::Value secret0 = constant(0x2d358dccaa6c78a5ull);
      mlir::Value secret1 = constant(0x8bb84b93962eacc9ull);
      mlir::Value secret2 = constant(0x4b33a62ed433d4a3ull);
      auto mix = [&](mlir::Value a, mlir::Value b) -> mlir::Value {
        auto wide = arith::MulUIExtendedOp::create(builder, at, a, b);
        return arith::XOrIOp::create(builder, at, wide.getLow(),
                                     wide.getHigh());
      };
      const Expr &operand = *expr.operands[0];
      // (`hash_of(x, seed)`: another hash of the same, by a seed.)
      mlir::Value seeded;
      if (expr.operands.size() == 2) {
        FailureOr<mlir::Value> given = emit(
            *expr.operands[1], builder.getIntegerType(64, /*isSigned=*/false));
        if (failed(given))
          return failure();
        if (!isUnsigned(given->getType()) ||
            given->getType().getIntOrFloatBitWidth() != 64)
          return error(expr.operands[1]->loc, "a seed is a u64");
        seeded = UnrealizedConversionCastOp::create(builder, at, i64, *given)
                     .getResult(0);
      }
      if (!isAnyText(operand) && !textTypeOf(operand)) {
        Type type = typeOf(operand);
        if (!type)
          type = defaultType(operand);
        FailureOr<mlir::Value> value = emit(operand, type);
        if (failed(value))
          return failure();
        Type given = value->getType();
        if (isa<FloatType>(given) || given.isInteger(1))
          return error(operand.loc, "'hash_of' takes a whole number, an "
                                    "enum, an entity, a row number or a "
                                    "text");
        mlir::Value hashed = HashOp::create(builder, at, i64, *value);
        if (seeded)
          hashed = mix(arith::XOrIOp::create(builder, at, hashed, secret2),
                       arith::XOrIOp::create(builder, at, seeded, secret1));
        return fromBits(at, hashed,
                        builder.getIntegerType(64, /*isSigned=*/false));
      }
      FailureOr<mlir::Value> text = emit(operand, StringType::get(context));
      if (failed(text))
        return failure();
      // (Its bytes 16 at a time, two words of 8 mixed in, then its
      // length: devices/text.c, in one call.)
      mlir::Value seed =
          seeded ? mlir::Value(arith::XOrIOp::create(
                       builder, at, seeded, constant(0xbdd89aa982704029ull)))
                 : constant(0xbdd89aa982704029ull);
      mlir::Value hashed = TextHashOp::create(builder, at, i64, *text, seed);
      return fromBits(at, hashed,
                      builder.getIntegerType(64, /*isSigned=*/false));
    }
    // `fma(a, b, c)` of floats: a * b + c rounded once, as IEEE 754 says
    // (an instruction where the machine has one, else C's fma).
    if (expr.name == "fma") {
      if (expr.operands.size() != 3)
        return error(expr.loc, "'fma' takes three arguments");
      Type type = typeOf(expr);
      if (!type)
        type = expected && (expected.isF32() || expected.isF64())
                   ? expected
                   : defaultType(*expr.operands[0]);
      if (!type.isF32() && !type.isF64())
        return error(expr.loc, "'fma' takes an f32 or an f64");
      SmallVector<mlir::Value, 3> values;
      for (const ExprPtr &operand : expr.operands) {
        FailureOr<mlir::Value> value = emit(*operand, type);
        if (failed(value))
          return failure();
        if (value->getType() != type)
          return error(operand->loc, "'fma' takes three of the same float");
        values.push_back(*value);
      }
      return math::FmaOp::create(builder, at, values[0], values[1], values[2])
          .getResult();
    }
    // `sqrt(x)` of a float: rounded as IEEE 754 says, the same on every
    // machine and on the GPU (as `/` is). `bits_of(x)` the bits of a
    // float as a whole number without a sign of as many, and
    // `float_of(b)` the float of such bits: what math.ent builds on.
    if (expr.name == "sqrt" || expr.name == "bits_of" ||
        expr.name == "float_of") {
      if (expr.operands.size() != 1)
        return error(expr.loc, "'" + expr.name + "' takes one argument");
      bool toFloat = expr.name == "float_of";
      Type type = typeOf(*expr.operands[0]);
      // (Of literals: as many bits as the float the context expects, or
      // the type literals take.)
      if (!type && toFloat)
        type = builder.getIntegerType(
            expected && expected.isF32() ? 32 : 64, /*isSigned=*/false);
      else if (!type)
        type = expected && (expected.isF32() || expected.isF64())
                   ? expected
                   : defaultType(*expr.operands[0]);
      FailureOr<mlir::Value> value = emit(*expr.operands[0], type);
      if (failed(value))
        return failure();
      Type given = value->getType();
      if (toFloat) {
        if (!isUnsigned(given) || (given.getIntOrFloatBitWidth() != 32 &&
                                   given.getIntOrFloatBitWidth() != 64))
          return error(expr.loc, "'float_of' takes a u32 or a u64");
        Type to = given.getIntOrFloatBitWidth() == 64
                      ? Type(builder.getF64Type())
                      : Type(builder.getF32Type());
        return arith::BitcastOp::create(builder, at, to, bits(at, *value))
            .getResult();
      }
      if (!given.isF32() && !given.isF64())
        return error(expr.loc, "'" + expr.name + "' takes an f32 or an f64");
      if (expr.name == "sqrt")
        return math::SqrtOp::create(builder, at, *value).getResult();
      unsigned width = given.getIntOrFloatBitWidth();
      mlir::Value word = arith::BitcastOp::create(
          builder, at, builder.getIntegerType(width), *value);
      return fromBits(at, word, builder.getIntegerType(width, false));
    }
    if (expr.name != "min" && expr.name != "max")
      return error(expr.loc, "unknown function '" + expr.name +
                                 "'; declare it before with 'fn', or one "
                                 "implemented in C with 'extern fn' or "
                                 "'extern proc' (min, max, len, mul_wide, "
                                 "sqrt, fma, bits_of, float_of and hash_of "
                                 "are built in)");
    if (expr.operands.size() != 2)
      return error(expr.loc, "'" + expr.name + "' takes two arguments");
    Type type = typeOf(expr);
    if (!type)
      type = expected ? expected : defaultType(expr);
    FailureOr<mlir::Value> a = emit(*expr.operands[0], type);
    FailureOr<mlir::Value> b = emit(*expr.operands[1], type);
    if (failed(a) || failed(b))
      return failure();
    return combine(at, expr.name, *a, *b);
  }
  case Expr::Cast: {
    // `as text`: of any length; `as text[N]` of one of any length: cut.
    if (isa<StringType>(expr.castType) ||
        (isa<TextType>(expr.castType) && isAnyText(*expr.operands[0]))) {
      if (!isAnyText(*expr.operands[0]) && !textTypeOf(*expr.operands[0]))
        return error(expr.loc, "only a text can be cast to a text; a "
                               "number is put into one with \"{value}\"");
      FailureOr<mlir::Value> value =
          emit(*expr.operands[0], StringType::get(context));
      if (failed(value))
        return failure();
      if (auto text = dyn_cast<TextType>(expr.castType))
        return TextCutOp::create(builder, at, text, *value).getResult();
      return value;
    }
    if (auto text = dyn_cast<TextType>(expr.castType)) {
      // `as text[N]`: the same text, cut to N bytes if it is longer.
      Type operand = textTypeOf(*expr.operands[0]);
      if (!operand)
        return error(expr.loc, "only a text can be cast to a text; a "
                               "number is put into one with \"{value}\"");
      FailureOr<mlir::Value> value = emit(*expr.operands[0], operand);
      if (failed(value))
        return failure();
      return textResize(at, *value, text);
    }
    Type from = typeOf(*expr.operands[0]);
    // A row number is an i32: `c as i32`, `n as index of Arena`, and of
    // one buffer as one of another (`l as index of Lists`).
    auto toIndex = dyn_cast<IndexOfType>(expr.castType);
    auto fromIndex = dyn_cast_or_null<IndexOfType>(from);
    if (toIndex || fromIndex) {
      if (from == expr.castType)
        return emit(*expr.operands[0], from);
      Type i32 = builder.getI32Type();
      // (Cast in two steps, by way of the i32: the one between numbers
      // is the cast of a value that is there already.)
      auto castOf = [&](mlir::Value value, Type to) {
        Expr given{Expr::Given, expr.loc};
        given.given = value;
        Expr cast{Expr::Cast, expr.loc};
        cast.castType = to;
        cast.operands.push_back(std::make_unique<Expr>(std::move(given)));
        return emitRaw(cast, to);
      };
      mlir::Value number;
      if (fromIndex) {
        FailureOr<mlir::Value> value = emit(*expr.operands[0], from);
        if (failed(value))
          return failure();
        number = indexNumber(at, *value);
      } else {
        if (!from)
          from = defaultType(*expr.operands[0]);
        if (!isWhole(from))
          return error(expr.loc, "a row number is made of a whole number");
        FailureOr<mlir::Value> value = emit(*expr.operands[0], from);
        if (failed(value))
          return failure();
        if (from != i32) {
          FailureOr<mlir::Value> cast = castOf(*value, i32);
          if (failed(cast))
            return failure();
          *value = *cast;
        }
        number = *value;
      }
      if (toIndex)
        return asIndex(at, number, toIndex);
      if (expr.castType == i32)
        return number;
      if (!isWhole(expr.castType))
        return error(expr.loc, "a row number is cast to a whole number");
      return castOf(number, expr.castType);
    }
    // An enum is the number of its case: `way as i32`, `2 as Way`.
    auto toEnum = dyn_cast<EnumType>(expr.castType);
    auto fromEnum = dyn_cast_or_null<EnumType>(from);
    if (toEnum || fromEnum) {
      if (!from)
        from = defaultType(*expr.operands[0]);
      FailureOr<mlir::Value> value = emit(*expr.operands[0], from);
      if (failed(value))
        return failure();
      if (from == expr.castType)
        return *value;
      Type other = toEnum ? from : expr.castType;
      if (!isWhole(other))
        return error(expr.loc, "an enum is cast to and from an integer, the "
                               "number of its case");
      Type byte = builder.getIntegerType(8);
      unsigned width = other.getIntOrFloatBitWidth();
      if (fromEnum) {
        mlir::Value number =
            UnrealizedConversionCastOp::create(builder, at, byte, *value)
                .getResult(0);
        return fromBits(at,
                        width == 8 ? number
                                   : arith::ExtUIOp::create(
                                         builder, at, bitsType(other), number)
                                         .getResult(),
                        other);
      }
      mlir::Value number =
          width == 8 ? bits(at, *value)
                     : arith::TruncIOp::create(builder, at, byte,
                                               bits(at, *value))
                           .getResult();
      return UnrealizedConversionCastOp::create(builder, at, expr.castType,
                                                number)
          .getResult(0);
    }
    // A literal is written in the type it is cast to (`5000000000 as i64`).
    if (!from && (isa<FloatType>(expr.castType) ||
                  (isWhole(expr.castType) &&
                   !defaultType(*expr.operands[0]).isF32())))
      from = expr.castType;
    if (!from)
      from = defaultType(*expr.operands[0]);
    FailureOr<mlir::Value> value = emit(*expr.operands[0], from);
    if (failed(value))
      return failure();
    Type to = expr.castType;
    if (from == to)
      return *value;
    bool fromFloat = isa<FloatType>(from), toFloat = isa<FloatType>(to);
    if (fromFloat && toFloat)
      return from.getIntOrFloatBitWidth() < to.getIntOrFloatBitWidth()
                 ? arith::ExtFOp::create(builder, at, to, *value).getResult()
                 : arith::TruncFOp::create(builder, at, to, *value)
                       .getResult();
    if (fromFloat && isUnsigned(to))
      return fromBits(
          at,
          arith::FPToUIOp::create(builder, at, bitsType(to), *value)
              .getResult(),
          to);
    if (isUnsigned(from) && toFloat)
      return arith::UIToFPOp::create(builder, at, to, bits(at, *value))
          .getResult();
    if (fromFloat && to.isSignlessInteger())
      return arith::FPToSIOp::create(builder, at, to, *value).getResult();
    if (from.isSignlessInteger() && toFloat)
      return arith::SIToFPOp::create(builder, at, to, *value).getResult();
    // A bool as a number: 1 for true, 0 for false.
    if (from.isInteger(1) && isWhole(to))
      return fromBits(at,
                      arith::ExtUIOp::create(builder, at, bitsType(to), *value)
                          .getResult(),
                      to);
    if (from.isInteger(1) || to.isInteger(1))
      return error(expr.loc, "a bool is cast to a whole number (1 or 0); a "
                             "number is made a bool by comparing it");
    if (isWhole(from) && isWhole(to)) {
      // More bits: those of the sign where the value has one, noughts
      // where it has none. Fewer: the low ones. As many: the same bits.
      unsigned had = from.getIntOrFloatBitWidth();
      unsigned wanted = to.getIntOrFloatBitWidth();
      mlir::Value held = bits(at, *value);
      Type target = bitsType(to);
      if (had < wanted)
        held = isUnsigned(from)
                   ? arith::ExtUIOp::create(builder, at, target, held)
                         .getResult()
                   : arith::ExtSIOp::create(builder, at, target, held)
                         .getResult();
      else if (had > wanted)
        held = arith::TruncIOp::create(builder, at, target, held).getResult();
      return fromBits(at, held, to);
    }
    if (isUnsigned(from) || isUnsigned(to))
      return error(expr.loc, "cannot cast between these types");
    if (from.isIntOrIndex() && to.isIntOrIndex())
      return arith::IndexCastOp::create(builder, at, to, *value).getResult();
    return error(expr.loc, "cannot cast between these types");
  }
  case Expr::If:
    return emitIf(expr, expected);
  case Expr::Spawn:
    // (A proc spawns what it was given to change: checked where the
    // entity's components are known, in emitSpawn.)
    if (inFunction && (!inProc || givenComponents.empty()))
      return error(expr.loc, "a fn only computes; a system spawns, and a "
                             "proc that was given the components to "
                             "change ('name: mut Component')");
    return emitSpawn(expr);
  case Expr::Given:
    return expr.given;
  case Expr::Const: {
    Constant &constant = constants.ofSymbol(expr.name);
    return emit(*constant.value, constant.type);
  }
  case Expr::Of: {
    Type held = typeOf(*expr.operands[0]);
    if (!rowOf(held))
      return error(expr.loc, "only a row of a table has fields to read "
                             "with '.'");
    FailureOr<mlir::Value> row = emit(*expr.operands[0], held);
    if (failed(row))
      return failure();
    return readRowValue(expr.loc, *row, expr.field);
  }
  case Expr::Tuple:
    return error(expr.loc, "'(a, b)' is several values, which a fn gives "
                           "back and 'let (a, b) = ...' takes apart; one "
                           "value is expected here");
  case Expr::Has: {
    const Variable *variable = lookup(expr.name);
    if (!variable || variable->kind != Variable::Entity)
      return error(expr.loc, "'has' tests the entity a 'for' visits; '" +
                                 expr.name + "' is not that entity");
    return HasOp::create(builder, at, builder.getI1Type(), symbol(expr.field))
        .getResult();
  }
  case Expr::InSet: {
    if (inFunction)
      return error(expr.loc, "'" + expr.name +
                                 "' is a set, which a system "
                                 "reads; a fn or proc does not");
    FailureOr<mlir::Value> entity =
        emit(*expr.operands[0], EntityType::get(context));
    if (failed(entity))
      return failure();
    if (!isa<EntityType>(entity->getType()))
      return mismatch(expr.operands[0]->loc, entity->getType(),
                      EntityType::get(context));
    return SetHasOp::create(builder, at, builder.getI1Type(), symbol(expr.name),
                            *entity)
        .getResult();
  }
  }
  return error(expr.loc, "unsupported expression");
}

FailureOr<mlir::Value> Parser::arithmetic(Location at, Token::Kind op,
                                          mlir::Value a, mlir::Value b) {
  // A row number with an i32: a row number of the same buffer. Two of the
  // same buffer: how many rows apart (`-`).
  auto ia = dyn_cast<IndexOfType>(a.getType());
  auto ib = dyn_cast<IndexOfType>(b.getType());
  if (ia || ib) {
    if (ia && ib) {
      if (ia != ib)
        return emitError(at, "these are row numbers of different buffers"),
               failure();
      if (op != Token::Minus)
        return emitError(at, "two row numbers are taken one from the other "
                             "('-'), which gives how many rows apart they "
                             "are; nothing else"),
               failure();
      return arithmetic(at, op, indexNumber(at, a), indexNumber(at, b));
    }
    mlir::Value other = ia ? b : a;
    if (!other.getType().isInteger(32))
      return emitError(at, "a row number is computed with an i32"),
             failure();
    FailureOr<mlir::Value> number =
        arithmetic(at, op, ia ? indexNumber(at, a) : a,
                   ib ? indexNumber(at, b) : b);
    if (failed(number))
      return failure();
    return asIndex(at, *number, ia ? ia : ib);
  }
  Type type = a.getType();
  bool isFloat = isa<FloatType>(type);
  if (!isFloat && (!type.isIntOrIndex() || type.isInteger(1)))
    return emitError(at, "arithmetic needs numbers"), failure();
  // One without a sign: computed with as its bits, by the operations that
  // take them so.
  bool noSign = isUnsigned(type);
  a = bits(at, a);
  b = bits(at, b);
  auto whole = [&](StringRef what) -> LogicalResult {
    if (!isFloat)
      return success();
    return emitError(at, "'" + what + "' is of whole numbers: a float has "
                         "no bits to say"),
           failure();
  };
  mlir::Value result;
  switch (op) {
  case Token::Plus:
    result = isFloat ? arith::AddFOp::create(builder, at, a, b).getResult()
                     : arith::AddIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Minus:
    result = isFloat ? arith::SubFOp::create(builder, at, a, b).getResult()
                     : arith::SubIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Star:
    result = isFloat ? arith::MulFOp::create(builder, at, a, b).getResult()
                     : arith::MulIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Slash:
    result = isFloat   ? arith::DivFOp::create(builder, at, a, b).getResult()
             : noSign ? arith::DivUIOp::create(builder, at, a, b).getResult()
                      : arith::DivSIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Percent:
    result = isFloat   ? arith::RemFOp::create(builder, at, a, b).getResult()
             : noSign ? arith::RemUIOp::create(builder, at, a, b).getResult()
                      : arith::RemSIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Amp:
    if (failed(whole("&")))
      return failure();
    result = arith::AndIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Pipe:
    if (failed(whole("|")))
      return failure();
    result = arith::OrIOp::create(builder, at, a, b).getResult();
    break;
  case Token::Caret:
    if (failed(whole("^")))
      return failure();
    result = arith::XOrIOp::create(builder, at, a, b).getResult();
    break;
  case Token::ShiftLeft:
  case Token::ShiftRight: {
    if (failed(whole(op == Token::ShiftLeft ? "<<" : ">>")))
      return failure();
    // By the amount's low bits, as many as say a place in the number: a
    // shift by its width or more is one by what is left of that.
    unsigned width = type.isIndex() ? 64 : type.getIntOrFloatBitWidth();
    mlir::Value amount = arith::AndIOp::create(
        builder, at, b,
        arith::ConstantOp::create(
            builder, at,
            IntegerAttr::get(a.getType(),
                             APInt(type.isIndex() ? 64 : width, width - 1))));
    // To the right, the bits that come in are the sign's, or noughts for
    // a number that has none.
    result = op == Token::ShiftLeft
                 ? arith::ShLIOp::create(builder, at, a, amount).getResult()
             : noSign
                 ? arith::ShRUIOp::create(builder, at, a, amount).getResult()
                 : arith::ShRSIOp::create(builder, at, a, amount).getResult();
    break;
  }
  default:
    return emitError(at, "not an arithmetic operator"), failure();
  }
  return fromBits(at, result, type);
}

mlir::Value Parser::combine(Location at, StringRef rule, mlir::Value a,
                            mlir::Value b) {
  Type type = a.getType();
  bool isFloat = isa<FloatType>(type);
  bool noSign = isUnsigned(type);
  a = bits(at, a);
  b = bits(at, b);
  mlir::Value result;
  if (rule == "min")
    result = isFloat  ? arith::MinimumFOp::create(builder, at, a, b).getResult()
             : noSign ? arith::MinUIOp::create(builder, at, a, b).getResult()
                      : arith::MinSIOp::create(builder, at, a, b).getResult();
  else
    result = isFloat  ? arith::MaximumFOp::create(builder, at, a, b).getResult()
             : noSign ? arith::MaxUIOp::create(builder, at, a, b).getResult()
                      : arith::MaxSIOp::create(builder, at, a, b).getResult();
  return fromBits(at, result, type);
}

/// Whether `expr` is worked out with a few instructions that cannot fail:
/// numbers, names, fields of what is bound and of uniques, and what is
/// made of them without a call, an index, a lookup or a division. The
/// right side of `&&` and `||` is worked out in any case then, which costs
/// less than a branch; else only where it counts.
static bool cheapAndSafe(const Expr &expr) {
  switch (expr.kind) {
  case Expr::Int:
  case Expr::Float:
  case Expr::Bool:
  case Expr::Name:
  case Expr::Const:
  case Expr::Given:
    return true;
  case Expr::Field:
    return expr.operands.empty();
  case Expr::Unary:
  case Expr::Cast:
    return cheapAndSafe(*expr.operands[0]);
  case Expr::Binary:
    if (expr.op == Token::Slash || expr.op == Token::Percent)
      return false;
    return llvm::all_of(expr.operands, [](const ExprPtr &operand) {
      return cheapAndSafe(*operand);
    });
  default:
    return false;
  }
}

FailureOr<mlir::Value> Parser::emitBinary(const Expr &expr, Type expected) {
  Location at = loc(expr.loc);
  const Expr &lhs = *expr.operands[0], &rhs = *expr.operands[1];
  if (expr.op == Token::AndAnd || expr.op == Token::OrOr) {
    bool isAnd = expr.op == Token::AndAnd;
    FailureOr<mlir::Value> a = emit(lhs, builder.getI1Type());
    if (failed(a))
      return failure();
    if (!a->getType().isInteger(1))
      return error(expr.loc, "'&&' and '||' need bools");
    if (cheapAndSafe(rhs)) {
      FailureOr<mlir::Value> b = emit(rhs, builder.getI1Type());
      if (failed(b))
        return failure();
      if (!b->getType().isInteger(1))
        return error(expr.loc, "'&&' and '||' need bools");
      return isAnd ? arith::AndIOp::create(builder, at, *a, *b).getResult()
                   : arith::OrIOp::create(builder, at, *a, *b).getResult();
    }
    // The right side only where the left does not decide: `a && b` is
    // `if a { b } else { false }`, `a || b` is `if a { true } else { b }`.
    auto branch = scf::IfOp::create(builder, at, TypeRange{builder.getI1Type()},
                                    *a, /*withElseRegion=*/true);
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(isAnd ? branch.thenBlock()
                                             : branch.elseBlock());
      ScopeGuard scope(*this);
      FailureOr<mlir::Value> b = emit(rhs, builder.getI1Type());
      if (failed(b))
        return failure();
      if (!b->getType().isInteger(1))
        return error(expr.loc, "'&&' and '||' need bools");
      scf::YieldOp::create(builder, at, ValueRange{*b});
      builder.setInsertionPointToStart(isAnd ? branch.elseBlock()
                                             : branch.thenBlock());
      scf::YieldOp::create(
          builder, at,
          ValueRange{arith::ConstantIntOp::create(builder, at, !isAnd, 1)
                         .getResult()});
    }
    return branch.getResult(0);
  }
  bool comparison = precedenceOf(expr.op) == 3 || precedenceOf(expr.op) == 4;
  // A row number of a buffer: with an i32, or with one of the same buffer.
  {
    auto li = dyn_cast_or_null<IndexOfType>(typeOf(lhs));
    auto ri = dyn_cast_or_null<IndexOfType>(typeOf(rhs));
    if (li || ri) {
      // (A literal on the other side is an i32; `none`, no row.)
      auto side = [&](const Expr &e) {
        Type its = typeOf(e);
        if (!its && e.kind == Expr::Name && e.name == "none" &&
            !lookup(e.name))
          its = li ? li : ri;
        return emit(e, its ? its : Type(builder.getI32Type()));
      };
      FailureOr<mlir::Value> a = side(lhs);
      if (failed(a))
        return failure();
      FailureOr<mlir::Value> b = side(rhs);
      if (failed(b))
        return failure();
      if (!comparison)
        return arithmetic(at, expr.op, *a, *b);
      auto ia = dyn_cast<IndexOfType>(a->getType());
      auto ib = dyn_cast<IndexOfType>(b->getType());
      if (ia && ib && ia != ib)
        return error(expr.loc, "these are row numbers of different buffers");
      mlir::Value x = ia ? indexNumber(at, *a) : *a;
      mlir::Value y = ib ? indexNumber(at, *b) : *b;
      if (!x.getType().isInteger(32) || !y.getType().isInteger(32))
        return error(expr.loc, "a row number is compared with a row number "
                               "of its buffer, or with an i32");
      arith::CmpIPredicate predicate;
      switch (expr.op) {
      case Token::Equal:
        predicate = arith::CmpIPredicate::eq;
        break;
      case Token::NotEqual:
        predicate = arith::CmpIPredicate::ne;
        break;
      case Token::Less:
        predicate = arith::CmpIPredicate::slt;
        break;
      case Token::LessEqual:
        predicate = arith::CmpIPredicate::sle;
        break;
      case Token::Greater:
        predicate = arith::CmpIPredicate::sgt;
        break;
      default:
        predicate = arith::CmpIPredicate::sge;
        break;
      }
      return arith::CmpIOp::create(builder, at, predicate, x, y).getResult();
    }
  }
  Type leftText = textTypeOf(lhs), rightText = textTypeOf(rhs);
  if (isAnyText(lhs) || isAnyText(rhs)) {
    // One of any length: compared byte for byte, with one of either kind.
    if (!(leftText || isAnyText(lhs)) || !(rightText || isAnyText(rhs)))
      return error(expr.loc, "a text is compared to texts; a number is put "
                             "into one with \"{value}\"");
    if (expr.op != Token::Equal && expr.op != Token::NotEqual &&
        expr.op != Token::Plus)
      return error(expr.loc, "texts can be joined ('+') and compared with "
                             "'==' and '!='");
    Type any = StringType::get(context);
    FailureOr<mlir::Value> a = emit(lhs, any);
    FailureOr<mlir::Value> b = emit(rhs, any);
    if (failed(a) || failed(b))
      return failure();
    // Joined: a text that is kept while the schedule runs.
    if (expr.op == Token::Plus)
      return TextJoinOp::create(builder, at, any, *a, *b).getResult();
    mlir::Value same =
        TextEqualOp::create(builder, at, builder.getI1Type(), *a, *b)
            .getResult();
    if (expr.op == Token::Equal)
      return same;
    return arith::XOrIOp::create(
               builder, at, same,
               arith::ConstantIntOp::create(builder, at, 1, 1))
        .getResult();
  }
  if (leftText || rightText) {
    if (!leftText || !rightText)
      return error(expr.loc, "a text is joined with and compared to texts; "
                             "a number is put into one with \"{value}\"");
    FailureOr<mlir::Value> a = emit(lhs, leftText);
    FailureOr<mlir::Value> b = emit(rhs, rightText);
    if (failed(a) || failed(b))
      return failure();
    if (expr.op == Token::Plus)
      return textConcat(expr.loc, *a, *b);
    if (expr.op != Token::Equal && expr.op != Token::NotEqual)
      return error(expr.loc, "texts can be joined ('+') and compared with "
                             "'==' and '!='");
    // Bytes past the length are zero, so equal texts are equal integers,
    // whatever their capacities.
    mlir::Value x = textBits(at, *a), y = textBits(at, *b);
    unsigned width = std::max(x.getType().getIntOrFloatBitWidth(),
                              y.getType().getIntOrFloatBitWidth());
    Type wide = builder.getIntegerType(width);
    if (x.getType() != wide)
      x = arith::ExtUIOp::create(builder, at, wide, x);
    if (y.getType() != wide)
      y = arith::ExtUIOp::create(builder, at, wide, y);
    return arith::CmpIOp::create(builder, at,
                                 expr.op == Token::Equal
                                     ? arith::CmpIPredicate::eq
                                     : arith::CmpIPredicate::ne,
                                 x, y)
        .getResult();
  }
  if (expr.op == Token::ShiftLeft || expr.op == Token::ShiftRight) {
    // What is shifted says the type; by how much is any whole number, of
    // which the low bits count.
    Type type = typeOf(lhs);
    if (!type)
      type = expected ? expected : defaultType(lhs);
    FailureOr<mlir::Value> a = emit(lhs, type);
    if (failed(a))
      return failure();
    if (!isWhole(a->getType()))
      return error(expr.loc, "'<<' and '>>' shift the bits of a whole "
                             "number");
    Type amountType = typeOf(rhs);
    if (!amountType)
      amountType = a->getType();
    FailureOr<mlir::Value> b = emit(rhs, amountType);
    if (failed(b))
      return failure();
    if (!isWhole(b->getType()))
      return error(rhs.loc, "a shift is by a whole number of bits");
    unsigned had = b->getType().getIntOrFloatBitWidth();
    unsigned wanted = a->getType().getIntOrFloatBitWidth();
    mlir::Value amount = bits(at, *b);
    Type held = bitsType(a->getType());
    if (had < wanted)
      amount = arith::ExtUIOp::create(builder, at, held, amount);
    else if (had > wanted)
      amount = arith::TruncIOp::create(builder, at, held, amount);
    return arithmetic(at, expr.op, *a, fromBits(at, amount, a->getType()));
  }
  Type type = typeOf(lhs);
  if (!type)
    type = typeOf(rhs);
  // (Whole numbers of different sizes: both as the one of more bits.)
  else if (Type other = typeOf(rhs))
    if (Type wider = widerInteger(type, other))
      type = wider;
  if (!type)
    type = comparison || !expected ? defaultType(expr) : expected;
  FailureOr<mlir::Value> a = emit(lhs, type);
  FailureOr<mlir::Value> b = emit(rhs, type);
  if (failed(a) || failed(b))
    return failure();
  if (a->getType() != b->getType())
    return error(expr.loc,
                 isWhole(a->getType()) && isWhole(b->getType()) &&
                         isUnsigned(a->getType()) != isUnsigned(b->getType())
                     ? "one of these has a sign and the other has none: "
                       "say which it is to be, with 'as'"
                     : "operands have different types");
  if (auto named = dyn_cast<EnumType>(a->getType())) {
    if (expr.op != Token::Equal && expr.op != Token::NotEqual)
      return error(expr.loc, "enum values are compared with '==' and '!='; "
                             "'as i32' gives the number of a case");
    Type byte = named.getStorageType();
    return arith::CmpIOp::create(
               builder, at,
               expr.op == Token::Equal ? arith::CmpIPredicate::eq
                                       : arith::CmpIPredicate::ne,
               UnrealizedConversionCastOp::create(builder, at, byte, *a)
                   .getResult(0),
               UnrealizedConversionCastOp::create(builder, at, byte, *b)
                   .getResult(0))
        .getResult();
  }
  if (!comparison)
    return arithmetic(at, expr.op, *a, *b);
  if (isa<FloatType>(type)) {
    // (As IEEE 754 says: not a number is unequal to everything, itself
    // too, and neither below nor above anything.)
    arith::CmpFPredicate predicate;
    switch (expr.op) {
    case Token::Equal:
      predicate = arith::CmpFPredicate::OEQ;
      break;
    case Token::NotEqual:
      predicate = arith::CmpFPredicate::UNE;
      break;
    case Token::Less:
      predicate = arith::CmpFPredicate::OLT;
      break;
    case Token::LessEqual:
      predicate = arith::CmpFPredicate::OLE;
      break;
    case Token::Greater:
      predicate = arith::CmpFPredicate::OGT;
      break;
    default:
      predicate = arith::CmpFPredicate::OGE;
      break;
    }
    return arith::CmpFOp::create(builder, at, predicate, *a, *b).getResult();
  }
  bool noSign = isUnsigned(a->getType());
  arith::CmpIPredicate predicate;
  switch (expr.op) {
  case Token::Equal:
    predicate = arith::CmpIPredicate::eq;
    break;
  case Token::NotEqual:
    predicate = arith::CmpIPredicate::ne;
    break;
  case Token::Less:
    predicate = noSign ? arith::CmpIPredicate::ult : arith::CmpIPredicate::slt;
    break;
  case Token::LessEqual:
    predicate = noSign ? arith::CmpIPredicate::ule : arith::CmpIPredicate::sle;
    break;
  case Token::Greater:
    predicate = noSign ? arith::CmpIPredicate::ugt : arith::CmpIPredicate::sgt;
    break;
  default:
    predicate = noSign ? arith::CmpIPredicate::uge : arith::CmpIPredicate::sge;
    break;
  }
  // Two entities: the same one or not.
  if (isa<EntityType>(a->getType())) {
    if (expr.op != Token::Equal && expr.op != Token::NotEqual)
      return error(expr.loc, "entities are compared with '==' and '!='");
    mlir::Value same =
        SameOp::create(builder, at, builder.getI1Type(), *a, *b).getResult();
    if (expr.op == Token::Equal)
      return same;
    return arith::XOrIOp::create(builder, at, same,
                                 integer(at, builder.getI1Type(), 1))
        .getResult();
  }
  return arith::CmpIOp::create(builder, at, predicate, bits(at, *a),
                               bits(at, *b))
      .getResult();
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

// One escape after its backslash: n t r 0 \ " ' { } or xHH.
FailureOr<char> Parser::parseEscape(StringRef &rest, llvm::SMLoc at) {
  if (rest.empty())
    return error(at, "a backslash needs a character after it");
  char c = rest.front();
  rest = rest.drop_front();
  switch (c) {
  case 'n':
    return '\n';
  case 't':
    return '\t';
  case 'r':
    return '\r';
  case '0':
    return '\0';
  case '\\':
  case '"':
  case '\'':
  case '{':
  case '}':
    return c;
  case 'x': {
    unsigned value;
    if (rest.size() < 2 || rest.take_front(2).getAsInteger(16, value))
      return error(at, "'\\x' needs two hex digits");
    rest = rest.drop_front(2);
    return static_cast<char>(value);
  }
  default:
    return error(at, "unknown escape '\\" + Twine(c) +
                         "'; there are \\n \\t \\r \\0 \\\\ \\\" \\' \\{ \\} "
                         "and \\xHH");
  }
}

// "text {value} more": the pieces joined; a value in braces is put in as
// text ("{x}" for a number, a bool or a text).
FailureOr<ExprPtr> Parser::parseString() {
  llvm::SMLoc at = token.loc;
  StringRef rest = token.spelling.drop_front().drop_back();
  SmallVector<ExprPtr> parts;
  std::string bytes;
  auto flush = [&](bool always) {
    if (bytes.empty() && !always)
      return;
    auto node = std::make_unique<Expr>();
    node->kind = Expr::String;
    node->loc = at;
    node->name = std::move(bytes);
    bytes.clear();
    parts.push_back(std::move(node));
  };
  while (!rest.empty()) {
    char c = rest.front();
    llvm::SMLoc here = llvm::SMLoc::getFromPointer(rest.data());
    rest = rest.drop_front();
    if (c == '\\') {
      FailureOr<char> escaped = parseEscape(rest, here);
      if (failed(escaped))
        return failure();
      bytes += *escaped;
    } else if (c == '}') {
      return error(here, "'}' without a '{' before it; write '\\}' for the "
                         "character");
    } else if (c == '{') {
      size_t close = rest.find('}');
      if (close == StringRef::npos)
        return error(here, "'{' starts a value that '}' ends; write '\\{' "
                           "for the character");
      // The expression between the braces, parsed where it stands.
      Lexer savedLexer = lexer;
      Token savedToken = token;
      const char *savedEnd = previousEnd;
      lexer = Lexer(rest.take_front(close));
      advance();
      FailureOr<ExprPtr> value = parseExpr();
      bool whole = token.is(Token::Eof);
      lexer = savedLexer;
      token = savedToken;
      previousEnd = savedEnd;
      if (failed(value))
        return failure();
      if (!whole)
        return error(here, "expected one expression between '{' and '}'");
      rest = rest.drop_front(close + 1);
      flush(/*always=*/false);
      auto node = std::make_unique<Expr>();
      node->kind = Expr::Format;
      node->loc = here;
      node->operands.push_back(std::move(*value));
      parts.push_back(std::move(node));
    } else {
      bytes += c;
    }
  }
  flush(/*always=*/parts.empty());
  advance();
  ExprPtr joined = std::move(parts.front());
  for (ExprPtr &part : llvm::drop_begin(parts)) {
    auto node = std::make_unique<Expr>();
    node->kind = Expr::Binary;
    node->loc = at;
    node->op = Token::Plus;
    node->operands.push_back(std::move(joined));
    node->operands.push_back(std::move(part));
    joined = std::move(node);
  }
  return joined;
}

/// Where `component`, which is added or removed, has a text of any
/// length: no `let` keeps one, which may be what the entity's field held.
LogicalResult Parser::keepsNoText(llvm::SMLoc at, StringRef component) {
  bool holds = llvm::any_of(components[component].fields, [](auto &field) {
    return isa<StringType>(field.second);
  });
  if (!holds)
    return success();
  for (auto &scope : scopes)
    for (auto &entry : scope)
      if (entry.second.kind == Variable::Value && entry.second.value &&
          isa<StringType>(entry.second.value.getType()))
        return error(at, "'" + entry.first() + "' keeps a text of any "
                         "length, and '" + component + "' has one that goes "
                         "here: keep a copy of a capacity ('as text[N]')");
  return success();
}

/// Whether `expr` is a text of any length.
bool Parser::isAnyText(const Expr &expr) {
  if (expr.kind == Expr::String)
    return false;
  Type type = typeOf(expr);
  return type && isa<StringType>(type);
}

/// Whether the text of any length that `expr` gives may be what a field
/// holds that can be assigned where `expr` is: a unique's, or that of a
/// `mut` binding.
bool Parser::mayBeAssigned(const Expr &expr) {
  switch (expr.kind) {
  case Expr::Field:
  case Expr::Name:
    if (const Variable *variable = lookup(expr.name))
      return variable->kind == Variable::Ref && variable->mut;
    return uniques.count(expr.name);
  case Expr::If:
    return (expr.thenBranch && mayBeAssigned(*expr.thenBranch->value)) ||
           (expr.elseBranch && mayBeAssigned(*expr.elseBranch->value));
  default:
    return llvm::any_of(expr.operands, [&](const ExprPtr &operand) {
      return mayBeAssigned(*operand);
    });
  }
}

/// The text type of `expr` if it is a text: for a literal the one that
/// just holds it.
Type Parser::textTypeOf(const Expr &expr) {
  if (expr.kind == Expr::String)
    return TextType::get(
        context, std::clamp<unsigned>(expr.name.size(), 1,
                                      TextType::kMaxCapacity));
  Type type = typeOf(expr);
  return type && isa<TextType>(type) ? type : Type();
}

mlir::Value Parser::integer(Location at, Type type, uint64_t value) {
  Type held = bitsType(type);
  unsigned width = held.isIndex() ? 64 : held.getIntOrFloatBitWidth();
  return fromBits(at,
                  arith::ConstantOp::create(
                      builder, at,
                      IntegerAttr::get(held, APInt(width, value,
                                                   /*isSigned=*/false,
                                                   /*implicitTrunc=*/true))),
                  type);
}

mlir::Value Parser::bits(Location at, mlir::Value value) {
  if (!isUnsigned(value.getType()))
    return value;
  return UnrealizedConversionCastOp::create(
             builder, at, bitsType(value.getType()), value)
      .getResult(0);
}

/// A row number as the i32 it is computed with, and back.
mlir::Value Parser::indexNumber(Location at, mlir::Value index) {
  return UnrealizedConversionCastOp::create(builder, at, builder.getI32Type(),
                                            index)
      .getResult(0);
}
mlir::Value Parser::asIndex(Location at, mlir::Value number,
                            IndexOfType type) {
  return UnrealizedConversionCastOp::create(builder, at, type, number)
      .getResult(0);
}

/// The number of a row of the buffer `buffer` (`name` as the program
/// says it): a row number of that buffer, or an integer.
FailureOr<mlir::Value> Parser::emitRowNumber(const Expr &number,
                                             FlatSymbolRefAttr buffer,
                                             StringRef name) {
  Location at = loc(number.loc);
  Type indexType = typeOf(number);
  if (auto index = dyn_cast_or_null<IndexOfType>(indexType)) {
    if (index.getBuffer() != buffer)
      return error(number.loc,
                   "this is a row number of '" +
                       index.getBuffer().getValue() + "', not of '" + name +
                       "': 'as index of " + name + "' makes it one");
    return emit(number, index);
  }
  FailureOr<mlir::Value> value =
      emit(number, indexType ? indexType : builder.getI32Type());
  if (failed(value))
    return failure();
  *value = countable(at, *value);
  if (!value->getType().isSignlessInteger() || value->getType().isInteger(1))
    return error(number.loc, "a row's number is an integer");
  return value;
}

FailureOr<mlir::Value> Parser::windowRow(const Variable &window,
                                         const Expr &number) {
  Location at = loc(number.loc);
  FailureOr<mlir::Value> row = emit(number, builder.getI32Type());
  if (failed(row))
    return failure();
  if (!row->getType().isInteger(32))
    return error(number.loc, "a row of a window is an i32, from 0 to below "
                             "its length");
  // (The number a loop counts from 0 or more up to below the window's
  // length, `for k in 0..len(w)`: in it, as it is.)
  if (auto counted = row->getDefiningOp<arith::IndexCastOp>()) {
    auto argument = dyn_cast<BlockArgument>(counted.getIn());
    auto loop = argument ? dyn_cast<scf::ForOp>(
                               argument.getOwner()->getParentOp())
                         : scf::ForOp();
    if (loop && loop.getInductionVar() == argument) {
      auto upper = loop.getUpperBound().getDefiningOp<arith::IndexCastOp>();
      auto constant = [](mlir::Value value) {
        if (auto cast = value.getDefiningOp<arith::IndexCastOp>())
          value = cast.getIn();
        return getConstantIntValue(value);
      };
      std::optional<int64_t> lower = constant(loop.getLowerBound());
      std::optional<int64_t> step = constant(loop.getStep());
      if (upper && upper.getIn() == window.which && lower && *lower >= 0 &&
          step && *step > 0)
        return arith::AddIOp::create(builder, at, window.value, *row)
            .getResult();
    }
  }
  mlir::Value inside = arith::AndIOp::create(
      builder, at,
      arith::CmpIOp::create(builder, at, arith::CmpIPredicate::sge, *row,
                            integer(at, builder.getI32Type(), 0)),
      arith::CmpIOp::create(builder, at, arith::CmpIPredicate::slt, *row,
                            window.which));
  RowsCheckOp::create(builder, at, inside,
                      FlatSymbolRefAttr::get(context, window.component));
  return arith::SelectOp::create(
             builder, at, inside,
             arith::AddIOp::create(builder, at, window.value, *row),
             arith::ConstantIntOp::create(builder, at, -1, 32))
      .getResult();
}

mlir::Value Parser::countable(Location at, mlir::Value value) {
  if (!isUnsigned(value.getType()))
    return value;
  // (A u64 from 2^63 on is less than nought then: no row, as it should
  // be.)
  unsigned width = value.getType().getIntOrFloatBitWidth();
  mlir::Value held = bits(at, value);
  if (width == 64)
    return held;
  return arith::ExtUIOp::create(
             builder, at,
             width < 32 ? builder.getI32Type() : builder.getI64Type(), held)
      .getResult();
}

mlir::Value Parser::fromBits(Location at, mlir::Value bits, Type type) {
  if (bits.getType() == type)
    return bits;
  return UnrealizedConversionCastOp::create(builder, at, type, bits)
      .getResult(0);
}

mlir::Value Parser::textBits(Location at, mlir::Value text) {
  return UnrealizedConversionCastOp::create(
             builder, at, cast<TextType>(text.getType()).getStorageType(),
             text)
      .getResult(0);
}

mlir::Value Parser::textFromBits(Location at, mlir::Value bits,
                                 TextType type) {
  return UnrealizedConversionCastOp::create(builder, at, type, bits)
      .getResult(0);
}

mlir::Value Parser::textConstant(Location at, StringRef bytes, TextType type) {
  IntegerType storage = type.getStorageType();
  APInt value(storage.getWidth(), bytes.size());
  for (auto [index, byte] : llvm::enumerate(bytes))
    value.insertBits(static_cast<unsigned char>(byte), 16 + 8 * index, 8);
  return textFromBits(
      at, arith::ConstantOp::create(builder, at, IntegerAttr::get(storage, value)),
      type);
}

/// `text` as a text of capacity `to`: the same bytes, or the first that
/// fit.
mlir::Value Parser::textResize(Location at, mlir::Value text, TextType to) {
  auto from = cast<TextType>(text.getType());
  if (from == to)
    return text;
  mlir::Value bits = textBits(at, text);
  IntegerType wide = from.getStorageType(), target = to.getStorageType();
  if (to.getCapacity() < from.getCapacity()) {
    // Keep the bytes that fit and the smaller of the two lengths.
    APInt keep = APInt::getLowBitsSet(wide.getWidth(),
                                      16 + 8 * to.getCapacity());
    keep.clearLowBits(16);
    mlir::Value content = arith::AndIOp::create(
        builder, at, bits,
        arith::ConstantOp::create(builder, at, IntegerAttr::get(wide, keep)));
    mlir::Value length = arith::AndIOp::create(builder, at, bits,
                                               integer(at, wide, 0xFFFF));
    length = arith::MinUIOp::create(builder, at, length,
                                    integer(at, wide, to.getCapacity()));
    bits = arith::OrIOp::create(builder, at, content, length);
  }
  if (target.getWidth() > wide.getWidth())
    bits = arith::ExtUIOp::create(builder, at, target, bits);
  else if (target.getWidth() < wide.getWidth())
    bits = arith::TruncIOp::create(builder, at, target, bits);
  return textFromBits(at, bits, to);
}

TextType Parser::textType(llvm::SMLoc at, unsigned capacity) {
  if (capacity > TextType::kMaxCapacity) {
    (void)error(at, "this text could have " + Twine(capacity) +
                        " bytes; a text holds at most " +
                        Twine(unsigned(TextType::kMaxCapacity)) +
                        " (cut a part with 'as text[N]')");
    capacity = TextType::kMaxCapacity;
  }
  return TextType::get(context, capacity);
}

/// `a` followed by `b`, in a text that holds both.
mlir::Value Parser::textConcat(llvm::SMLoc where, mlir::Value a,
                               mlir::Value b) {
  Location at = loc(where);
  TextType type =
      textType(where, cast<TextType>(a.getType()).getCapacity() +
                          cast<TextType>(b.getType()).getCapacity());
  IntegerType wide = type.getStorageType();
  auto widen = [&](mlir::Value text) {
    mlir::Value bits = textBits(at, text);
    if (bits.getType() != wide)
      bits = arith::ExtUIOp::create(builder, at, wide, bits);
    return bits;
  };
  mlir::Value x = widen(a), y = widen(b);
  mlir::Value mask = integer(at, wide, 0xFFFF);
  mlir::Value sixteen = integer(at, wide, 16);
  mlir::Value lengthA = arith::AndIOp::create(builder, at, x, mask);
  mlir::Value lengthB = arith::AndIOp::create(builder, at, y, mask);
  // b's bytes move behind a's; the lengths add up.
  mlir::Value bytesA = arith::SubIOp::create(builder, at, x, lengthA);
  mlir::Value bytesB = arith::ShRUIOp::create(builder, at, y, sixteen);
  mlir::Value shift = arith::AddIOp::create(
      builder, at, sixteen,
      arith::MulIOp::create(builder, at, lengthA, integer(at, wide, 8)));
  bytesB = arith::ShLIOp::create(builder, at, bytesB, shift);
  mlir::Value bits = arith::OrIOp::create(
      builder, at, arith::OrIOp::create(builder, at, bytesA, bytesB),
      arith::AddIOp::create(builder, at, lengthA, lengthB));
  return textFromBits(at, bits, type);
}

mlir::Value Parser::textLength(Location at, mlir::Value text) {
  mlir::Value length = arith::TruncIOp::create(
      builder, at, builder.getIntegerType(16), textBits(at, text));
  return arith::ExtUIOp::create(builder, at, builder.getI32Type(), length);
}

/// Byte `index` of `text`; 0 past its end.
mlir::Value Parser::textIndex(Location at, mlir::Value text,
                              mlir::Value index) {
  auto type = cast<TextType>(text.getType());
  IntegerType wide = type.getStorageType();
  mlir::Value bits = textBits(at, text);
  mlir::Value position =
      arith::ExtSIOp::create(builder, at, builder.getI64Type(), index);
  if (index.getType() == builder.getI64Type())
    position = index;
  // Bytes past the length are zero already; past the capacity (or below
  // zero) there is nothing to shift to.
  mlir::Value inside = arith::CmpIOp::create(
      builder, at, arith::CmpIPredicate::ult, position,
      integer(at, builder.getI64Type(), type.getCapacity()));
  position = arith::SelectOp::create(builder, at, inside, position,
                                     integer(at, builder.getI64Type(), 0));
  mlir::Value shift = arith::AddIOp::create(
      builder, at, integer(at, wide, 16),
      arith::MulIOp::create(
          builder, at, arith::ExtUIOp::create(builder, at, wide, position),
          integer(at, wide, 8)));
  mlir::Value byte = arith::TruncIOp::create(
      builder, at, builder.getIntegerType(8),
      arith::ShRUIOp::create(builder, at, bits, shift));
  return arith::SelectOp::create(builder, at, inside, byte,
                                 integer(at, builder.getIntegerType(8), 0));
}

/// The decimal digits of an unsigned 64-bit `magnitude`, as a text[20].
mlir::Value Parser::formatUnsigned(Location at, mlir::Value magnitude) {
  TextType type = TextType::get(context, 20);
  IntegerType wide = type.getStorageType();
  Type i64 = builder.getI64Type();
  // How many digits: one, and one more for every power of ten reached.
  mlir::Value count = integer(at, i64, 1);
  uint64_t power = 1;
  for (int k = 1; k < 20; ++k) {
    power *= 10;
    mlir::Value reached = arith::CmpIOp::create(
        builder, at, arith::CmpIPredicate::uge, magnitude,
        integer(at, i64, power));
    count = arith::AddIOp::create(
        builder, at, count, arith::ExtUIOp::create(builder, at, i64, reached));
  }
  // Digit k (from the right) goes to byte count - 1 - k.
  mlir::Value bits = arith::ExtUIOp::create(builder, at, wide, count);
  power = 1;
  for (int k = 0; k < 20; ++k) {
    mlir::Value digit = arith::RemUIOp::create(
        builder, at,
        arith::DivUIOp::create(builder, at, magnitude, integer(at, i64, power)),
        integer(at, i64, 10));
    mlir::Value character =
        arith::AddIOp::create(builder, at, digit, integer(at, i64, '0'));
    mlir::Value used = arith::CmpIOp::create(
        builder, at, arith::CmpIPredicate::ult, integer(at, i64, k), count);
    mlir::Value position = arith::SubIOp::create(
        builder, at, count, integer(at, i64, k + 1));
    position = arith::SelectOp::create(builder, at, used, position,
                                       integer(at, i64, 0));
    mlir::Value shift = arith::AddIOp::create(
        builder, at, integer(at, wide, 16),
        arith::MulIOp::create(
            builder, at, arith::ExtUIOp::create(builder, at, wide, position),
            integer(at, wide, 8)));
    mlir::Value placed = arith::ShLIOp::create(
        builder, at, arith::ExtUIOp::create(builder, at, wide, character),
        shift);
    placed = arith::SelectOp::create(builder, at, used, placed,
                                     integer(at, wide, 0));
    bits = arith::OrIOp::create(builder, at, bits, placed);
    power *= 10;
  }
  return textFromBits(at, bits, type);
}

/// `value` as text: a text as it is, a bool as true or false, an integer
/// in decimal, a float with up to six decimals.
FailureOr<mlir::Value> Parser::formatValue(llvm::SMLoc where,
                                           mlir::Value value) {
  Location at = loc(where);
  Type type = value.getType();
  Type i64 = builder.getI64Type();
  if (isa<TextType>(type))
    return value;
  // (A row number: its number.)
  if (isa<IndexOfType>(type))
    return formatValue(where, indexNumber(at, value));
  auto choose = [&](mlir::Value condition, mlir::Value a, mlir::Value b) {
    auto text = cast<TextType>(a.getType());
    return textFromBits(at,
                        arith::SelectOp::create(builder, at, condition,
                                                textBits(at, a),
                                                textBits(at, b)),
                        text);
  };
  if (rowOf(type) || rowsOf(type))
    return error(where, rowOf(type) ? "a row of a table is no text; its "
                                      "fields are ('{row.field}')"
                                    : "a table is no text; its rows' "
                                      "fields are");
  // An enum: the name of its case.
  if (auto named = dyn_cast<EnumType>(type)) {
    const SmallVector<std::string> &labels =
        enums.ofSymbol(named.getName().getValue());
    unsigned longest = 1;
    for (const std::string &label : labels)
      longest = std::max<unsigned>(longest, label.size());
    TextType text = TextType::get(context, longest);
    Type byte = named.getStorageType();
    mlir::Value number =
        UnrealizedConversionCastOp::create(builder, at, byte, value)
            .getResult(0);
    mlir::Value name = textConstant(at, labels.back(), text);
    for (unsigned i = labels.size() - 1; i-- > 0;)
      name = choose(arith::CmpIOp::create(builder, at,
                                          arith::CmpIPredicate::eq, number,
                                          integer(at, byte, i)),
                    textConstant(at, labels[i], text), name);
    return name;
  }
  // "-" or nothing.
  auto sign = [&](mlir::Value negative) {
    TextType one = TextType::get(context, 1);
    return choose(negative, textConstant(at, "-", one),
                  textConstant(at, "", one));
  };
  if (type.isInteger(1)) {
    TextType five = TextType::get(context, 5);
    return choose(value, textConstant(at, "true", five),
                  textConstant(at, "false", five));
  }
  if (isUnsigned(type)) {
    // Without a sign: its digits.
    mlir::Value wide = bits(at, value);
    if (wide.getType() != i64)
      wide = arith::ExtUIOp::create(builder, at, i64, wide);
    return formatUnsigned(at, wide);
  }
  if (type.isIntOrIndex()) {
    mlir::Value wide = value;
    if (type.isIndex())
      wide = arith::IndexCastOp::create(builder, at, i64, value);
    else if (type != i64)
      wide = arith::ExtSIOp::create(builder, at, i64, value);
    mlir::Value zero = integer(at, i64, 0);
    mlir::Value negative = arith::CmpIOp::create(
        builder, at, arith::CmpIPredicate::slt, wide, zero);
    mlir::Value magnitude = arith::SelectOp::create(
        builder, at, negative, arith::SubIOp::create(builder, at, zero, wide),
        wide);
    return textConcat(where, sign(negative), formatUnsigned(at, magnitude));
  }
  if (!isa<FloatType>(type))
    return error(where, "this value has no text form; numbers, bools and "
                        "texts do");

  // A float: its sign, the digits before the point and up to six after
  // it, rounded; without a fraction from 9e12 on; from 9e18 on "big", or
  // "inf" for infinity; and "nan".
  Type f64 = builder.getF64Type();
  mlir::Value x = value;
  if (type != f64)
    x = arith::ExtFOp::create(builder, at, f64, value);
  auto real = [&](double v) {
    return arith::ConstantOp::create(builder, at, builder.getF64FloatAttr(v))
        .getResult();
  };
  auto compare = [&](arith::CmpFPredicate predicate, mlir::Value a,
                     mlir::Value b) {
    return arith::CmpFOp::create(builder, at, predicate, a, b).getResult();
  };
  mlir::Value nan = compare(arith::CmpFPredicate::UNO, x, x);
  mlir::Value negative = compare(arith::CmpFPredicate::OLT, x, real(0));
  mlir::Value magnitude = arith::SelectOp::create(
      builder, at, negative, arith::NegFOp::create(builder, at, x), x);
  mlir::Value large = compare(arith::CmpFPredicate::OGE, magnitude, real(9e12));
  mlir::Value beyond =
      compare(arith::CmpFPredicate::OGE, magnitude, real(9e18));
  mlir::Value infinite = compare(
      arith::CmpFPredicate::OEQ, magnitude,
      real(std::numeric_limits<double>::infinity()));
  mlir::Value finite = compare(arith::CmpFPredicate::OLT, magnitude, real(9e18));
  // Values the conversions below cannot take are replaced by zero; their
  // result is not used.
  mlir::Value small = compare(arith::CmpFPredicate::OLT, magnitude, real(9e12));
  mlir::Value scaled = arith::FPToUIOp::create(
      builder, at, i64,
      arith::AddFOp::create(
          builder, at,
          arith::MulFOp::create(
              builder, at,
              arith::SelectOp::create(builder, at, small, magnitude, real(0)),
              real(1e6)),
          real(0.5)));
  mlir::Value million = integer(at, i64, 1000000);
  mlir::Value whole = arith::DivUIOp::create(builder, at, scaled, million);
  mlir::Value fraction = arith::RemUIOp::create(builder, at, scaled, million);

  // The fraction's six digits, of which the trailing zeros are dropped,
  // but not the first.
  TextType six = TextType::get(context, 6);
  IntegerType sixBits = six.getStorageType();
  mlir::Value kept = integer(at, i64, 1);
  uint64_t power = 100000;
  for (int digits = 2; digits <= 6; ++digits) {
    mlir::Value nonzero = arith::CmpIOp::create(
        builder, at, arith::CmpIPredicate::ne,
        arith::RemUIOp::create(builder, at, fraction, integer(at, i64, power)),
        integer(at, i64, 0));
    kept = arith::SelectOp::create(builder, at, nonzero,
                                   integer(at, i64, digits), kept);
    power /= 10;
  }
  mlir::Value fractionBits = arith::ExtUIOp::create(builder, at, sixBits, kept);
  power = 100000;
  for (int j = 0; j < 6; ++j) {
    mlir::Value digit = arith::RemUIOp::create(
        builder, at,
        arith::DivUIOp::create(builder, at, fraction, integer(at, i64, power)),
        integer(at, i64, 10));
    mlir::Value character = arith::ExtUIOp::create(
        builder, at, sixBits,
        arith::AddIOp::create(builder, at, digit, integer(at, i64, '0')));
    mlir::Value placed = arith::ShLIOp::create(
        builder, at, character, integer(at, sixBits, 16 + 8 * j));
    mlir::Value used = arith::CmpIOp::create(
        builder, at, arith::CmpIPredicate::ult, integer(at, i64, j), kept);
    fractionBits = arith::OrIOp::create(
        builder, at, fractionBits,
        arith::SelectOp::create(builder, at, used, placed,
                                integer(at, sixBits, 0)));
    power /= 10;
  }
  mlir::Value minus = sign(negative);
  mlir::Value fixed = textConcat(
      where,
      textConcat(where,
                 textConcat(where, minus, formatUnsigned(at, whole)),
                 textConstant(at, ".", TextType::get(context, 1))),
      textFromBits(at, fractionBits, six));
  auto result = cast<TextType>(fixed.getType());
  // From 9e12 on a double has no six decimals left: digits only.
  mlir::Value integral = arith::FPToUIOp::create(
      builder, at, i64,
      arith::SelectOp::create(
          builder, at,
          arith::AndIOp::create(builder, at, large, finite), magnitude,
          real(0)));
  mlir::Value digits = textResize(
      at, textConcat(where, minus, formatUnsigned(at, integral)), result);
  TextType three = TextType::get(context, 3);
  mlir::Value beyondText = textResize(
      at,
      textConcat(where, minus,
                 choose(infinite, textConstant(at, "inf", three),
                        textConstant(at, "big", three))),
      result);
  mlir::Value text = choose(large, digits, fixed);
  text = choose(beyond, beyondText, text);
  return choose(nan, textConstant(at, "nan", result), text);
}

FailureOr<mlir::Value> Parser::emitIf(const Expr &expr, Type expected) {
  Location at = loc(expr.loc);
  FailureOr<mlir::Value> condition =
      emit(*expr.operands[0], builder.getI1Type());
  if (failed(condition))
    return failure();
  if (!condition->getType().isInteger(1))
    return error(expr.loc, "an 'if' condition must be a bool");
  Type type = typeOf(expr);
  if (!type)
    type = expected ? expected : defaultType(*expr.thenBranch->value);
  auto branch = scf::IfOp::create(builder, at, TypeRange{type}, *condition,
                                  /*withElseRegion=*/true);
  for (auto [block, part] :
       {std::make_pair(branch.thenBlock(), expr.thenBranch.get()),
        std::make_pair(branch.elseBlock(), expr.elseBranch.get())}) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(block);
    ScopeGuard scope(*this);
    for (const Let &let : part->lets)
      if (failed(emitLet(let, /*isVar=*/false)))
        return failure();
    FailureOr<mlir::Value> value = emit(*part->value, type);
    if (failed(value))
      return failure();
    if (value->getType() != type)
      return error(part->value->loc,
                   "both branches of an 'if' must have the same type");
    scf::YieldOp::create(builder, at, ValueRange{*value});
  }
  return branch.getResult(0);
}

/// The values of a component's fields in declaration order.
FailureOr<SmallVector<mlir::Value>>
Parser::emitInitValues(const ComponentInit &init) {
  const Record &record = components[init.component];
  SmallVector<mlir::Value> values;
  for (auto &[field, type] : record.fields) {
    const Expr *value = nullptr;
    for (auto &[name, expr] : init.fields)
      if (name == field)
        value = expr.get();
    if (!value && llvm::is_contained(record.hidden, field)) {
      values.push_back(noughtOf(loc(init.loc), type));
      continue;
    }
    if (!value)
      return error(init.loc, "'" + init.component + "' needs a value for '" +
                                 field + "'");
    FailureOr<mlir::Value> emitted = emit(*value, type);
    if (failed(emitted))
      return failure();
    if (emitted->getType() != type)
      return error(value->loc, "value for '" + field +
                                   "' has a different type than the field");
    // (The field's own copy.)
    if (isa<StringType>(type))
      emitted = TextOwnOp::create(builder, loc(value->loc), type, *emitted)
                    .getResult();
    values.push_back(*emitted);
  }
  for (auto &[name, expr] : init.fields) {
    if (!record.fieldType(name))
      return error(expr->loc, "component '" + init.component +
                                  "' has no field '" + name + "'");
    if (llvm::is_contained(record.hidden, name))
      return error(expr->loc, "'" + name + "' is where an index keeps the "
                              "entity's row: the index gives it");
  }
  return values;
}

/// Emit what the entries from `from` on add to `spawned`, and then
/// `rest`, which gives the entity (the entries of the list this one is
/// in that come after it, and in the end the spawn itself). An `if`
/// makes two ways on, each with all that follows; a prefab's entries are
/// emitted where its parameters are what it was given, and nothing else
/// of where it is named is in sight.
FailureOr<mlir::Value>
Parser::emitSpawnEntries(const std::vector<SpawnEntry> &entries, size_t from,
                         Spawned spawned, llvm::SMLoc at,
                         function_ref<FailureOr<mlir::Value>(Spawned)> rest) {
  if (from == entries.size())
    return rest(std::move(spawned));
  const SpawnEntry &entry = entries[from];
  auto next = [&](Spawned so) {
    return emitSpawnEntries(entries, from + 1, std::move(so), at, rest);
  };
  switch (entry.kind) {
  case SpawnEntry::Init: {
    FailureOr<SmallVector<mlir::Value>> values = emitInitValues(entry.init);
    if (failed(values))
      return failure();
    std::string component = resolve(entry.init.component);
    auto *known = llvm::find_if(spawned.parts, [&](auto &part) {
      return part.first == component;
    });
    if (known == spawned.parts.end()) {
      spawned.parts.push_back({component, std::move(*values)});
    } else {
      // Listed again: the later one's values. (A text the earlier one
      // was to hold is given back.)
      for (mlir::Value value : known->second)
        if (isa<StringType>(value.getType()))
          TextDropOp::create(builder, loc(entry.loc), value);
      known->second = std::move(*values);
    }
    return next(std::move(spawned));
  }
  case SpawnEntry::Prefab: {
    Prefab &prefab = prefabs[entry.prefab];
    if (entry.args.size() != prefab.params.size())
      return error(entry.loc, "'" + entry.prefab + "' takes " +
                                  Twine(prefab.params.size()) +
                                  " argument(s), not " +
                                  Twine(entry.args.size()));
    llvm::StringMap<Variable> given;
    for (auto [arg, param] : llvm::zip(entry.args, prefab.params)) {
      FailureOr<mlir::Value> value = emit(*arg, param.second);
      if (failed(value))
        return failure();
      if (value->getType() != param.second)
        return error(arg->loc, "argument has a different type than the "
                               "parameter");
      given[param.first] = Variable::ofValue(*value);
    }
    // Its entries where only its parameters are, in its own module; what
    // comes after it, back where it was named.
    auto here = std::move(scopes);
    SourceModule *module = current;
    scopes.clear();
    scopes.push_back(std::move(given));
    current = prefab.home;
    FailureOr<mlir::Value> entity = emitSpawnEntries(
        prefab.entries, 0, std::move(spawned), at,
        [&](Spawned so) -> FailureOr<mlir::Value> {
          auto inside = std::move(scopes);
          scopes = std::move(here);
          current = module;
          FailureOr<mlir::Value> result = next(std::move(so));
          here = std::move(scopes);
          scopes = std::move(inside);
          current = prefab.home;
          return result;
        });
    scopes = std::move(here);
    current = module;
    return entity;
  }
  case SpawnEntry::If: {
    FailureOr<mlir::Value> condition =
        emit(*entry.condition, builder.getI1Type());
    if (failed(condition))
      return failure();
    if (!condition->getType().isInteger(1))
      return error(entry.condition->loc, "an 'if' condition must be a bool");
    auto branch =
        scf::IfOp::create(builder, loc(entry.loc), EntityType::get(context),
                          *condition, /*withElseRegion=*/true);
    for (bool taken : {true, false}) {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToEnd(taken ? branch.thenBlock()
                                           : branch.elseBlock());
      FailureOr<mlir::Value> entity = emitSpawnEntries(
          taken ? entry.then : entry.otherwise, 0, spawned, at, next);
      if (failed(entity))
        return failure();
      scf::YieldOp::create(builder, loc(entry.loc), *entity);
    }
    return branch.getResult(0);
  }
  }
  llvm_unreachable("unknown kind of entry");
}

FailureOr<mlir::Value> Parser::emitSpawn(const Expr &expr) {
  if (inEach)
    return error(expr.loc, "an entity is spawned after the 'for' that goes "
                           "through other entities, not in it: that one "
                           "only reads them");
  return emitSpawnEntries(
      expr.entries, 0, Spawned(), expr.loc,
      [&](Spawned spawned) -> FailureOr<mlir::Value> {
        if (spawned.parts.empty())
          return error(expr.loc, "a spawn lists at least one component");
        SmallVector<Attribute> listed;
        SmallVector<mlir::Value> values;
        for (auto &[component, fields] : spawned.parts) {
          if (inFunction) {
            bool given = false;
            for (auto &entry : givenComponents)
              if (symbol(entry.second).getValue() == component)
                given |= givenToChange(entry.second);
            if (!given)
              return error(expr.loc,
                           "a proc spawns an entity of components it was "
                           "given to change: '" + component +
                               "' is not one ('name: mut " + component +
                               "')");
          }
          listed.push_back(FlatSymbolRefAttr::get(context, component));
          values.append(fields.begin(), fields.end());
        }
        OperationState state(loc(expr.loc), SpawnOp::getOperationName());
        state.addAttribute("components", builder.getArrayAttr(listed));
        state.addOperands(values);
        state.addTypes(EntityType::get(context));
        return builder.create(state)->getResult(0);
      });
}

OwningOpRef<ModuleOp>
mlir::ent::importEnt(llvm::SourceMgr &sourceMgr, MLIRContext *context,
                     ArrayRef<std::string> directories, ImportedFiles *files) {
  context->loadDialect<EntDialect, arith::ArithDialect, math::MathDialect,
                       memref::MemRefDialect, scf::SCFDialect>();
  Parser parser(sourceMgr, context, directories);
  parser.files = files;
  return parser.parseModule();
}
