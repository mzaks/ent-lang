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
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/SourceMgr.h"

#include <memory>
#include <optional>

using namespace mlir;
using namespace mlir::ent;

namespace {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

/// `Position { x: 1.0 }` in a spawn or an add.
struct ComponentInit {
  std::string component;
  llvm::SMLoc loc;
  SmallVector<std::pair<std::string, ExprPtr>> fields;
};

/// A branch of an if-expression: `{ let a = ...; value }`.
struct Branch {
  SmallVector<std::pair<std::string, ExprPtr>> lets;
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
  };
  Kind kind;
  llvm::SMLoc loc;
  int64_t intValue = 0;
  double floatValue = 0;
  bool boolValue = false;
  std::string name, field;
  Token::Kind op = Token::Eof;
  SmallVector<ExprPtr> operands;
  Type castType;
  std::unique_ptr<Branch> thenBranch, elseBranch;
  SmallVector<ComponentInit> inits;
};

/// A record declared in the source: a component or a unique.
struct Record {
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

/// What a name stands for in a body.
struct Variable {
  enum Kind { Value, Ref, Entity };
  Kind kind;
  mlir::Value value;
  std::string component; // for Ref
  bool mut = false;      // for Ref

  static Variable ofValue(mlir::Value value) {
    return {Value, value, std::string(), false};
  }
  static Variable ofEntity(mlir::Value value) {
    return {Entity, value, std::string(), false};
  }
};

class Parser {
public:
  Parser(llvm::SourceMgr &sourceMgr, MLIRContext *context)
      : sourceMgr(sourceMgr), context(context),
        lexer(sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID())
                  ->getBuffer()),
        builder(context) {
    advance();
  }

  OwningOpRef<ModuleOp> parseModule();

private:
  //===--------------------------------------------------------------===//
  // Tokens and diagnostics
  //===--------------------------------------------------------------===//

  void advance() { token = lexer.next(); }

  Location loc(llvm::SMLoc at) {
    auto [line, column] = sourceMgr.getLineAndColumn(at);
    StringRef file =
        sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID())
            ->getBufferIdentifier();
    return FileLineColLoc::get(context, file, line, column);
  }

  LogicalResult error(llvm::SMLoc at, const Twine &message) {
    emitError(loc(at)) << message;
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
  LogicalResult parseComponent(bool tag);
  LogicalResult parseUnique();
  LogicalResult parseArchetype();
  LogicalResult parseSystem();
  LogicalResult parseSchedule();
  FailureOr<ArrayAttr> parseNameList(StringRef what);

  //===--------------------------------------------------------------===//
  // Statements (emitted as they are parsed)
  //===--------------------------------------------------------------===//

  LogicalResult parseBlock();
  LogicalResult parseStatement();
  LogicalResult parseFor();
  LogicalResult parseIf();
  LogicalResult parseIfLet(llvm::SMLoc at);
  LogicalResult parseNameStatement();
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
  LogicalResult parseConnect(llvm::SMLoc at);
  LogicalResult emitCondition(const Expr &expr);
  FailureOr<std::unique_ptr<Branch>> parseBranch();
  FailureOr<ComponentInit> parseComponentInit();

  Type typeOf(const Expr &expr);
  Type defaultType(const Expr &expr);
  FailureOr<mlir::Value> emit(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitBinary(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitIf(const Expr &expr, Type expected);
  FailureOr<mlir::Value> emitSpawn(const Expr &expr);
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
    return FlatSymbolRefAttr::get(context, name);
  }

  llvm::SourceMgr &sourceMgr;
  MLIRContext *context;
  Lexer lexer;
  Token token;
  OpBuilder builder;
  ModuleOp module;

  llvm::StringMap<Record> components;
  llvm::StringMap<Record> uniques;
  llvm::StringMap<Record> relations;

  /// The fields a ref's variable has: a component's, or for an edge a
  /// relation's.
  const Record &recordOf(const Variable &variable) {
    auto relation = relations.find(variable.component);
    if (relation != relations.end())
      return relation->second;
    return components[variable.component];
  }
  llvm::StringMap<SmallVector<Type>> systems;
  SmallVector<llvm::StringMap<Variable>> scopes;
  /// Inside a `for`: the name of the visited entity, if bound.
  bool inQuery = false;
  bool inEdges = false;
  std::string queryEntity;
};

} // namespace

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

