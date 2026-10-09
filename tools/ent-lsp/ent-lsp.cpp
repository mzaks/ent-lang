// The language server of ent-lang: what an editor shows of a `.ent` file
// while it is read. It runs the compiler's own front end on every change
// and answers from what that found:
//
// - the errors and warnings of the importer, the verifier, scheduling and
//   lowering, where they are;
// - the outline of a file: its components, uniques, enums, systems, fns,
//   schedules;
// - where a name is declared, also in an imported module;
// - on hovering over a name: its declaration with the comment above it,
//   and for a system what it reads and writes and what it waits for in a
//   schedule.
//
// Modules are found as `tools/ent` finds them: next to the importing file,
// in the `-I` directories, and in the devices that come with the compiler.
// An imported file is read as it is on disk.

#include "Ent/Access.h"
#include "Ent/EntDialect.h"
#include "Ent/EntOps.h"
#include "Ent/Import.h"
#include "Ent/Passes.h"
#include "Ent/Structure.h"

#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LSP/Logging.h"
#include "llvm/Support/LSP/Protocol.h"
#include "llvm/Support/LSP/Transport.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

using namespace mlir;
using namespace mlir::ent;
namespace lsp = llvm::lsp;

namespace {

//===----------------------------------------------------------------------===//
// What the compiler found
//===----------------------------------------------------------------------===//

/// A place in a file; `line` and `column` count from 0, as editors do.
struct Place {
  std::string file;
  int line = 0, column = 0;
};

/// Something a program declares at its top level.
struct Declaration {
  /// As the IR names it: `Position`, or `clock.Time` for a module's.
  std::string symbol;
  /// The keyword it is declared with.
  StringRef keyword;
  lsp::SymbolKind kind;
  Place place;
  /// For a system: what it reads and writes, and what it waits for.
  std::string access;
  std::vector<std::string> waits;
};

/// An error, warning or remark of the compiler.
struct Finding {
  DiagnosticSeverity severity;
  Place place;
  std::string message;
  std::vector<std::pair<Place, std::string>> notes;
};

/// What one run of the front end over a file gave.
struct Analysis {
  bool parsed = false;
  std::vector<Declaration> declarations;
  std::vector<Finding> findings;
};

Place placeOf(Location location) {
  Place place;
  if (auto at = location->findInstanceOf<FileLineColLoc>()) {
    place.file = at.getFilename().str();
    place.line = std::max<int>(at.getLine(), 1) - 1;
    place.column = std::max<int>(at.getColumn(), 1) - 1;
  }
  return place;
}

/// The keyword and the kind of a top-level op, or no keyword for one that
/// is not a declaration of the program's.
std::pair<StringRef, lsp::SymbolKind> describe(Operation *op) {
  if (auto component = dyn_cast<ComponentOp>(op))
    return {component.getFieldNames().empty() ? "tag" : "component",
            lsp::SymbolKind::Struct};
  if (isa<ResourceOp>(op))
    return {"unique", lsp::SymbolKind::Variable};
  if (isa<BufferOp>(op))
    return {"buffer", lsp::SymbolKind::Array};
  if (isa<RelationOp>(op))
    return {"relation", lsp::SymbolKind::Interface};
  if (isa<ArchetypeOp>(op))
    return {"archetype", lsp::SymbolKind::Class};
  if (isa<EnumOp>(op))
    return {"enum", lsp::SymbolKind::Enum};
  if (isa<SystemOp>(op))
    return {"system", lsp::SymbolKind::Function};
  if (isa<ExternOp>(op))
    return {"extern system", lsp::SymbolKind::Function};
  if (auto function = dyn_cast<FunctionOp>(op))
    return {function.isDefined() ? "fn"
                                 : function.getProc() ? "extern proc"
                                                      : "extern fn",
            lsp::SymbolKind::Method};
  if (isa<ScheduleOp>(op))
    return {"schedule", lsp::SymbolKind::Event};
  if (isa<MainOp>(op))
    return {"main", lsp::SymbolKind::Namespace};
  return {StringRef(), lsp::SymbolKind::Null};
}

/// What a system reads or writes, for a reader: the fields by the
/// component or unique they belong to, whichever archetypes hold them
/// (`Ghost.{col, row}`, `Score`), and, of what it writes, the archetypes
/// whose entities it makes or takes away.
std::string summarize(const llvm::SetVector<Column> &columns, bool writes) {
  std::vector<std::pair<std::string, std::vector<std::string>>> groups;
  std::vector<std::string> archetypes;
  auto add = [](std::vector<std::string> &list, StringRef item) {
    if (!llvm::is_contained(list, item))
      list.push_back(item.str());
  };
  for (const Column &column : columns) {
    auto [archetype, component, field] = column;
    if (component.getValue().empty()) {
      // An archetype's count and ids: who is there.
      if (archetype && writes)
        add(archetypes, archetype.getValue());
      continue;
    }
    StringRef name = component.getValue();
    auto group = llvm::find_if(groups,
                               [&](auto &entry) { return entry.first == name; });
    if (group == groups.end()) {
      groups.push_back({name.str(), {}});
      group = std::prev(groups.end());
    }
    StringRef part = field.getValue();
    add(group->second, part.empty() ? (archetype ? "is there" : "edges")
                           : part == "+"   ? "added"
                             : part == "-" ? "removed"
                                           : part);
  }
  std::string text;
  for (auto &[name, fields] : groups) {
    text += text.empty() ? "" : ", ";
    // A unique written `unique Score: i32` is its one value.
    if (fields.size() == 1 && fields.front() == "value")
      text += "`" + name + "`";
    else if (fields.size() == 1)
      text += "`" + name + "." + fields.front() + "`";
    else
      text += "`" + name + ".{" + llvm::join(fields, ", ") + "}`";
  }
  if (!archetypes.empty())
    text += std::string(text.empty() ? "" : "; ") + "the entities of `" +
            llvm::join(archetypes, "`, `") + "`";
  return text.empty() ? std::string("nothing") : text;
}

/// Run the front end over `text`, the contents of the file `path`.
Analysis analyze(StringRef path, StringRef text,
                 ArrayRef<std::string> directories) {
  Analysis analysis;
  MLIRContext context;
  // Remarks are answers to what the server asked (who waits for whom);
  // the rest is for the reader.
  llvm::StringMap<std::vector<std::string>> waits;
  // A file without `main` is a module for programs to import: which
  // entities there are, and what else writes what it reacts to, is theirs
  // to say, and no finding here.
  bool library = false;
  context.getDiagEngine().registerHandler([&](Diagnostic &diagnostic) {
    std::string message = diagnostic.str();
    if (library && diagnostic.getSeverity() == DiagnosticSeverity::Warning &&
        (StringRef(message).starts_with("matches no archetype") ||
         StringRef(message).contains(", but no system ")))
      return success();
    if (diagnostic.getSeverity() == DiagnosticSeverity::Remark) {
      StringRef rest = message;
      if (rest.consume_front("@") && rest.contains(" waits for "))
        waits[rest.split(' ').first].push_back(message);
      return success();
    }
    Finding finding{diagnostic.getSeverity(),
                    placeOf(diagnostic.getLocation()), std::move(message), {}};
    // Not the IR the compiler adds to what it reports about an op.
    for (Diagnostic &note : diagnostic.getNotes())
      if (!StringRef(note.str()).starts_with("see current operation"))
        finding.notes.push_back({placeOf(note.getLocation()), note.str()});
    // Once, though more than one pass may find it.
    for (const Finding &known : analysis.findings)
      if (known.message == finding.message &&
          known.place.file == finding.place.file &&
          known.place.line == finding.place.line &&
          known.place.column == finding.place.column)
        return success();
    analysis.findings.push_back(std::move(finding));
    return success();
  });

  llvm::SourceMgr sourceMgr;
  sourceMgr.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(text, path),
                               llvm::SMLoc());
  OwningOpRef<ModuleOp> module = importEnt(sourceMgr, &context, directories);
  if (!module)
    return analysis;
  analysis.parsed = true;
  library = module->getOps<MainOp>().empty();

