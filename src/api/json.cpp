#include "deckhand/api/json.hpp"

#include <locale>
#include <sstream>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace deckhand::api {

bool valid_utf8(std::string_view s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const auto* e = p + s.size();
  while (p < e) {
    unsigned char c = *p;
    if (c < 0x80) { ++p; continue; }
    int n;
    std::uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else return false;
    if (e - p <= n) return false;
    for (int i = 1; i <= n; ++i) {
      if ((p[i] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
    if (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
    p += n + 1;
  }
  return true;
}

namespace {
const Json kNull;
const std::string kEmpty;

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void dump_string(std::string& out, const std::string& s) {
  out += '"';
  const bool ok = valid_utf8(s);
  for (std::size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else if (c >= 0x80 && !ok) {
          // Invalid UTF-8 somewhere: emit U+FFFD for every non-ASCII byte.
          out += "\xEF\xBF\xBD";
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}
}  // namespace

class JsonParser {
 public:
  JsonParser(std::string_view t, const JsonLimits& l) : p_(t.data()), b_(t.data()), e_(t.data() + t.size()), lim_(l) {}

  bool run(Json* out) {
    ws();
    if (!value(*out, 0)) return false;
    ws();
    if (p_ != e_) return fail("trailing characters");
    return true;
  }
  std::string err;

 private:
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at offset " + std::to_string(p_ - b_);
    return false;
  }
  void ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool lit(const char* s, std::size_t n) {
    if (static_cast<std::size_t>(e_ - p_) >= n && std::string_view(p_, n) == std::string_view(s, n)) {
      p_ += n;
      return true;
    }
    return false;
  }

  bool value(Json& out, std::size_t depth) {
    if (p_ >= e_) return fail("unexpected end");
    switch (*p_) {
      case 'n': if (!lit("null", 4)) return fail("invalid literal"); out = Json(); return true;
      case 't': if (!lit("true", 4)) return fail("invalid literal"); out = Json(true); return true;
      case 'f': if (!lit("false", 5)) return fail("invalid literal"); out = Json(false); return true;
      case '"': {
        std::string s;
        if (!string(s)) return false;
        out = Json(std::move(s));
        return true;
      }
      case '[': return array(out, depth);
      case '{': return object(out, depth);
      default: return number(out);
    }
  }

  bool array(Json& out, std::size_t depth) {
    if (depth + 1 > lim_.max_depth) return fail("nesting too deep");
    ++p_;
    out = Json::array();
    ws();
    if (p_ < e_ && *p_ == ']') { ++p_; return true; }
    for (;;) {
      ws();
      Json v;
      if (!value(v, depth + 1)) return false;
      out.items_.push_back(std::move(v));
      ws();
      if (p_ >= e_) return fail("unexpected end");
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == ']') { ++p_; return true; }
      return fail("expected ',' or ']'");
    }
  }

  bool object(Json& out, std::size_t depth) {
    if (depth + 1 > lim_.max_depth) return fail("nesting too deep");
    ++p_;
    out = Json::object();
    ws();
    if (p_ < e_ && *p_ == '}') { ++p_; return true; }
    for (;;) {
      ws();
      if (p_ >= e_ || *p_ != '"') return fail("expected string key");
      std::string k;
      if (!string(k)) return false;
      ws();
      if (p_ >= e_ || *p_ != ':') return fail("expected ':'");
      ++p_;
      ws();
      Json v;
      if (!value(v, depth + 1)) return false;
      out.set(std::move(k), std::move(v));  // duplicate keys: last one wins
      ws();
      if (p_ >= e_) return fail("unexpected end");
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == '}') { ++p_; return true; }
      return fail("expected ',' or '}'");
    }
  }

  bool hex4(std::uint32_t& v) {
    if (e_ - p_ < 4) return fail("bad \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return fail("bad \\u escape");
    }
    return true;
  }

  bool string(std::string& out) {
    ++p_;  // opening quote
    for (;;) {
      if (p_ >= e_) return fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(*p_++);
      if (c == '"') return true;
      if (c < 0x20) { --p_; return fail("control character in string"); }
      if (c != '\\') { out += static_cast<char>(c); continue; }
      if (p_ >= e_) return fail("unterminated string");
      char x = *p_++;
      switch (x) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return fail("unpaired surrogate");
            p_ += 2;
            std::uint32_t lo = 0;
            if (!hex4(lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unpaired surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default: --p_; return fail("invalid escape");
      }
    }
  }

  bool number(Json& out) {
    const char* s = p_;
    const char* q = p_;
    if (q < e_ && *q == '-') ++q;
    if (q >= e_) return fail("invalid number");
    if (*q == '0') ++q;
    else if (*q >= '1' && *q <= '9') { while (q < e_ && *q >= '0' && *q <= '9') ++q; }
    else { p_ = q; return fail("unexpected character"); }
    bool integral = true;
    if (q < e_ && *q == '.') {
      integral = false;
      ++q;
      if (q >= e_ || *q < '0' || *q > '9') { p_ = q; return fail("invalid number"); }
      while (q < e_ && *q >= '0' && *q <= '9') ++q;
    }
    if (q < e_ && (*q == 'e' || *q == 'E')) {
      integral = false;
      ++q;
      if (q < e_ && (*q == '+' || *q == '-')) ++q;
      if (q >= e_ || *q < '0' || *q > '9') { p_ = q; return fail("invalid number"); }
      while (q < e_ && *q >= '0' && *q <= '9') ++q;
    }
    p_ = q;
    if (integral) {
      std::int64_t v = 0;
      auto r = std::from_chars(s, q, v);
      if (r.ec == std::errc() && r.ptr == q) { out = Json(v); return true; }
    }
    // Floating-point std::from_chars is missing on older Apple systems; the token is already
    // validated against the JSON grammar, so parse it with a stream fixed to the classic locale.
    double d = 0;
    std::istringstream in(std::string(s, q));
    in.imbue(std::locale::classic());
    in >> d;
    if (in.fail() || !std::isfinite(d)) return fail("number out of range");
    out = Json(d);
    return true;
  }

  const char* p_;
  const char* b_;
  const char* e_;
  JsonLimits lim_;
};

std::int64_t Json::as_int(std::int64_t def) const {
  if (type_ != Type::Number) return def;
  if (integral_) return i_;
  if (!std::isfinite(d_) || d_ >= 9.2e18 || d_ <= -9.2e18) return def;
  return static_cast<std::int64_t>(d_);
}

const std::string& Json::as_string() const { return type_ == Type::String ? s_ : kEmpty; }

const Json& Json::at(std::size_t i) const { return i < items_.size() ? items_[i] : kNull; }
const std::string& Json::key_at(std::size_t i) const { return i < keys_.size() ? keys_[i] : kEmpty; }

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (std::size_t i = 0; i < keys_.size(); ++i)
    if (keys_[i] == key) return &items_[i];
  return nullptr;
}

const Json& Json::operator[](std::string_view key) const {
  const Json* f = find(key);
  return f ? *f : kNull;
}

Json& Json::push_back(Json v) {
  if (type_ == Type::Null) type_ = Type::Array;
  if (type_ == Type::Array) items_.push_back(std::move(v));
  return *this;
}

Json& Json::set(std::string key, Json v) {
  if (type_ == Type::Null) type_ = Type::Object;
  if (type_ != Type::Object) return *this;
  for (std::size_t i = 0; i < keys_.size(); ++i)
    if (keys_[i] == key) { items_[i] = std::move(v); return *this; }
  keys_.push_back(std::move(key));
  items_.push_back(std::move(v));
  return *this;
}

void Json::dump_to(std::string& out) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
      if (integral_) { out += std::to_string(i_); break; }
      if (!std::isfinite(d_)) { out += "null"; break; }
      char buf[40];
      auto r = std::to_chars(buf, buf + sizeof buf, d_);
      out.append(buf, r.ptr);
      break;
    }
    case Type::String: dump_string(out, s_); break;
    case Type::Array:
      out += '[';
      for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i) out += ',';
        items_[i].dump_to(out);
      }
      out += ']';
      break;
    case Type::Object:
      out += '{';
      for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i) out += ',';
        dump_string(out, keys_[i]);
        out += ':';
        items_[i].dump_to(out);
      }
      out += '}';
      break;
  }
}

std::string Json::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

bool Json::try_parse(std::string_view text, Json* out, std::string* error, const JsonLimits& limits) {
  Json tmp;
  std::string msg;
  if (text.size() > limits.max_bytes) {
    msg = "input too large";
  } else if (!valid_utf8(text)) {
    msg = "invalid UTF-8";
  } else {
    JsonParser p(text, limits);
    if (p.run(&tmp)) {
      if (out) *out = std::move(tmp);
      return true;
    }
    msg = p.err;
  }
  if (error) *error = msg;
  if (out) *out = Json();
  return false;
}

Json Json::parse(std::string_view text, std::string* error, const JsonLimits& limits) {
  Json j;
  try_parse(text, &j, error, limits);
  return j;
}

bool Json::operator==(const Json& o) const {
  if (type_ != o.type_) return false;
  switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return b_ == o.b_;
    case Type::Number: return integral_ && o.integral_ ? i_ == o.i_ : d_ == o.d_;
    case Type::String: return s_ == o.s_;
    case Type::Array: return items_ == o.items_;
    case Type::Object: return keys_ == o.keys_ && items_ == o.items_;
  }
  return false;
}

}  // namespace deckhand::api
