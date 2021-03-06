#ifndef IV_LV5_JSON_LEXER_H_
#define IV_LV5_JSON_LEXER_H_
#include <cassert>
#include <cstdlib>
#include <vector>
#include <string>
#include <iv/token.h>
#include <iv/character.h>
#include <iv/noncopyable.h>
#include <iv/conversions.h>
namespace iv {
namespace lv5 {
namespace detail {

inline bool IsJSONWhiteSpace(char16_t c) {
  return (c == ' ' || c == '\n' || c == '\t' || c == '\r');
}

template<bool AcceptLineTerminator>
inline bool IsAcceptedChar(char16_t c) {
  return c > 0x001F;
}

template<>
inline bool IsAcceptedChar<false>(char16_t c) {
  return c > 0x001F && !core::character::IsLineTerminator(c);
}


}  // namespace detail

template<typename Source, bool AcceptLineTerminator>
class JSONLexer : private core::Noncopyable<> {
 public:
  explicit JSONLexer(const Source& source)
    : source_(source),
      buffer8_(),
      buffer16_(),
      numeric_(),
      pos_(0),
      end_(source.size()),
      c_(-1) {
    Advance();
  }

  core::Token::Type Next() {
    while (c_ >= 0 && detail::IsJSONWhiteSpace(c_)) {
      // white space and line terminator
      Advance();
    }
    switch (c_) {
      case '"':
        // string literal
        return ScanString();

      case ':':
        Advance();
        return core::Token::TK_COLON;

      case ',':
        Advance();
        return core::Token::TK_COMMA;

      case '[':
        Advance();
        return core::Token::TK_LBRACK;
        break;

      case ']':
        Advance();
        return core::Token::TK_RBRACK;

      case '{':
        Advance();
        return core::Token::TK_LBRACE;

      case '}':
        Advance();
        return core::Token::TK_RBRACE;

      case '-':
        Advance();
        return ScanNumber<true>();

      case 'n': {  // null literal
        static const char* literal = "ull";
        Advance();
        return MatchLiteral(literal, 3, core::Token::TK_NULL_LITERAL);
      }

      case 't': {  // true literal
        static const char* literal = "rue";
        Advance();
        return MatchLiteral(literal, 3, core::Token::TK_TRUE_LITERAL);
      }

      case 'f': {  // false literal
        static const char* literal = "alse";
        Advance();
        return MatchLiteral(literal, 4, core::Token::TK_FALSE_LITERAL);
      }

      default:
        if (c_ < 0) {
          // EOS
          return core::Token::TK_EOS;
        } else if (core::character::IsDecimalDigit(c_)) {
          return ScanNumber<false>();
        } else {
          return core::Token::TK_ILLEGAL;
        }
    }
  }

  inline const std::vector<char16_t>& Buffer() const {
    return buffer16_;
  }

  inline const double& Numeric() const {
    return numeric_;
  }

 private:
  inline void Advance() {
    if (pos_ == end_) {
      c_ = -1;
    } else {
      c_ = source_[pos_++];
    }
  }

  inline void Record8() {
    buffer8_.push_back(static_cast<char>(c_));
  }

  inline void Record8(const int ch) {
    buffer8_.push_back(static_cast<char>(ch));
  }

  inline void Record16() {
    buffer16_.push_back(c_);
  }

  inline void Record16(const int ch) {
    buffer16_.push_back(ch);
  }

  inline void Record8Advance() {
    Record8();
    Advance();
  }

  inline void Record16Advance() {
    Record16();
    Advance();
  }

  core::Token::Type MatchLiteral(const char* literal,
                                 std::size_t size,
                                 core::Token::Type token) {
    for (std::size_t i = 0; i < size; ++i) {
      if (c_ != literal[i]) {
        return core::Token::TK_ILLEGAL;
      }
      Advance();
    }
    return token;
  }

  core::Token::Type ScanString() {
    assert(c_ == '"');
    buffer16_.clear();
    Advance();
    while (c_ != '"' && c_ >= 0) {
      if (c_ == '\\') {
        Advance();
        // escape sequence
        if (c_ < 0) {
          return core::Token::TK_ILLEGAL;
        }
        if (!ScanEscape()) {
          return core::Token::TK_ILLEGAL;
        }
      } else if (detail::IsAcceptedChar<AcceptLineTerminator>(c_)) {
        Record16Advance();
      } else {
        return core::Token::TK_ILLEGAL;
      }
    }
    if (c_ != '"') {
      // not closed
      return core::Token::TK_ILLEGAL;
    }
    Advance();
    return core::Token::TK_STRING;
  }

  template<bool find_sign>
  typename core::Token::Type ScanNumber() {
    buffer8_.clear();
    if (find_sign) {
      Advance();
      if (c_ < 0 ||
          !core::character::IsDecimalDigit(c_)) {
        return core::Token::TK_ILLEGAL;
      }
    }
    if (c_ == '0') {
      Record8Advance();
    } else {
      ScanDecimalDigits();
    }
    if (c_ == '.') {
      Record8Advance();
      if (c_ < 0 ||
          !core::character::IsDecimalDigit(c_)) {
        return core::Token::TK_ILLEGAL;
      }
      ScanDecimalDigits();
    }

    // exponent part
    if (c_ == 'e' || c_ == 'E') {
      Record8Advance();
      if (c_ == '+' || c_ == '-') {
        Record8Advance();
      }
      // more than 1 decimal digit required
      if (c_ < 0 ||
          !core::character::IsDecimalDigit(c_)) {
        return core::Token::TK_ILLEGAL;
      }
      ScanDecimalDigits();
    }
    buffer8_.push_back('\0');
    numeric_ = std::atof(buffer8_.data());
    if (find_sign) {
      numeric_ = -numeric_;
    }
    return core::Token::TK_NUMBER;
  }

  void ScanDecimalDigits() {
    while (0 <= c_ && core::character::IsDecimalDigit(c_)) {
      Record8Advance();
    }
  }

  bool ScanEscape() {
    switch (c_) {
      case '"' :
      case '/':
      case '\\':
        Record16Advance();
        break;
      case 'b' :
        Record16('\b');
        Advance();
        break;
      case 'f' :
        Record16('\f');
        Advance();
        break;
      case 'n' :
        Record16('\n');
        Advance();
        break;
      case 'r' :
        Record16('\r');
        Advance();
        break;
      case 't' :
        Record16('\t');
        Advance();
        break;
      case 'u' : {
        Advance();
        char16_t uc = '\0';
        for (int i = 0; i < 4; ++i) {
          const int d = core::HexValue(c_);
          if (d < 0) {
            return false;
          }
          uc = uc * 16 + d;
          Advance();
        }
        Record16(uc);
        break;
      }
      default:
        // in JSON syntax, NonEscapeCharacter not found
        return false;
    }
    return true;
  }

  const Source& source_;
  std::vector<char> buffer8_;
  std::vector<char16_t> buffer16_;
  double numeric_;
  std::size_t pos_;
  const std::size_t end_;
  int c_;
};

} }  // namespace iv::lv5
#endif  // IV_LV5_JSON_LEXER_H_