  // What scheduling and lowering say, on a copy: they take the program
  // apart.
  {
    OwningOpRef<ModuleOp> copy(cast<ModuleOp>(module->getOperation()->clone()));
    PassManager passes(&context);
    passes.addPass(createEntSchedule(EntScheduleOptions{/*explain=*/true}));
    passes.addPass(createEntLowerToLoops());
    (void)passes.run(*copy);
  }

  bool inferred = succeeded(inferArchetypes(*module));
  SmallVector<ArchetypeOp> archetypes(module->getOps<ArchetypeOp>());
  for (Operation &op : module->getOps()) {
    auto [keyword, kind] = describe(&op);
    if (keyword.empty())
      continue;
    Declaration declaration;
    declaration.keyword = keyword;
    declaration.kind = kind;
    declaration.place = placeOf(op.getLoc());
    if (auto name = op.getAttrOfType<StringAttr>(
            SymbolTable::getSymbolAttrName()))
      declaration.symbol = name.getValue().str();
    else
      declaration.symbol = "main";
    // An archetype the compiler made up has no place in the source.
    if (declaration.place.file.empty())
      continue;
    if (inferred && isa<SystemOp, ExternOp>(op)) {
      SystemAccess access = computeAccess(&op, archetypes);
      if (access.opaqueOp == &op) {
        declaration.access = "Declares no access, so it conflicts with every "
                             "other system.";
      } else {
        declaration.access = "**reads** " + summarize(access.reads, false) +
                             "\n\n**writes** " +
                             summarize(access.writes, true);
        if (access.isOpaque()) {
          Place where = placeOf(access.opaqueOp->getLoc());
          declaration.access +=
              ("\n\nActs on the outside (" +
               llvm::sys::path::filename(where.file) + ":" +
               Twine(where.line + 1) +
               "), so it keeps its place among all other systems.")
                  .str();
        }
      }
      auto waiting = waits.find(declaration.symbol);
      if (waiting != waits.end())
        declaration.waits = waiting->second;
    }
    analysis.declarations.push_back(std::move(declaration));
  }
  return analysis;
}

