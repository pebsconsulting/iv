#include <cmath>
#include <iv/detail/cstdint.h>
#include <iv/noncopyable.h>
#include <iv/character.h>
#include <iv/conversions.h>
#include <iv/unicode.h>
#include <iv/platform_math.h>
#include <iv/lv5/error_check.h>
#include <iv/lv5/constructor_check.h>
#include <iv/lv5/arguments.h>
#include <iv/lv5/jsval.h>
#include <iv/lv5/jsobject.h>
#include <iv/lv5/jsstring.h>
#include <iv/lv5/jsstring_builder.h>
#include <iv/lv5/error.h>
#include <iv/lv5/context.h>
#include <iv/lv5/internal.h>
namespace iv {
namespace lv5 {
namespace runtime {
namespace detail {

inline bool IsURIMark(char16_t ch) {
  return
      ch <= 126 &&
      (ch == 33 || ((39 <= ch) &&
                    ((ch <= 42) || ((45 <= ch) &&
                                    ((ch <= 46) || ch == 95 || ch == 126)))));
}

inline bool IsURIReserved(char16_t ch) {
  return
      ch <= 64 &&
      (ch == 36 || ch == 38 || ((43 <= ch) &&
                                ((ch <= 44) || ch == 47 ||
                                 ((58 <= ch &&
                                   ((ch <= 59) || ch == 61 || (63 <= ch)))))));
}

inline bool IsEscapeTarget(char16_t ch) {
  return
      '@' == ch ||
      '*' == ch ||
      '_' == ch ||
      '+' == ch ||
      '-' == ch ||
      '.' == ch ||
      '/' == ch;
}

class URIComponent : core::Noncopyable<> {
 public:
  static bool ContainsInEncode(char16_t ch) {
    return core::character::IsASCII(ch) &&
        (core::character::IsASCIIAlphanumeric(ch) || IsURIMark(ch));
  }
  static bool ContainsInDecode(char16_t ch) {
    // Empty String
    return false;
  }
};

class URI : core::Noncopyable<> {
 public:
  static bool ContainsInEncode(char16_t ch) {
    return core::character::IsASCII(ch) &&
        (core::character::IsASCIIAlphanumeric(ch) ||
         IsURIMark(ch) ||
         ch == '#' ||
         IsURIReserved(ch));
  }
  static bool ContainsInDecode(char16_t ch) {
    return IsURIReserved(ch) || ch == '#';
  }
};

class Escape : core::Noncopyable<> {
 public:
  static bool ContainsInEncode(char16_t ch) {
    return core::character::IsASCII(ch) &&
        (core::character::IsASCIIAlphanumeric(ch) ||
         IsEscapeTarget(ch));
  }
  static bool ContainsInDecode(char16_t ch) {
    return IsURIReserved(ch) || ch == '#';
  }
};

template<typename URITraits, typename FiberType>
JSVal Encode(Context* ctx, const FiberType* fiber, Error* e) {
  static const char kHexDigits[17] = "0123456789ABCDEF";
  std::array<uint8_t, 4> uc8buf;
  std::array<char16_t, 3> hexbuf;
  JSStringBuilder builder;
  hexbuf[0] = '%';
  for (typename FiberType::const_iterator it = fiber->begin(),
       last = fiber->end(); it != last; ++it) {
    const char16_t ch = *it;
    if (URITraits::ContainsInEncode(ch)) {
      builder.Append(ch);
    } else {
      uint32_t v;
      if (core::unicode::IsLowSurrogate(ch)) {
        // ch is low surrogate. but high is not found.
        e->Report(Error::URI, "invalid uri char");
        return JSUndefined;
      }
      if (!core::unicode::IsHighSurrogate(ch)) {
        // ch is not surrogate pair code point
        v = ch;
      } else {
        // ch is high surrogate
        ++it;
        if (it == last) {
          // high surrogate only is invalid
          e->Report(Error::URI, "invalid uri char");
          return JSUndefined;
        }
        const char16_t k_char = *it;
        if (!core::unicode::IsLowSurrogate(k_char)) {
          // k_char is not low surrogate
          e->Report(Error::URI, "invalid uri char");
          return JSUndefined;
        }
        // construct surrogate pair to ucs4
        v = (ch - core::unicode::kHighSurrogateMin) * 0x400 +
            (k_char - core::unicode::kLowSurrogateMin) + 0x10000;
      }
      for (std::size_t len =
           core::unicode::UCS4OneCharToUTF8(v, uc8buf.begin()), i = 0;
           i < len; ++i) {
        hexbuf[1] = kHexDigits[uc8buf[i] >> 4];
        hexbuf[2] = kHexDigits[uc8buf[i] & 0xf];
        builder.Append(hexbuf.begin(), 3);
      }
    }
  }
  // always in ASCII range
  return builder.Build(ctx, true, e);
}

template<typename URITraits, typename FiberType>
JSVal Decode(Context* ctx, const FiberType* fiber, Error* e) {
  JSStringBuilder builder;
  const uint32_t length = fiber->size();
  std::array<char16_t, 3> buf;
  std::array<uint8_t, 4> octets;
  buf[0] = '%';
  for (uint32_t k = 0; k < length; ++k) {
    const char16_t ch = (*fiber)[k];
    if (ch != '%') {
      builder.Append(ch);
    } else {
      const uint32_t start = k;
      if (k + 2 >= length) {
        e->Report(Error::URI, "invalid uri char");
        return JSUndefined;
      }
      buf[1] = (*fiber)[k+1];
      buf[2] = (*fiber)[k+2];
      k += 2;
      if ((!core::character::IsHexDigit(buf[1])) ||
          (!core::character::IsHexDigit(buf[2]))) {
        e->Report(Error::URI, "invalid uri char");
        return JSUndefined;
      }
      const uint8_t b0 = core::HexValue(buf[1]) * 16 + core::HexValue(buf[2]);
      if (!(b0 & 0x80)) {
        // b0 is 0xxxxxxx, in ascii range
        if (URITraits::ContainsInDecode(b0)) {
          builder.Append(buf.begin(), 3);
        } else {
          builder.Append(static_cast<char16_t>(b0));
        }
      } else {
        // b0 is 1xxxxxxx
        std::size_t n = 1;
        while (b0 & (0x80 >> n)) {
          ++n;
        }
        if (n == 1 || n > 4) {
          e->Report(Error::URI, "invalid uri char");
          return JSUndefined;
        }
        octets[0] = b0;
        if (k + (3 * (n - 1)) >= length) {
          e->Report(Error::URI, "invalid uri char");
          return JSUndefined;
        }
        for (std::size_t j = 1; j < n; ++j) {
          ++k;
          if ((*fiber)[k] != '%') {
            e->Report(Error::URI, "invalid uri char");
            return JSUndefined;
          }
          buf[1] = (*fiber)[k+1];
          buf[2] = (*fiber)[k+2];
          k += 2;
          if ((!core::character::IsHexDigit(buf[1])) ||
              (!core::character::IsHexDigit(buf[2]))) {
            e->Report(Error::URI, "invalid uri char");
            return JSUndefined;
          }
          const uint8_t b1 =
              core::HexValue(buf[1]) * 16 + core::HexValue(buf[2]);
          // section 15.1.3 decoding 4-b-7-7-e
          // code point 10xxxxxx check is moved to core::UTF8ToUCS4Strict
          octets[j] = b1;
        }
        core::unicode::UTF8Error err = core::unicode::UNICODE_NO_ERROR;
        uint32_t v;
        core::unicode::NextUCS4FromUTF8(octets.begin(), octets.end(), &v, &err);
        // if octets utf8 sequence is not valid,
        if (err != core::unicode::UNICODE_NO_ERROR) {
          e->Report(Error::URI, "invalid uri char");
          return JSUndefined;
        }
        if (v < 0x10000) {
          // not surrogate pair
          const char16_t code = static_cast<char16_t>(v);
          if (URITraits::ContainsInDecode(code)) {
            builder.Append(fiber->data() + start, (k - start + 1));
          } else {
            builder.Append(code);
          }
        } else {
          // surrogate pair
          v -= 0x10000;
          const char16_t L =
              (v & core::unicode::kLowSurrogateMask) +
              core::unicode::kLowSurrogateMin;
          const char16_t H =
              ((v >> core::unicode::kSurrogateBits) &
               core::unicode::kHighSurrogateMask) +
              core::unicode::kHighSurrogateMin;
          builder.Append(H);
          builder.Append(L);
        }
      }
    }
  }
  return builder.Build(ctx, false, e);
}

template<typename FiberType>
JSVal EscapeHelper(Context* ctx, const FiberType* fiber, Error* e);

template<typename FiberType>
JSVal UnescapeHelper(Context* ctx, const FiberType* fiber, Error* e);

}  // namespace detail

JSVal GlobalParseInt(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("parseInt", args, e);
  if (args.size() > 0) {
    JSString* const str = args[0].ToString(args.ctx(), IV_LV5_ERROR(e));
    int radix = 0;
    if (args.size() > 1) {
      const double ret = args[1].ToNumber(args.ctx(), IV_LV5_ERROR(e));
      radix = core::DoubleToInt32(ret);
    }
    bool strip_prefix = true;
    if (radix != 0) {
      if (radix < 2 || radix > 36) {
        return JSNaN;
      }
      if (radix != 16) {
        strip_prefix = false;
      }
    } else {
      radix = 10;
    }
    if (str->Is8Bit()) {
      return core::StringToIntegerWithRadix(*str->Flatten8(),
                                            radix, strip_prefix);
    } else {
      return core::StringToIntegerWithRadix(*str->Flatten16(),
                                            radix, strip_prefix);
    }
  } else {
    return JSNaN;
  }
}

// section 15.1.2.3 parseFloat(string)
JSVal GlobalParseFloat(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("parseFloat", args, e);
  if (!args.empty()) {
    JSString* const str = args[0].ToString(args.ctx(), IV_LV5_ERROR(e));
    if (str->Is8Bit()) {
      return core::StringToDouble(*str->Flatten8(), true);
    } else {
      return core::StringToDouble(*str->Flatten16(), true);
    }
  }
  return JSNaN;
}

// section 15.1.2.4 isNaN(number)
JSVal GlobalIsNaN(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("isNaN", args, e);
  if (!args.empty()) {
    if (args.front().IsInt32()) {  // int32_t short circuit
      return JSFalse;
    }
    const double number = args[0].ToNumber(args.ctx(), e);
    return JSVal::Bool(core::math::IsNaN(number));
  }
  return JSTrue;
}

// section 15.1.2.5 isFinite(number)
JSVal GlobalIsFinite(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("isFinite", args, e);
  if (!args.empty()) {
    if (args.front().IsInt32()) {  // int32_t short circuit
      return JSTrue;
    }
    const double number = args.front().ToNumber(args.ctx(), e);
    return JSVal::Bool(core::math::IsFinite(number));
  }
  return JSFalse;
}

// section 15.1.3 URI Handling Function Properties
// section 15.1.3.1 decodeURI(encodedURI)
JSVal GlobalDecodeURI(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("decodeURI", args, e);
  JSString* str = args.At(0).ToString(args.ctx(), IV_LV5_ERROR(e));
  if (str->Is8Bit()) {
    return detail::Decode<detail::URI>(args.ctx(), str->Flatten8(), e);
  } else {
    return detail::Decode<detail::URI>(args.ctx(), str->Flatten16(), e);
  }
}

// section 15.1.3.2 decodeURIComponent(encodedURIComponent)
JSVal GlobalDecodeURIComponent(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("decodeURIComponent", args, e);
  JSString* str = args.At(0).ToString(args.ctx(), IV_LV5_ERROR(e));
  if (str->Is8Bit()) {
    return detail::Decode<detail::URIComponent>(args.ctx(), str->Flatten8(), e);
  } else {
    return detail::Decode<detail::URIComponent>(args.ctx(),
                                                str->Flatten16(), e);
  }
}

// section 15.1.3.3 encodeURI(uri)
JSVal GlobalEncodeURI(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("encodeURIComponent", args, e);
  JSString* str = args.At(0).ToString(args.ctx(), IV_LV5_ERROR(e));
  if (str->Is8Bit()) {
    return detail::Encode<detail::URI>(args.ctx(), str->Flatten8(), e);
  } else {
    return detail::Encode<detail::URI>(args.ctx(), str->Flatten16(), e);
  }
}

// section 15.1.3.4 encodeURIComponent(uriComponent)
JSVal GlobalEncodeURIComponent(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("encodeURI", args, e);
  JSString* str = args.At(0).ToString(args.ctx(), IV_LV5_ERROR(e));
  if (str->Is8Bit()) {
    return detail::Encode<detail::URIComponent>(args.ctx(), str->Flatten8(), e);
  } else {
    return detail::Encode<detail::URIComponent>(args.ctx(),
                                                str->Flatten16(), e);
  }
}

JSVal ThrowTypeError(const Arguments& args, Error* e) {
  e->Report(Error::Type, "[[ThrowTypeError]] called");
  return JSUndefined;
}

template<typename FiberType>
inline JSVal detail::EscapeHelper(Context* ctx,
                                  const FiberType* fiber, Error* e) {
  const char kHexDigits[17] = "0123456789ABCDEF";
  std::array<char16_t, 3> hexbuf;
  hexbuf[0] = '%';
  JSStringBuilder builder;
  const std::size_t len = fiber->size();
  for (typename FiberType::const_iterator it = fiber->begin(),
       last = it + len; it != last; ++it) {
    const char16_t ch = *it;
    if (detail::Escape::ContainsInEncode(ch)) {
      builder.Append(ch);
    } else {
      if (ch < 256) {
        hexbuf[1] = kHexDigits[ch / 16];
        hexbuf[2] = kHexDigits[ch % 16];
        builder.Append(hexbuf.begin(), hexbuf.size());
      } else {
        core::UnicodeSequenceEscape(
            std::back_inserter(builder), ch, "%u");
      }
    }
  }
  return builder.Build(ctx, true, e);
}

// section B.2.1 escape(string)
// this method is deprecated.
JSVal GlobalEscape(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("escape", args, e);
  Context* const ctx = args.ctx();
  JSString* str = args.At(0).ToString(ctx, IV_LV5_ERROR(e));
  if (str->empty()) {
    return str;  // empty string
  }
  if (str->Is8Bit()) {
    return detail::EscapeHelper(ctx, str->Flatten8(), e);
  } else {
    return detail::EscapeHelper(ctx, str->Flatten16(), e);
  }
}

template<typename FiberType>
inline JSVal detail::UnescapeHelper(Context* ctx,
                                    const FiberType* fiber, Error* e) {
  JSStringBuilder builder;
  const std::size_t len = fiber->size();
  std::size_t k = 0;
  while (k != len) {
    const char16_t ch = (*fiber)[k];
    if (ch == '%') {
      if (k <= (len - 6) &&
          (*fiber)[k + 1] == 'u' &&
          core::character::IsHexDigit((*fiber)[k + 2]) &&
          core::character::IsHexDigit((*fiber)[k + 3]) &&
          core::character::IsHexDigit((*fiber)[k + 4]) &&
          core::character::IsHexDigit((*fiber)[k + 5])) {
        char16_t uc = '\0';
        for (int i = k + 2, last = k + 6; i < last; ++i) {
          const int d = core::HexValue((*fiber)[i]);
          uc = uc * 16 + d;
        }
        builder.Append(uc);
        k += 6;
      } else if (k <= (len - 3) &&
                 core::character::IsHexDigit((*fiber)[k + 1]) &&
                 core::character::IsHexDigit((*fiber)[k + 2])) {
        // step 14
        builder.Append(
            core::HexValue((*fiber)[k + 1]) * 16 +
            core::HexValue((*fiber)[k + 2]));
        k += 3;
      } else {
        // step 18
        builder.Append(ch);
        ++k;
      }
    } else {
      // step 18
      builder.Append(ch);
      ++k;
    }
  }
  return builder.Build(ctx, false, e);
}

// section B.2.2 unescape(string)
// this method is deprecated.
JSVal GlobalUnescape(const Arguments& args, Error* e) {
  IV_LV5_CONSTRUCTOR_CHECK("unescape", args, e);
  Context* const ctx = args.ctx();
  JSString* str = args.At(0).ToString(ctx, IV_LV5_ERROR(e));
  if (str->empty()) {
    return str;  // empty string
  }
  if (str->Is8Bit()) {
    return detail::UnescapeHelper(ctx, str->Flatten8(), e);
  } else {
    return detail::UnescapeHelper(ctx, str->Flatten16(), e);
  }
}

} } }  // namespace iv::lv5::runtime
