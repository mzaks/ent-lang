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

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
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
  return x.getWidth() >= y.getWidth() ? a : b;
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
    String, // "text": name holds its bytes
    Index,  // text[i]
    Format, // {value} inside a string: the value as text
    Tuple,  // (a, b): the values a fn gives
    Row,    // table[i].field: name the table, field the field (or none)
    Of,     // value.field: a field of the row a value is
    Given,  // a value that is there already (`given`)
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
  enum Kind { Value, Ref, Entity, Other, Row };
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
      : sourceMgr(sourceMgr), context(context),
        directories(directories),
        lexer(sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID())
                  ->getBuffer()),
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
        assetFiles([this](StringRef name) { return resolve(name); }),
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

  void advance() { token = lexer.next(); }
  /// The token after the current one.
  Token peek() {
    Lexer ahead = lexer;
    return ahead.next();
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
  LogicalResult parseUnique();
  LogicalResult parseArchetype();
  LogicalResult parseSystem(bool isExtern);
  LogicalResult parseFunction(bool proc, bool isExtern);
  LogicalResult parseEnum();
  LogicalResult parseTable();
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
  LogicalResult checkEaches(Operation *query);
  /// Inside the body of a `for` over entities that is in another.
  bool inEach = false;
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
  Token token;
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
  /// The files that have a name (`asset name = "file"`): as the program
  /// writes them.
  Declared<std::string> assetFiles;
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
  root->loading = false;
  finished.push_back(root);
  if (failed(finishRowKinds()) || failed(finishRows()) ||
      failed(finishCallables()))
    return nullptr;

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

// The declarations of one file, up to its end.
LogicalResult Parser::parseDeclarations() {
  while (!token.is(Token::Eof)) {
    LogicalResult result = success();
    if (consumeKeyword("component"))
      result = parseComponent(/*tag=*/false);
    else if (consumeKeyword("tag"))
      result = parseComponent(/*tag=*/true);
    else if (consumeKeyword("unique"))
      result = parseUnique();
    else if (consumeKeyword("relation"))
      result = parseRelation();
    else if (consumeKeyword("archetype"))
      result = parseArchetype();
    else if (consumeKeyword("system"))
      result = parseSystem(/*isExtern=*/false);
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
      if (!declared || !isa<ComponentOp, RelationOp, ArchetypeOp>(declared))
        return error(nameAt, "'" + *name + "' is no component, relation or "
                             "archetype declared before this: 'capacity' "
                             "gives one of those another capacity");
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
  SourceModule *importing = current;
  lexer = Lexer(text);
  current = imported;
  advance();
  LogicalResult result = parseDeclarations();
  lexer = savedLexer;
  token = savedToken;
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
  if (!type)
    return error(at, "unknown type '" + *name +
                         "'; expected f32, f64, bool, i8, i16, i32, i64, "
                         "index, entity, text, text[N] or an enum");
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
        return arith::ConstantOp::create(
            builder, at,
            cast<TypedAttr>(builder.getZeroAttr(callable->result)));
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
  for (auto &[field, type] : record.fields)
    if (isa<StringType>(type))
      return error(fieldsAt, "an edge does not hold a text of any length "
                             "yet ('" + field + "'): give it a capacity "
                             "('text[N]')");
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
    if (!isa<IntegerType>(type) || type.isInteger(1))
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
  ResourceOp::create(builder, loc(at), declareSymbol(at, *name),
                     builder.getArrayAttr(names), builder.getArrayAttr(types));
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

// system name(params) [reads A, B] [writes C] { statements }
// extern system name(params) [reads A, B] [writes C]
LogicalResult Parser::parseSystem(bool isExtern) {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a system name");
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

  OperationState state(loc(at), isExtern ? ExternOp::getOperationName()
                                         : SystemOp::getOperationName());
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     declareSymbol(at, *name));
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
  SmallVector<std::string> names;
  while (!token.is(Token::RParen)) {
    FailureOr<std::string> param = identifier("a parameter name");
    if (failed(param) || failed(expect(Token::Colon, "':'")))
      return failure();
    FailureOr<Type> type = parseType();
    if (failed(type))
      return failure();
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
  if (!isExtern && !token.is(Token::LBrace))
    return error(at, "a " + kind + " has a body ('{ ... }'); 'extern " +
                         kind + "' declares one implemented in C");
  if (isExtern)
    consumeIf(Token::Semicolon);
  if (name->size() == 3 && (*name == "min" || *name == "max" || *name == "len"))
    return error(at, "'" + *name + "' is built in");
  StringAttr symbolName = declareSymbol(at, *name);
  auto op = FunctionOp::create(builder, loc(at), symbolName,
                               builder.getTypeArrayAttr(function.params),
                               builder.getTypeArrayAttr(function.results),
                               proc);
  // Known before its body is parsed: a fn may call itself.
  Type result = function.result;
  SmallVector<Type> results = function.results;
  SmallVector<Type> params = function.params;
  functions[*name] = std::move(function);
  if (isExtern)
    return success();

  auto *block = new Block();
  op.getBody().push_back(block);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [param, type] : llvm::zip(names, params))
    bind(param, Variable::ofValue(block->addArgument(type, loc(at))));
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
      statement = next.is(Token::Assign) || next.is(Token::PlusAssign) ||
                  next.is(Token::MinusAssign) || next.is(Token::StarAssign) ||
                  next.is(Token::SlashAssign) || next.isKeyword("min") ||
                  next.isKeyword("max");
    }
    // (In a proc: a call of a proc, for what it does.)
    if (proc && token.is(Token::Identifier) && peek().is(Token::LParen)) {
      Callable *through = callableNamed(token.spelling);
      statement = through ? through->proc && !through->result
                          : functions.count(token.spelling) &&
                                functions[token.spelling].results.empty();
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
  while (!token.is(Token::RBrace)) {
    if (token.is(Token::Eof))
      return error("expected '}'");
    if (failed(parseStatement()))
      return failure();
    consumeIf(Token::Semicolon);
  }
  advance();
  return success();
}

LogicalResult Parser::parseStatement() {
  llvm::SMLoc at = token.loc;
  if (consumeKeyword("let")) {
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
  while (!filter()) {
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

  OperationState state(loc(at), EdgesOp::getOperationName());
  state.addAttribute("relation", symbol(*relation));
  state.addAttribute("direction", builder.getStringAttr(direction));
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  block->addArgument(RefType::get(context, symbol(*relation), mut), loc(at));
  block->addArgument(EntityType::get(context), loc(at));
  Operation *edges = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  edgesScopes = scopes.size();
  ScopeGuard scope(*this);
  if (!edge.empty())
    bind(edge, {Variable::Ref, block->getArgument(0), *relation, mut});
  if (!other.name.empty())
    bind(other.name, Variable::ofValue(block->getArgument(1)));
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
  if (has) {
    auto branch = scf::IfOp::create(builder, loc(other.loc), has);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
  }
  inEdges = true;
  llvm::scope_exit leave([&] { inEdges = false; });
  if (failed(parseBlock()))
    return failure();
  EdgesOp::ensureTerminator(edges->getRegion(0), builder, loc(at));
  return success();
}

// connect (source)-[Relation { field: value, ... }]->(target), and on:
// ...->(target)<-[Relation]-(another), every arrow an edge.
LogicalResult Parser::parseConnect(llvm::SMLoc at) {
  if (inFunction)
    return error(at, "a fn only computes; a system connects");
  if (inEdges)
    return error(at, "'connect' inside an edge loop is not supported yet");
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
    if (failed(parseBlock()))
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
          goesOn = *value;
        if (!goesOn)
          goesOn = arith::ConstantIntOp::create(builder, where, 0, 1);
        scf::ConditionOp::create(builder, where, goesOn, arguments);
      },
      [&](OpBuilder &, Location, ValueRange arguments) {
        ScopeGuard scope(*this);
        bindTo(arguments);
        SmallVector<mlir::Value> ends(arguments.begin(), arguments.end());
        if (succeeded(parsed)) {
          if (failed(parseBlock())) {
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
  for (auto [state, result] : llvm::zip(vars, loop.getResults()))
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
//     [cascade R [leaves first]] [where cond] [on trigger, ...] { }
LogicalResult Parser::parseFor() {
  llvm::SMLoc at = token.loc;
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
  // name: [mut] Component, one or more, into `into`; the first name was
  // read (`pending`).
  // `optional` before a name: the entity may be without the component,
  // and the binding is read with `if let`.
  auto parseBindings = [&](std::string pending,
                           SmallVectorImpl<Binding> &into,
                           bool optional = false) -> LogicalResult {
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
    if (hasBindings && failed(parseBindings(pending, bindings, firstOptional)))
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
  ExprPtr where;
  if (consumeKeyword("where")) {
    FailureOr<ExprPtr> condition = parseExpr();
    if (failed(condition))
      return failure();
    where = std::move(*condition);
  }
  SmallVector<Attribute> triggers;
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
  if (failed(checkEaches(query)))
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

// `=`, `+=`, `-=`, `*=`, `/=`, `min=`, `max=`
FailureOr<std::pair<Token::Kind, std::optional<StringRef>>>
Parser::parseAssignOp() {
  Token::Kind kind = token.kind;
  switch (kind) {
  case Token::Assign:
  case Token::PlusAssign:
  case Token::MinusAssign:
  case Token::StarAssign:
  case Token::SlashAssign:
    advance();
    return std::make_pair(kind, std::optional<StringRef>());
  default:
    break;
  }
  if (token.isKeyword("min") || token.isKeyword("max")) {
    StringRef rule = token.isKeyword("min") ? "min" : "max";
    advance();
    if (failed(expect(Token::Assign, "'='")))
      return failure();
    return std::make_pair(Token::Assign, std::optional<StringRef>(rule));
  }
  return error("expected an assignment ('=', '+=', '-=', '*=', '/=', "
               "'min=', 'max='), found '" +
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
  if (type.isInteger(1))
    return "a bool";
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
  FailureOr<mlir::Value> rhs = emit(value, type);
  if (failed(rhs))
    return failure();
  if (rhs->getType() != type)
    return mismatch(value.loc, rhs->getType(), type);
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
  Token::Kind arith = op == Token::PlusAssign    ? Token::Plus
                      : op == Token::MinusAssign ? Token::Minus
                      : op == Token::StarAssign  ? Token::Star
                                                 : Token::Slash;
  FailureOr<mlir::Value> result = arithmetic(loc(at), arith, old, *rhs);
  if (failed(result))
    return failure();
  return store(*result);
}

/// A statement starting with a name: an assignment to a field, a unique or
/// another entity's field, or a method on the visited entity.
LogicalResult Parser::parseNameStatement() {
  llvm::SMLoc at = token.loc;
  std::string name = token.spelling.str();
  advance();

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
          arith::ConstantOp::create(builder, loc(at),
                                    builder.getZeroAttr(type)),
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
          arith::ConstantOp::create(builder, loc(at),
                                    builder.getZeroAttr(type)),
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
    if (inQuery) {
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
            arith::ConstantOp::create(builder, loc(at),
                                      builder.getZeroAttr(type)),
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
            arith::ConstantOp::create(builder, loc(at),
                                      builder.getZeroAttr(type)),
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
    if (inEdges && scope < edgesScopes)
      return error(at, "assigning a var from outside an edge loop is not "
                       "supported yet");
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
  case Token::Plus:
  case Token::Minus:
    return 5;
  case Token::Star:
  case Token::Slash:
  case Token::Percent:
    return 6;
  default:
    return -1;
  }
}

FailureOr<ExprPtr> Parser::parseBinary(int minPrecedence) {
  FailureOr<ExprPtr> lhs = parseUnary();
  if (failed(lhs))
    return failure();
  while (true) {
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
  if (token.is(Token::Minus) || token.is(Token::Not)) {
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
    if ((*primary)->kind == Expr::Name && !lookup((*primary)->name) &&
        tables.count((*primary)->name)) {
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
  if (token.is(Token::LParen)) {
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
    } else if (consumeIf(Token::LParen)) {
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
      if (variable->kind == Variable::Row)
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
      if (variable->kind == Variable::Ref)
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
    default: {
      // Joining texts gives a text that holds both: one of any length
      // where one of them is.
      Type a = textTypeOf(*expr.operands[0]), b = textTypeOf(*expr.operands[1]);
      bool anyA = isAnyText(*expr.operands[0]);
      bool anyB = isAnyText(*expr.operands[1]);
      if (expr.op == Token::Plus && (anyA || anyB) && (a || anyA) &&
          (b || anyB))
        return StringType::get(context);
      if (expr.op == Token::Plus && a && b)
        return TextType::get(
            context, std::min<unsigned>(cast<TextType>(a).getCapacity() +
                                            cast<TextType>(b).getCapacity(),
                                        TextType::kMaxCapacity));
      if (Type type = typeOf(*expr.operands[0])) {
        if (Type other = typeOf(*expr.operands[1]))
          if (Type wider = widerInteger(type, other))
            return wider;
        return type;
      }
      return typeOf(*expr.operands[1]);
    }
    }
  case Expr::Call:
    if (Callable *through = callableThrough(expr))
      return through->result;
    if (!expr.field.empty())
      return {};
    if (expr.name == "len")
      return builder.getI32Type();
    if (functions.count(expr.name))
      return functions[expr.name].result;
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
    return builder.getI1Type();
  case Expr::Given:
    return expr.given.getType();
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
  for (auto [operand, type] : llvm::zip(expr.operands, function.params)) {
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
      value->getType() != expected)
    return arith::ExtSIOp::create(builder, loc(expr.loc), expected, *value)
        .getResult();
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
      return arith::ConstantOp::create(
                 builder, at, IntegerAttr::get(type, APInt(width, bits)))
          .getResult();
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
    if (isa<FloatType>(operand->getType()))
      return arith::NegFOp::create(builder, at, *operand).getResult();
    return arithmetic(at, Token::Minus,
                      arith::ConstantOp::create(
                          builder, at, builder.getZeroAttr(operand->getType())),
                      *operand);
  }
  case Expr::Binary:
    return emitBinary(expr, expected);
  case Expr::Call: {
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
    if (expr.name != "min" && expr.name != "max")
      return error(expr.loc, "unknown function '" + expr.name +
                                 "'; declare it before with 'fn', or one "
                                 "implemented in C with 'extern fn' or "
                                 "'extern proc' (min, max and len are "
                                 "built in)");
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
      if (!other.isSignlessInteger() || other.isInteger(1))
        return error(expr.loc, "an enum is cast to and from an integer, the "
                               "number of its case");
      Type byte = builder.getIntegerType(8);
      unsigned width = other.getIntOrFloatBitWidth();
      if (fromEnum) {
        mlir::Value number =
            UnrealizedConversionCastOp::create(builder, at, byte, *value)
                .getResult(0);
        return width == 8 ? number
                          : arith::ExtUIOp::create(builder, at, other, number)
                                .getResult();
      }
      mlir::Value number =
          width == 8
              ? *value
              : arith::TruncIOp::create(builder, at, byte, *value).getResult();
      return UnrealizedConversionCastOp::create(builder, at, expr.castType,
                                                number)
          .getResult(0);
    }
    // A literal is written in the type it is cast to (`5000000000 as i64`).
    if (!from && (isa<FloatType>(expr.castType) ||
                  (expr.castType.isSignlessInteger() &&
                   !expr.castType.isInteger(1) &&
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
    if (fromFloat && to.isSignlessInteger())
      return arith::FPToSIOp::create(builder, at, to, *value).getResult();
    if (from.isSignlessInteger() && toFloat)
      return arith::SIToFPOp::create(builder, at, to, *value).getResult();
    if (from.isSignlessInteger() && to.isSignlessInteger())
      return from.getIntOrFloatBitWidth() < to.getIntOrFloatBitWidth()
                 ? arith::ExtSIOp::create(builder, at, to, *value).getResult()
                 : arith::TruncIOp::create(builder, at, to, *value)
                       .getResult();
    if (from.isIntOrIndex() && to.isIntOrIndex())
      return arith::IndexCastOp::create(builder, at, to, *value).getResult();
    return error(expr.loc, "cannot cast between these types");
  }
  case Expr::If:
    return emitIf(expr, expected);
  case Expr::Spawn:
    if (inFunction)
      return error(expr.loc, "a fn only computes; a system spawns");
    return emitSpawn(expr);
  case Expr::Given:
    return expr.given;
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
  }
  return error(expr.loc, "unsupported expression");
}

FailureOr<mlir::Value> Parser::arithmetic(Location at, Token::Kind op,
                                          mlir::Value a, mlir::Value b) {
  Type type = a.getType();
  bool isFloat = isa<FloatType>(type);
  if (!isFloat && (!type.isIntOrIndex() || type.isInteger(1)))
    return emitError(at, "arithmetic needs numbers"), failure();
  switch (op) {
  case Token::Plus:
    return isFloat ? arith::AddFOp::create(builder, at, a, b).getResult()
                   : arith::AddIOp::create(builder, at, a, b).getResult();
  case Token::Minus:
    return isFloat ? arith::SubFOp::create(builder, at, a, b).getResult()
                   : arith::SubIOp::create(builder, at, a, b).getResult();
  case Token::Star:
    return isFloat ? arith::MulFOp::create(builder, at, a, b).getResult()
                   : arith::MulIOp::create(builder, at, a, b).getResult();
  case Token::Slash:
    return isFloat ? arith::DivFOp::create(builder, at, a, b).getResult()
                   : arith::DivSIOp::create(builder, at, a, b).getResult();
  case Token::Percent:
    return isFloat ? arith::RemFOp::create(builder, at, a, b).getResult()
                   : arith::RemSIOp::create(builder, at, a, b).getResult();
  default:
    return emitError(at, "not an arithmetic operator"), failure();
  }
}

mlir::Value Parser::combine(Location at, StringRef rule, mlir::Value a,
                            mlir::Value b) {
  bool isFloat = isa<FloatType>(a.getType());
  if (rule == "min")
    return isFloat ? arith::MinimumFOp::create(builder, at, a, b).getResult()
                   : arith::MinSIOp::create(builder, at, a, b).getResult();
  return isFloat ? arith::MaximumFOp::create(builder, at, a, b).getResult()
                 : arith::MaxSIOp::create(builder, at, a, b).getResult();
}

FailureOr<mlir::Value> Parser::emitBinary(const Expr &expr, Type expected) {
  Location at = loc(expr.loc);
  const Expr &lhs = *expr.operands[0], &rhs = *expr.operands[1];
  if (expr.op == Token::AndAnd || expr.op == Token::OrOr) {
    FailureOr<mlir::Value> a = emit(lhs, builder.getI1Type());
    FailureOr<mlir::Value> b = emit(rhs, builder.getI1Type());
    if (failed(a) || failed(b))
      return failure();
    if (!a->getType().isInteger(1) || !b->getType().isInteger(1))
      return error(expr.loc, "'&&' and '||' need bools");
    return expr.op == Token::AndAnd
               ? arith::AndIOp::create(builder, at, *a, *b).getResult()
               : arith::OrIOp::create(builder, at, *a, *b).getResult();
  }
  bool comparison = precedenceOf(expr.op) == 3 || precedenceOf(expr.op) == 4;
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
    return error(expr.loc, "operands have different types");
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
    arith::CmpFPredicate predicate;
    switch (expr.op) {
    case Token::Equal:
      predicate = arith::CmpFPredicate::OEQ;
      break;
    case Token::NotEqual:
      predicate = arith::CmpFPredicate::ONE;
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
  return arith::CmpIOp::create(builder, at, predicate, *a, *b).getResult();
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
      lexer = Lexer(rest.take_front(close));
      advance();
      FailureOr<ExprPtr> value = parseExpr();
      bool whole = token.is(Token::Eof);
      lexer = savedLexer;
      token = savedToken;
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
  return arith::ConstantOp::create(
      builder, at,
      IntegerAttr::get(type, APInt(type.getIntOrFloatBitWidth(), value)));
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
  for (auto &[name, expr] : init.fields)
    if (!record.fieldType(name))
      return error(expr->loc, "component '" + init.component +
                                  "' has no field '" + name + "'");
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
  context->loadDialect<EntDialect, arith::ArithDialect, scf::SCFDialect>();
  Parser parser(sourceMgr, context, directories);
  parser.files = files;
  return parser.parseModule();
}