//===----------------------------------------------------------------------===//
// Source text
//===----------------------------------------------------------------------===//

std::vector<StringRef> linesOf(StringRef text) {
  std::vector<StringRef> lines;
  while (true) {
    auto [line, rest] = text.split('\n');
    lines.push_back(line.rtrim('\r'));
    if (rest.empty() && !text.contains('\n'))
      break;
    text = rest;
  }
  return lines;
}

bool isNameChar(char c) { return llvm::isAlnum(c) || c == '_'; }

/// The name at a column of a line, with its module if written `m::Name`,
/// and the columns it spans.
struct Word {
  std::string name;
  int begin = 0, end = 0;
};
std::optional<Word> wordAt(StringRef line, int column) {
  int size = line.size();
  if (column > size)
    return std::nullopt;
  int begin = column, end = column;
  while (begin > 0 && isNameChar(line[begin - 1]))
    --begin;
  while (end < size && isNameChar(line[end]))
    ++end;
  if (begin == end || llvm::isDigit(line[begin]))
    return std::nullopt;
  Word word{line.substr(begin, end - begin).str(), begin, end};
  // `m::Name`: the module belongs to the name, whichever part is pointed at.
  if (begin >= 2 && line.substr(begin - 2, 2) == "::") {
    int start = begin - 2;
    while (start > 0 && isNameChar(line[start - 1]))
      --start;
    word.name = line.substr(start, begin - 2 - start).str() + "." + word.name;
  }
  return word;
}