OwningOpRef<ModuleOp> Parser::parseModule() {
  OwningOpRef<ModuleOp> owned = ModuleOp::create(loc(token.loc));
  module = *owned;
  builder.setInsertionPointToEnd(module.getBody());
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
      result = parseSystem();
    else if (consumeKeyword("schedule"))
      result = parseSchedule();
    else if (consumeKeyword("default_capacity")) {
      FailureOr<int64_t> capacity = integer("a capacity");
      if (failed(capacity))
        return nullptr;
      module->setAttr("ent.default_capacity",
                      builder.getI64IntegerAttr(*capacity));
    } else if (token.isKeyword("world") || token.isKeyword("proc") ||
               token.isKeyword("fn") || token.isKeyword("device") ||
               token.isKeyword("prefab")) {
      result = error("'" + token.spelling + "' is not supported yet");
    } else {
      result = error("expected a declaration (component, tag, unique, "
                     "relation, archetype, system, schedule), found '" +
                     token.spelling + "'");
    }
    if (failed(result))
      return nullptr;
  }
  if (failed(verify(module)))
    return nullptr;
  return owned;
}

FailureOr<Type> Parser::parseType() {
  llvm::SMLoc at = token.loc;
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
  if (!type)
    return error(at, "unknown type '" + *name +
                         "'; expected f32, f64, bool, i8, i16, i32, i64, "
                         "index or entity");
  return type;
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

// component Name { fields } [capacity N] / tag Name [capacity N]
LogicalResult Parser::parseComponent(bool tag) {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a component name");
  if (failed(name))
    return failure();
  Record record;
  if (!tag && failed(parseFields(record)))
    return failure();
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
  ComponentOp::create(builder, loc(at), builder.getStringAttr(*name),
                      builder.getArrayAttr(names), builder.getArrayAttr(types),
                      capacity);
  components[*name] = std::move(record);
  return success();
}

// relation Name [{ fields }] capacity N
LogicalResult Parser::parseRelation() {
  llvm::SMLoc at = token.loc;
  FailureOr<std::string> name = identifier("a relation name");
  if (failed(name))
    return failure();
  if (components.count(*name) || uniques.count(*name))
    return error(at, "'" + *name + "' is already declared");
  Record record;
  if (token.is(Token::LBrace) && failed(parseFields(record)))
    return failure();
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
  RelationOp::create(builder, loc(at), builder.getStringAttr(*name),
                     builder.getArrayAttr(names), builder.getArrayAttr(types),
                     builder.getI64IntegerAttr(*capacity));
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
  ResourceOp::create(builder, loc(at), builder.getStringAttr(*name),
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
  ArchetypeOp::create(builder, loc(at), builder.getStringAttr(*name),
                      builder.getArrayAttr(all),
                      optional.empty() ? ArrayAttr()
                                       : builder.getArrayAttr(optional),
                      builder.getI64IntegerAttr(*capacity));
  return success();
}

// A, B, ... (up to the next keyword or '{')
FailureOr<ArrayAttr> Parser::parseNameList(StringRef what) {
  SmallVector<Attribute> names;
  if (token.is(Token::LBrace) || token.isKeyword("writes"))
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
LogicalResult Parser::parseSystem() {
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

  OperationState state(loc(at), SystemOp::getOperationName());
  state.addAttribute(SymbolTable::getSymbolAttrName(),
                     builder.getStringAttr(*name));
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

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [param, arg] : llvm::zip(params, block->getArguments()))
    bind(param.first, Variable::ofValue(arg));
  if (failed(parseBlock()))
    return failure();
  SystemOp::ensureTerminator(system->getRegion(0), builder, loc(at));
  return success();
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
                     builder.getStringAttr(*name));
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  for (auto &[param, type] : params)
    block->addArgument(type, loc(at));
  state.addRegion(); // the condition, filled below if there is one
  Operation *schedule = builder.create(state);

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

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

// { statement* }, at the builder's insertion point.
LogicalResult Parser::parseBlock() {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  ScopeGuard scope(*this);
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
    FailureOr<std::string> name = identifier("a name");
    if (failed(name) || failed(expect(Token::Assign, "'='")))
      return failure();
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    FailureOr<mlir::Value> emitted = emit(**value, Type());
    if (failed(emitted))
      return failure();
    bind(*name, Variable::ofValue(*emitted));
    return success();
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
  if (token.isKeyword("return") || token.isKeyword("while") ||
      token.isKeyword("loop") || token.isKeyword("var"))
    return error("'" + token.spelling + "' is not supported yet");
  if (token.is(Token::Identifier))
    return parseNameStatement();
  return error(at, "expected a statement, found '" + token.spelling + "'");
}

// for [mut] s, other in e.out(R) { } / e.in(R), inside a `for`
LogicalResult Parser::parseEdges(llvm::SMLoc at) {
  if (inEdges)
    return error(at, "edge loops cannot be nested");
  bool mut = consumeKeyword("mut");
  FailureOr<std::string> edge = identifier("a name for the edge");
  if (failed(edge))
    return failure();
  if (token.is(Token::Colon))
    return error(at, "a 'for' cannot be nested in another 'for'; inside "
                     "one, 'for s, other in e.out(R)' visits the entity's "
                     "edges");
  if (failed(expect(Token::Comma, "','")))
    return failure();
  FailureOr<std::string> other =
      identifier("a name for the entity at the other end");
  if (failed(other) || failed(expectKeyword("in")))
    return failure();
  llvm::SMLoc entityAt = token.loc;
  FailureOr<std::string> entity = identifier("the visited entity");
  if (failed(entity))
    return failure();
  if (*entity != queryEntity)
    return error(entityAt,
                 queryEntity.empty()
                     ? "edges are visited from the entity a 'for' visits; "
                       "name it: 'for e, ...'"
                     : "edges are visited from the entity the 'for' visits, '" +
                           queryEntity + "', not '" + *entity + "'");
  if (failed(expect(Token::Dot, "'.'")))
    return failure();
  llvm::SMLoc directionAt = token.loc;
  FailureOr<std::string> direction = identifier("'out' or 'in'");
  if (failed(direction))
    return failure();
  if (*direction != "out" && *direction != "in")
    return error(directionAt, "expected 'out' or 'in', found '" + *direction +
                                  "'");
  if (failed(expect(Token::LParen, "'('")))
    return failure();
  llvm::SMLoc relationAt = token.loc;
  FailureOr<std::string> relation = identifier("a relation");
  if (failed(relation) || failed(expect(Token::RParen, "')'")))
    return failure();
  if (!relations.count(*relation))
    return error(relationAt, "unknown relation '" + *relation + "'");

  OperationState state(loc(at), EdgesOp::getOperationName());
  state.addAttribute("relation", symbol(*relation));
  state.addAttribute("direction", builder.getStringAttr(*direction));
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  block->addArgument(RefType::get(context, symbol(*relation), mut), loc(at));
  block->addArgument(EntityType::get(context), loc(at));
  Operation *edges = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  bind(*edge, {Variable::Ref, block->getArgument(0), *relation, mut});
  bind(*other, Variable::ofValue(block->getArgument(1)));
  inEdges = true;
  llvm::scope_exit leave([&] { inEdges = false; });
  if (failed(parseBlock()))
    return failure();
  EdgesOp::ensureTerminator(edges->getRegion(0), builder, loc(at));
  return success();
}

// connect(source, target, Relation { field: value, ... })
LogicalResult Parser::parseConnect(llvm::SMLoc at) {
  if (failed(expect(Token::LParen, "'('")))
    return failure();
  FailureOr<ExprPtr> source = parseExpr();
  if (failed(source) || failed(expect(Token::Comma, "','")))
    return failure();
  FailureOr<ExprPtr> target = parseExpr();
  if (failed(target) || failed(expect(Token::Comma, "','")))
    return failure();
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
  if (failed(expect(Token::RParen, "')'")))
    return failure();
  if (inEdges)
    return error(at, "'connect' inside an edge loop is not supported yet");
  FailureOr<mlir::Value> sourceValue =
      emit(**source, EntityType::get(context));
  FailureOr<mlir::Value> targetValue =
      emit(**target, EntityType::get(context));
  if (failed(sourceValue) || failed(targetValue))
    return failure();
  if (!isa<EntityType>(sourceValue->getType()) ||
      !isa<EntityType>(targetValue->getType()))
    return error(at, "'connect' takes two entities");
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
      return error(expr->loc, "relation '" + *relation + "' has no field '" +
                                  name + "'");
  ConnectOp::create(builder, loc(at), symbol(*relation), *sourceValue,
                    *targetValue, values);
  return success();
}

// for [e,] [p: [mut] P, ...] [with A, any(B, C)] [without D]
//     [where cond] [on trigger, ...] { }
LogicalResult Parser::parseFor() {
  llvm::SMLoc at = token.loc;
  if (inQuery)
    return parseEdges(at);
  if (!isa<SystemOp>(builder.getInsertionBlock()->getParentOp()))
    return error(at, "a 'for' must be at the top level of a system");

  struct Binding {
    std::string name, component;
    bool mut;
  };
  SmallVector<Binding> bindings;
  std::string entity;
  auto startsFilter = [&] {
    return token.isKeyword("with") || token.isKeyword("without");
  };
  // The first name is the entity if no ':' follows it; a query may filter
  // without binding anything (`for e with Enemy`, `for with Enemy`).
  std::string pending;
  bool hasBindings = !startsFilter();
  if (hasBindings) {
    FailureOr<std::string> first = identifier("a binding");
    if (failed(first))
      return failure();
    pending = *first;
    if (startsFilter()) {
      entity = pending;
      hasBindings = false;
    } else if (consumeIf(Token::Comma)) {
      entity = pending;
      FailureOr<std::string> next = identifier("a binding");
      if (failed(next))
        return failure();
      pending = *next;
    }
  }
  while (hasBindings) {
    if (failed(expect(Token::Colon, "':' and a component")))
      return failure();
    bool mut = consumeKeyword("mut");
    llvm::SMLoc componentAt = token.loc;
    FailureOr<std::string> component = identifier("a component");
    if (failed(component))
      return failure();
    if (!components.count(*component))
      return error(componentAt, "unknown component '" + *component + "'");
    bindings.push_back({pending, *component, mut});
    if (!consumeIf(Token::Comma))
      break;
    FailureOr<std::string> next = identifier("a binding");
    if (failed(next))
      return failure();
    pending = *next;
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
      llvm::SMLoc componentAt = token.loc;
      FailureOr<std::string> component = identifier("a component");
      if (failed(component))
        return failure();
      if (!components.count(*component))
        return error(componentAt, "unknown component '" + *component + "'");
      std::string field;
      if (*kind == "changed" && consumeIf(Token::Dot)) {
        FailureOr<std::string> name = identifier("a field");
        if (failed(name))
          return failure();
        field = *name;
      }
      SmallVector<Attribute, 4> entry{builder.getStringAttr(*kind),
                                      symbol(*component),
                                      builder.getStringAttr(field)};
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
  Region *body = state.addRegion();
  auto *block = new Block();
  body->push_back(block);
  for (const Binding &binding : bindings)
    block->addArgument(
        RefType::get(context, symbol(binding.component), binding.mut),
        loc(at));
  Operation *query = builder.create(state);

  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(block);
  ScopeGuard scope(*this);
  for (auto [binding, arg] : llvm::zip(bindings, block->getArguments()))
    bind(binding.name, {Variable::Ref, arg, binding.component, binding.mut});
  inQuery = true;
  queryEntity = entity;
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
  if (failed(parseBlock()))
    return failure();
  QueryOp::ensureTerminator(query->getRegion(0), builder, loc(at));
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
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    if (failed(parseBlock()))
      return failure();
  }
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
  return success();
}

// if let x = Component(id).field { found } [else { not found }]
LogicalResult Parser::parseIfLet(llvm::SMLoc at) {
  FailureOr<std::string> name = identifier("a name");
  if (failed(name) || failed(expect(Token::Assign, "'='")))
    return failure();
  llvm::SMLoc componentAt = token.loc;
  FailureOr<std::string> component = identifier("a component");
  if (failed(component))
    return failure();
  auto record = components.find(*component);
  if (record == components.end())
    return error(componentAt, "'if let' reads another entity's field: "
                              "expected 'Component(entity).field'");
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
    return error(fieldAt, "component '" + *component + "' has no field '" +
                              *field + "'");
  FailureOr<mlir::Value> entity = emit(**id, EntityType::get(context));
  if (failed(entity))
    return failure();
  auto lookupOp =
      LookupOp::create(builder, loc(at), type, builder.getI1Type(), *entity,
                       symbol(*component), builder.getStringAttr(*field));
  auto branch = scf::IfOp::create(builder, loc(at), lookupOp.getFound(),
                                  /*withElseRegion=*/true);
  {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.thenBlock()->getTerminator());
    ScopeGuard scope(*this);
    bind(*name, Variable::ofValue(lookupOp.getValue()));
    if (failed(parseBlock()))
      return failure();
  }
  if (consumeKeyword("else")) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(branch.elseBlock()->getTerminator());
    if (failed(parseBlock()))
      return failure();
  } else {
    branch.getElseRegion().getBlocks().clear();
  }
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
LogicalResult Parser::emitAssignment(
    llvm::SMLoc at, Token::Kind op, std::optional<StringRef> rule,
    function_ref<Type()> targetType, function_ref<mlir::Value()> load,
    function_ref<LogicalResult(mlir::Value)> store, const Expr &value) {
  Type type = targetType();
  FailureOr<mlir::Value> rhs = emit(value, type);
  if (failed(rhs))
    return failure();
  if (rhs->getType() != type)
    return error(value.loc, "value has a different type than the target");
  if (op == Token::Assign && !rule)
    return store(*rhs);
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
    FailureOr<ExprPtr> value = parseExpr();
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
  // e.destroy(), e.add(C { .. }), e.remove(C)
  if (variable && variable->kind == Variable::Entity) {
    if (failed(expect(Token::Dot, "'.' and a method")))
      return failure();
    return parseMethod(name, at);
  }

  // A unique: Unique.field op value, or Unique op value (shorthand).
  auto unique = uniques.find(name);
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
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    if (inQuery) {
      // Inside a query a unique can only be accumulated into.
      StringRef rule;
      bool negate = false;
      if (op->second)
        rule = *op->second;
      else if (op->first == Token::PlusAssign)
        rule = "add";
      else if (op->first == Token::MinusAssign)
        rule = "add", negate = true;
      else
        return error(at, "inside a 'for', a unique can only be accumulated "
                         "into ('+=', '-=', 'min=', 'max='): every entity "
                         "would write the same field");
      FailureOr<mlir::Value> rhs = emit(**value, type);
      if (failed(rhs))
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
    if (!variable->mut)
      return error(at, edge ? "'" + name + "' is not 'mut': bind it as 'for "
                                  "mut " + name + ", ...' to write it"
                            : "'" + name + "' is not 'mut': bind it as '" +
                                  name + ": mut " + variable->component +
                                  "' to write it");
    auto op = parseAssignOp();
    if (failed(op))
      return failure();
    FailureOr<ExprPtr> value = parseExpr();
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

  if (variable)
    return error(at, "'" + name + "' is a value and cannot be assigned; "
                                  "locals are immutable");
  return error(at, "unknown name '" + name + "'");
}

// e.destroy() / e.add(C { .. }) / e.remove(C)
LogicalResult Parser::parseMethod(const std::string &entity, llvm::SMLoc at) {
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
  if (*method == "add") {
    FailureOr<ComponentInit> init = parseComponentInit();
    if (failed(init) || failed(expect(Token::RParen, "')'")))
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

// { [let x = e;]* value }
FailureOr<std::unique_ptr<Branch>> Parser::parseBranch() {
  if (failed(expect(Token::LBrace, "'{'")))
    return failure();
  auto branch = std::make_unique<Branch>();
  while (consumeKeyword("let")) {
    FailureOr<std::string> name = identifier("a name");
    if (failed(name) || failed(expect(Token::Assign, "'='")))
      return failure();
    FailureOr<ExprPtr> value = parseExpr();
    if (failed(value))
      return failure();
    consumeIf(Token::Semicolon);
    branch->lets.push_back({*name, std::move(*value)});
  }
  FailureOr<ExprPtr> value = parseExpr();
  if (failed(value) || failed(expect(Token::RBrace, "'}'")))
    return failure();
  branch->value = std::move(*value);
  return branch;
}

// Component { field: value, ... } or a tag: Component [{}]
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
  auto node = std::make_unique<Expr>();
  node->loc = token.loc;
  switch (token.kind) {
  case Token::Integer: {
    node->kind = Expr::Int;
    if (token.spelling.getAsInteger(10, node->intValue))
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
    if (failed(inner) || failed(expect(Token::RParen, "')'")))
      return failure();
    return inner;
  }
  case Token::Identifier:
    break;
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
    auto elseBranch = parseBranch();
    if (failed(elseBranch))
      return failure();
    node->thenBranch = std::move(*thenBranch);
    node->elseBranch = std::move(*elseBranch);
    return node;
  }
  if (consumeKeyword("spawn")) {
    node->kind = Expr::Spawn;
    if (failed(expect(Token::LBrace, "'{'")))
      return failure();
    do {
      FailureOr<ComponentInit> init = parseComponentInit();
      if (failed(init))
        return failure();
      node->inits.push_back(std::move(*init));
    } while (consumeIf(Token::Comma));
    if (failed(expect(Token::RBrace, "'}'")))
      return failure();
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
    }
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
  case Expr::Float:
    return {};
  case Expr::Bool:
    return builder.getI1Type();
  case Expr::Name: {
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind == Variable::Ref)
        return {};
      if (variable->kind == Variable::Entity)
        return EntityType::get(context);
      return variable->value.getType();
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end() && unique->second.shorthand)
      return unique->second.fieldType("value");
    return {};
  }
  case Expr::Field: {
    if (const Variable *variable = lookup(expr.name))
      if (variable->kind == Variable::Ref)
        return recordOf(*variable).fieldType(expr.field);
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end())
      return unique->second.fieldType(expr.field);
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
    default:
      if (Type type = typeOf(*expr.operands[0]))
        return type;
      return typeOf(*expr.operands[1]);
    }
  case Expr::Call:
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

FailureOr<mlir::Value> Parser::emit(const Expr &expr, Type expected) {
  Location at = loc(expr.loc);
  switch (expr.kind) {
  case Expr::Int: {
    Type type = expected ? expected : builder.getI32Type();
    if (isa<FloatType>(type))
      return arith::ConstantOp::create(
                 builder, at,
                 builder.getFloatAttr(type, double(expr.intValue)))
          .getResult();
    if (!type.isIntOrIndex() || type.isInteger(1))
      return error(expr.loc, "an integer cannot be used here");
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
      return variable->value;
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end()) {
      if (!unique->second.shorthand)
        return error(expr.loc, "unique '" + expr.name +
                                   "' has fields; read one with '" +
                                   expr.name + ".field'");
      return ReadOp::create(builder, at, unique->second.fieldType("value"),
                            symbol(expr.name), builder.getStringAttr("value"))
          .getResult();
    }
    return error(expr.loc, "unknown name '" + expr.name + "'");
  }
  case Expr::Field: {
    if (const Variable *variable = lookup(expr.name)) {
      if (variable->kind != Variable::Ref)
        return error(expr.loc, "'" + expr.name + "' has no fields");
      Type type = recordOf(*variable).fieldType(expr.field);
      if (!type)
        return error(expr.loc, std::string(relations.count(variable->component)
                                               ? "relation '"
                                               : "component '") +
                                   variable->component + "' has no field '" +
                                   expr.field + "'");
      return GetOp::create(builder, at, type, variable->value,
                           builder.getStringAttr(expr.field))
          .getResult();
    }
    auto unique = uniques.find(expr.name);
    if (unique != uniques.end()) {
      Type type = unique->second.fieldType(expr.field);
      if (!type)
        return error(expr.loc, "unique '" + expr.name + "' has no field '" +
                                   expr.field + "'");
      return ReadOp::create(builder, at, type, symbol(expr.name),
                            builder.getStringAttr(expr.field))
          .getResult();
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
    if (expr.name != "min" && expr.name != "max")
      return error(expr.loc, "unknown function '" + expr.name +
                                 "'; functions are not supported yet "
                                 "(min and max are built in)");
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
    Type from = typeOf(*expr.operands[0]);
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
    return emitSpawn(expr);
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
  Type type = typeOf(lhs);
  if (!type)
    type = typeOf(rhs);
  if (!type)
    type = comparison || !expected ? defaultType(expr) : expected;
  FailureOr<mlir::Value> a = emit(lhs, type);
  FailureOr<mlir::Value> b = emit(rhs, type);
  if (failed(a) || failed(b))
    return failure();
  if (a->getType() != b->getType())
    return error(expr.loc, "operands have different types");
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
  if (isa<EntityType>(type))
    return error(expr.loc, "entities cannot be compared yet");
  return arith::CmpIOp::create(builder, at, predicate, *a, *b).getResult();
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
    for (auto &[name, value] : part->lets) {
      FailureOr<mlir::Value> emitted = emit(*value, Type());
      if (failed(emitted))
        return failure();
      bind(name, Variable::ofValue(*emitted));
    }
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
    values.push_back(*emitted);
  }
  for (auto &[name, expr] : init.fields)
    if (!record.fieldType(name))
      return error(expr->loc, "component '" + init.component +
                                  "' has no field '" + name + "'");
  return values;
}

FailureOr<mlir::Value> Parser::emitSpawn(const Expr &expr) {
  SmallVector<Attribute> listed;
  SmallVector<mlir::Value> values;
  for (const ComponentInit &init : expr.inits) {
    FailureOr<SmallVector<mlir::Value>> fields = emitInitValues(init);
    if (failed(fields))
      return failure();
    listed.push_back(symbol(init.component));
    values.append(fields->begin(), fields->end());
  }
  OperationState state(loc(expr.loc), SpawnOp::getOperationName());
  state.addAttribute("components", builder.getArrayAttr(listed));
  state.addOperands(values);
  state.addTypes(EntityType::get(context));
  return builder.create(state)->getResult(0);
}

OwningOpRef<ModuleOp> mlir::ent::importEnt(llvm::SourceMgr &sourceMgr,
                                           MLIRContext *context) {
  context->loadDialect<EntDialect, arith::ArithDialect, scf::SCFDialect>();
  Parser parser(sourceMgr, context);
  return parser.parseModule();
}
