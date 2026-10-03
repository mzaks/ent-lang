#ifndef ENT_IMPORT_LEXER_H
#define ENT_IMPORT_LEXER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"

namespace mlir::ent {

/// One token of ent-lang source. Keywords are identifiers; the parser
/// decides where a word is one.
struct Token {
  enum Kind {
    Eof,
    Error,
    Identifier,
    Integer,
    Float,
    String, // "..." with its quotes
    Char,   // 'a' with its quotes
    // Punctuation and operators.
    LBrace,
    RBrace,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Semicolon,
    Dot,
    DotDot, // ..
    Assign,     // =
    PlusAssign, // +=
    MinusAssign,
    StarAssign,
    SlashAssign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,    // ==
    NotEqual, // !=
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Not,
    AndAnd,
    OrOr,
  };
  Kind kind = Eof;
  llvm::StringRef spelling;
  llvm::SMLoc loc;

  bool is(Kind k) const { return kind == k; }
  bool isKeyword(llvm::StringRef word) const {
    return kind == Identifier && spelling == word;
  }
};

/// Splits ent-lang source into tokens; `//` starts a comment to the end of
/// the line.
class Lexer {
public:
  explicit Lexer(llvm::StringRef buffer)
      : buffer(buffer), current(buffer.begin()) {}

  Token next();

private:
  Token make(Token::Kind kind, const char *start) {
    return {kind, llvm::StringRef(start, current - start),
            llvm::SMLoc::getFromPointer(start)};
  }

  llvm::StringRef buffer;
  const char *current;
};

} // namespace mlir::ent

#endif // ENT_IMPORT_LEXER_H