/// The comment right above a declaration and the declaration itself: up
/// to its body for what has one, whole for the rest.
std::pair<std::string, std::string> excerpt(const std::vector<StringRef> &lines,
                                            const Declaration &declaration) {
  int line = declaration.place.line;
  if (line >= int(lines.size()))
    return {};
  std::string comment;
  int first = line;
  while (first > 0 && lines[first - 1].ltrim().starts_with("//"))
    --first;
  for (int i = first; i < line; ++i) {
    StringRef text = lines[i].ltrim().drop_front(2);
    text.consume_front(" ");
    comment += text.str() + "\n";
  }
  bool hasBody = declaration.keyword == "system" ||
                 declaration.keyword == "fn" ||
                 declaration.keyword == "schedule" ||
                 declaration.keyword == "main";
  std::string code;
  int depth = 0;
  for (int i = line, e = std::min<int>(lines.size(), line + 40); i < e; ++i) {
    StringRef text = lines[i];
    bool done = false;
    size_t taken = 0;
    for (; taken < text.size() && !done; ++taken) {
      char c = text[taken];
      if (c == '/' && text.substr(taken).starts_with("//"))
        break;
      if (c == '(' || c == '[')
        ++depth;
      else if (c == ')' || c == ']')
        --depth;
      else if (c == '{') {
        if (hasBody && depth == 0)
          done = true;
        else
          ++depth;
      } else if (c == '}')
        --depth;
    }
    if (done) {
      code += text.substr(0, taken - 1).rtrim().str();
      break;
    }
    code += text.rtrim().str();
    if (depth <= 0)
      break;
    code += "\n";
  }
  return {comment, code};
}

//===----------------------------------------------------------------------===//
// The server
//===----------------------------------------------------------------------===//

struct Document {
  std::string text;
  int64_t version = 0;
  /// The last analysis, and the last that got through the importer: while
  /// a file does not parse, one still finds one's way by the old one.
  Analysis latest, parsed;
};

class Server {
public:
  Server(std::vector<std::string> directories)
      : directories(std::move(directories)) {}

  void onInitialize(const lsp::InitializeParams &params,
                    lsp::Callback<llvm::json::Value> reply);
  void onInitialized(const lsp::InitializedParams &) {}
  void onShutdown(const lsp::NoParams &, lsp::Callback<std::nullptr_t> reply) {
    shutdownRequested = true;
    reply(nullptr);
  }
  void onOpen(const lsp::DidOpenTextDocumentParams &params);
  void onChange(const lsp::DidChangeTextDocumentParams &params);
  void onClose(const lsp::DidCloseTextDocumentParams &params);
  void onSymbols(const lsp::DocumentSymbolParams &params,
                 lsp::Callback<std::vector<lsp::DocumentSymbol>> reply);
  void onDefinition(const lsp::TextDocumentPositionParams &params,
                    lsp::Callback<std::vector<lsp::Location>> reply);
  void onHover(const lsp::TextDocumentPositionParams &params,
               lsp::Callback<std::optional<lsp::Hover>> reply);

  lsp::OutgoingNotification<lsp::PublishDiagnosticsParams> publish;
  bool shutdownRequested = false;

private:
  void update(const lsp::URIForFile &uri, int64_t version);
  const Declaration *find(const Document &document,
                          const lsp::TextDocumentPositionParams &params,
                          Word &word);
  std::vector<StringRef> sourceOf(StringRef file, StringRef documentFile,
                                  const Document &document,
                                  std::string &storage);

  std::vector<std::string> directories;
  llvm::StringMap<Document> documents;
};

lsp::Range rangeAt(const Place &place, int length = 0) {
  return lsp::Range(lsp::Position(place.line, place.column),
                    lsp::Position(place.line, place.column + length));
}

std::optional<lsp::Location> locationOf(const Place &place, int length = 0) {
  SmallString<256> path(place.file);
  llvm::sys::fs::make_absolute(path);
  llvm::Expected<lsp::URIForFile> uri = lsp::URIForFile::fromFile(path);
  if (!uri) {
    llvm::consumeError(uri.takeError());
    return std::nullopt;
  }
  return lsp::Location(*uri, rangeAt(place, length));
}

