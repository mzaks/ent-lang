#include "Lexer.h"

#include "llvm/ADT/StringExtras.h"

using namespace mlir::ent;

Token Lexer::next() {
  // Whitespace and comments.
  while (current != buffer.end()) {
    if (llvm::isSpace(*current)) {
      ++current;
    } else if (*current == '/' && current + 1 != buffer.end() &&
               current[1] == '/') {
      while (current != buffer.end() && *current != '\n')
        ++current;
    } else {
      break;
    }
  }
  const char *start = current;
  if (current == buffer.end())
    return make(Token::Eof, start);

  char c = *current++;
  if (llvm::isAlpha(c) || c == '_') {
    while (current != buffer.end() &&
           (llvm::isAlnum(*current) || *current == '_'))
      ++current;
    return make(Token::Identifier, start);
  }
  if (llvm::isDigit(c)) {
    while (current != buffer.end() && llvm::isDigit(*current))
      ++current;
    bool isFloat = false;
    if (current != buffer.end() && *current == '.' &&
        current + 1 != buffer.end() && llvm::isDigit(current[1])) {
      isFloat = true;
      ++current;
      while (current != buffer.end() && llvm::isDigit(*current))
        ++current;
    }
    if (current != buffer.end() && (*current == 'e' || *current == 'E')) {
      const char *exponent = current + 1;
      if (exponent != buffer.end() && (*exponent == '+' || *exponent == '-'))
        ++exponent;
      if (exponent != buffer.end() && llvm::isDigit(*exponent)) {
        isFloat = true;
        current = exponent;
        while (current != buffer.end() && llvm::isDigit(*current))
          ++current;
      }
    }
    return make(isFloat ? Token::Float : Token::Integer, start);
  }

  auto followedBy = [&](char next) {
    if (current != buffer.end() && *current == next) {
      ++current;
      return true;
    }
    return false;
  };
  switch (c) {
  case '{':
    return make(Token::LBrace, start);
  case '}':
    return make(Token::RBrace, start);
  case '(':
    return make(Token::LParen, start);
  case ')':
    return make(Token::RParen, start);
  case ',':
    return make(Token::Comma, start);
  case ':':
    return make(Token::Colon, start);
  case ';':
    return make(Token::Semicolon, start);
  case '.':
    return make(Token::Dot, start);
  case '=':
    return make(followedBy('=') ? Token::Equal : Token::Assign, start);
  case '+':
    return make(followedBy('=') ? Token::PlusAssign : Token::Plus, start);
  case '-':
    return make(followedBy('=') ? Token::MinusAssign : Token::Minus, start);
  case '*':
    return make(followedBy('=') ? Token::StarAssign : Token::Star, start);
  case '/':
    return make(followedBy('=') ? Token::SlashAssign : Token::Slash, start);
  case '%':
    return make(Token::Percent, start);
  case '!':
    return make(followedBy('=') ? Token::NotEqual : Token::Not, start);
  case '<':
    return make(followedBy('=') ? Token::LessEqual : Token::Less, start);
  case '>':
    return make(followedBy('=') ? Token::GreaterEqual : Token::Greater,
                start);
  case '&':
    if (followedBy('&'))
      return make(Token::AndAnd, start);
    break;
  case '|':
    if (followedBy('|'))
      return make(Token::OrOr, start);
    break;
  default:
    break;
  }
  return make(Token::Error, start);
}