/// The name of a declaration as the source writes it.
StringRef written(const Declaration &declaration) {
  StringRef symbol = declaration.symbol;
  return symbol.contains('.') ? symbol.rsplit('.').second : symbol;
}

void Server::onInitialize(const lsp::InitializeParams &,
                          lsp::Callback<llvm::json::Value> reply) {
  llvm::json::Object capabilities{
      {"textDocumentSync",
       llvm::json::Object{
           {"openClose", true},
           {"change", int(lsp::TextDocumentSyncKind::Incremental)},
           {"save", true},
       }},
      {"documentSymbolProvider", true},
      {"definitionProvider", true},
      {"hoverProvider", true},
  };
  reply(llvm::json::Object{
      {"serverInfo",
       llvm::json::Object{{"name", "ent-lsp"}, {"version", "0.1"}}},
      {"capabilities", std::move(capabilities)}});
}

void Server::update(const lsp::URIForFile &uri, int64_t version) {
  Document &document = documents[uri.file()];
  document.version = version;
  document.latest = analyze(uri.file(), document.text, directories);
  if (document.latest.parsed)
    document.parsed = document.latest;

  lsp::PublishDiagnosticsParams params(uri, version);
  for (const Finding &finding : document.latest.findings) {
    lsp::Diagnostic diagnostic;
    diagnostic.source = "ent";
    diagnostic.severity =
        finding.severity == DiagnosticSeverity::Error
            ? lsp::DiagnosticSeverity::Error
            : finding.severity == DiagnosticSeverity::Warning
                  ? lsp::DiagnosticSeverity::Warning
                  : lsp::DiagnosticSeverity::Information;
    diagnostic.message = finding.message;
    // Up to the end of the line: the compiler names where it starts.
    diagnostic.range = rangeAt(finding.place);
    bool here = finding.place.file == uri.file();
    if (here) {
      std::vector<StringRef> lines = linesOf(document.text);
      if (finding.place.line < int(lines.size()))
        diagnostic.range.end.character = std::max<int>(
            lines[finding.place.line].size(), finding.place.column + 1);
    } else {
      // Found in a module this file imports: said at the top of this one.
      diagnostic.range = lsp::Range(lsp::Position(0, 0), lsp::Position(0, 1));
      if (!finding.place.file.empty())
        diagnostic.message =
            (llvm::sys::path::filename(finding.place.file) + ":" +
             Twine(finding.place.line + 1) + ": " + finding.message)
                .str();
    }
    for (const auto &[place, message] : finding.notes) {
      std::optional<lsp::Location> location = locationOf(place);
      if (!location)
        continue;
      if (!diagnostic.relatedInformation)
        diagnostic.relatedInformation.emplace();
      diagnostic.relatedInformation->emplace_back(*location, message);
    }
    params.diagnostics.push_back(std::move(diagnostic));
  }
  publish(params);
}

void Server::onOpen(const lsp::DidOpenTextDocumentParams &params) {
  documents[params.textDocument.uri.file()].text = params.textDocument.text;
  update(params.textDocument.uri, params.textDocument.version);
}

void Server::onChange(const lsp::DidChangeTextDocumentParams &params) {
  Document &document = documents[params.textDocument.uri.file()];
  if (failed(lsp::TextDocumentContentChangeEvent::applyTo(
          params.contentChanges, document.text)))
    lsp::Logger::error("a change to {0} could not be applied",
                       params.textDocument.uri.file());
  update(params.textDocument.uri, params.textDocument.version);
}

void Server::onClose(const lsp::DidCloseTextDocumentParams &params) {
  documents.erase(params.textDocument.uri.file());
  publish(lsp::PublishDiagnosticsParams(params.textDocument.uri, 0));
}

void Server::onSymbols(const lsp::DocumentSymbolParams &params,
                       lsp::Callback<std::vector<lsp::DocumentSymbol>> reply) {
  std::vector<lsp::DocumentSymbol> symbols;
  auto found = documents.find(params.textDocument.uri.file());
  if (found == documents.end())
    return reply(std::move(symbols));
  const Document &document = found->second;
  int lastLine = std::max<int>(linesOf(document.text).size(), 1) - 1;
  // Its own, and not what the compiler makes of `world`; in the order they
  // stand in the file.
  std::vector<const Declaration *> own;
  for (const Declaration &declaration : document.parsed.declarations)
    if (declaration.place.file == params.textDocument.uri.file() &&
        declaration.symbol != "world_init")
      own.push_back(&declaration);
  llvm::sort(own, [](const Declaration *a, const Declaration *b) {
    return std::tie(a->place.line, a->place.column) <
           std::tie(b->place.line, b->place.column);
  });
  for (const Declaration *declaration : own) {
    StringRef name = declaration->symbol == "world_setup"
                         ? StringRef("world")
                         : written(*declaration);
    lsp::Range selection = rangeAt(declaration->place, name.size());
    symbols.emplace_back(name, declaration->kind, selection, selection);
    symbols.back().detail = declaration->keyword.str();
  }
  // A declaration lasts until the next one starts.
  std::vector<StringRef> lines = linesOf(document.text);
  for (size_t i = 0; i < symbols.size(); ++i) {
    int end = i + 1 < symbols.size()
                  ? symbols[i + 1].selectionRange.start.line - 1
                  : lastLine;
    end = std::max(end, symbols[i].selectionRange.end.line);
    symbols[i].range.start.character = 0;
    symbols[i].range.end = lsp::Position(
        end, std::max<int>(end < int(lines.size()) ? lines[end].size() : 0,
                           end == symbols[i].selectionRange.end.line
                               ? symbols[i].selectionRange.end.character
                               : 0));
  }
  reply(std::move(symbols));
}

/// The declaration the name at a position stands for: this file's own
/// before an imported module's, a module's if the name says so.
const Declaration *Server::find(const Document &document,
                                const lsp::TextDocumentPositionParams &params,
                                Word &word) {
  std::vector<StringRef> lines = linesOf(document.text);
  if (params.position.line >= int(lines.size()))
    return nullptr;
  std::optional<Word> at =
      wordAt(lines[params.position.line], params.position.character);
  if (!at)
    return nullptr;
  word = *at;
  const Declaration *imported = nullptr;
  for (const Declaration &declaration : document.parsed.declarations) {
    if (declaration.symbol == word.name)
      return &declaration;
    if (!imported && !StringRef(word.name).contains('.') &&
        StringRef(declaration.symbol).ends_with("." + word.name))
      imported = &declaration;
  }
  return imported;
}

void Server::onDefinition(const lsp::TextDocumentPositionParams &params,
                          lsp::Callback<std::vector<lsp::Location>> reply) {
  std::vector<lsp::Location> locations;
  auto found = documents.find(params.textDocument.uri.file());
  Word word;
  if (found != documents.end())
    if (const Declaration *declaration = find(found->second, params, word))
      if (auto location =
              locationOf(declaration->place, written(*declaration).size()))
        locations.push_back(*location);
  reply(std::move(locations));
}

/// The lines of a file: of the open document, or as it is on disk.
std::vector<StringRef> Server::sourceOf(StringRef file, StringRef documentFile,
                                        const Document &document,
                                        std::string &storage) {
  if (file == documentFile)
    return linesOf(document.text);
  auto open = documents.find(file);
  if (open != documents.end())
    return linesOf(open->second.text);
  if (auto buffer = llvm::MemoryBuffer::getFile(file))
    storage = (*buffer)->getBuffer().str();
  return linesOf(storage);
}

void Server::onHover(const lsp::TextDocumentPositionParams &params,
                     lsp::Callback<std::optional<lsp::Hover>> reply) {
  auto found = documents.find(params.textDocument.uri.file());
  Word word;
  const Declaration *declaration =
      found == documents.end() ? nullptr : find(found->second, params, word);
  if (!declaration)
    return reply(std::nullopt);

  std::string storage;
  std::vector<StringRef> lines =
      sourceOf(declaration->place.file, params.textDocument.uri.file(),
               found->second, storage);
  auto [comment, code] = excerpt(lines, *declaration);
  lsp::Hover hover(lsp::Range(
      lsp::Position(params.position.line, word.begin),
      lsp::Position(params.position.line, word.end)));
  std::string &text = hover.contents.value;
  if (!code.empty())
    text += "```ent\n" + code + "\n```\n";
  StringRef symbol = declaration->symbol;
  if (symbol.contains('.'))
    text += ("From the module `" + symbol.rsplit('.').first + "`.\n\n").str();
  if (!comment.empty())
    text += comment + "\n";
  if (!declaration->access.empty())
    text += "---\n" + declaration->access + "\n";
  for (const std::string &wait : declaration->waits)
    text += "\nIn a schedule, " + wait + ".\n";
  reply(std::move(hover));
}

} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  llvm::cl::opt<bool> litTest(
      "lit-test",
      llvm::cl::desc("Read messages separated by lines of '// -----', and "
                     "write the answers indented: for tests"));
  llvm::cl::opt<lsp::Logger::Level> logLevel(
      "log", llvm::cl::desc("How much to say on standard error"),
      llvm::cl::values(
          clEnumValN(lsp::Logger::Level::Error, "error", "Errors only"),
          clEnumValN(lsp::Logger::Level::Info, "info", "What it does"),
          clEnumValN(lsp::Logger::Level::Debug, "verbose", "Everything")),
      llvm::cl::init(lsp::Logger::Level::Error));
  llvm::cl::ParseCommandLineOptions(argc, argv, "ent-lang language server\n");
  lsp::Logger::setLogLevel(logLevel);

  // The devices that come with the compiler: next to the build directory
  // (build/bin/ent-lsp and devices/), or where ENT_DEVICES says.
  std::vector<std::string> directories;
  if (const char *devices = std::getenv("ENT_DEVICES")) {
    directories.push_back(devices);
  } else {
    SmallString<256> path(
        llvm::sys::fs::getMainExecutable(argv[0], (void *)&main));
    llvm::sys::path::remove_filename(path);
    llvm::sys::path::append(path, "..", "..", "devices");
    llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
    if (llvm::sys::fs::is_directory(path))
      directories.push_back(std::string(path));
  }

  // A test's files are nowhere on disk.
  if (litTest)
    lsp::URIForFile::registerSupportedScheme("test");
  llvm::sys::ChangeStdinToBinary();
  lsp::JSONTransport transport(stdin, llvm::outs(),
                               litTest ? lsp::JSONStreamStyle::Delimited
                                       : lsp::JSONStreamStyle::Standard,
                               /*PrettyOutput=*/litTest);
  Server server(std::move(directories));
  lsp::MessageHandler handler(transport);
  handler.method("initialize", &server, &Server::onInitialize);
  handler.notification("initialized", &server, &Server::onInitialized);
  handler.method("shutdown", &server, &Server::onShutdown);
  handler.notification("textDocument/didOpen", &server, &Server::onOpen);
  handler.notification("textDocument/didChange", &server, &Server::onChange);
  handler.notification("textDocument/didClose", &server, &Server::onClose);
  handler.method("textDocument/documentSymbol", &server, &Server::onSymbols);
  handler.method("textDocument/definition", &server, &Server::onDefinition);
  handler.method("textDocument/hover", &server, &Server::onHover);
  server.publish = handler.outgoingNotification<lsp::PublishDiagnosticsParams>(
      "textDocument/publishDiagnostics");

  if (llvm::Error error = transport.run(handler)) {
    lsp::Logger::error("transport error: {0}", error);
    llvm::consumeError(std::move(error));
    return 1;
  }
  return server.shutdownRequested ? 0 : 1;
}
