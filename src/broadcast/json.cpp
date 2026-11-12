#include "deckhand/broadcast/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace deckhand::broadcast {
namespace {

const Json kNull;
constexpr int kMaxDepth = 64;

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 when malformed.
std::size_t utf8_len(std::string_view s, std::size_t i) {
  const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = b(i);
  const std::size_t left = s.size() - i;
  if (c < 0x80) return 1;
  if (c >= 0xc2 && c <= 0xdf) return left >= 2 && (b(i + 1) & 0xc0) == 0x80 ? 2 : 0;
  if (c >= 0xe0 && c <= 0xef) {
    if (left < 3 || (b(i + 1) & 0xc0) != 0x80 || (b(i + 2) & 0xc0) != 0x80) return 0;
    if (c == 0xe0 && b(i + 1) < 0xa0) return 0;   // overlong
    if (c == 0xed && b(i + 1) >= 0xa0) return 0;  // surrogates
    return 3;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    if (left < 4 || (b(i + 1) & 0xc0) != 0x80 || (b(i + 2) & 0xc0) != 0x80 || (b(i + 3) & 0xc0) != 0x80) return 0;
    if (c == 0xf0 && b(i + 1) < 0x90) return 0;
    if (c == 0xf4 && b(i + 1) >= 0x90) return 0;
    return 4;
  }
  return 0;
}

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  std::optional<Json> run() {
    Json v;
    if (!value(v, 0)) return std::nullopt;
    ws();
    if (p_ != t_.size()) return std::nullopt;
    return v;
  }

 private:
  void ws() {
    while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
  }
  bool lit(std::string_view w) {
    if (t_.substr(p_, w.size()) != w) return false;
    p_ += w.size();
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaxDepth) return false;
    ws();
    if (p_ >= t_.size()) return false;
    const char c = t_[p_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"') {
      std::string s;
      if (!str(s)) return false;
      out = Json(std::move(s));
      return true;
    }
    if (c == 't') { if (!lit("true")) return false; out = Json(true); return true; }
    if (c == 'f') { if (!lit("false")) return false; out = Json(false); return true; }
    if (c == 'n') { if (!lit("null")) return false; out = Json(); return true; }
    return number(out);
  }

  bool number(Json& out) {
    const std::size_t start = p_;
    if (p_ < t_.size() && t_[p_] == '-') ++p_;
    if (p_ >= t_.size()) return false;
    if (t_[p_] == '0') {
      ++p_;
    } else if (t_[p_] >= '1' && t_[p_] <= '9') {
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    } else {
      return false;
    }
    if (p_ < t_.size() && t_[p_] == '.') {
      ++p_;
      const std::size_t d = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == d) return false;
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      ++p_;
      if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
      const std::size_t d = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == d) return false;
    }
    const std::string num(t_.substr(start, p_ - start));
    out = Json(std::strtod(num.c_str(), nullptr));
    return true;
  }

  bool hex4(std::uint32_t& v) {
    if (p_ + 4 > t_.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool str(std::string& out) {
    ++p_;  // opening quote
    while (p_ < t_.size()) {
      const unsigned char c = static_cast<unsigned char>(t_[p_]);
      if (c == '"') { ++p_; return true; }
      if (c < 0x20) return false;
      if (c != '\\') { out.push_back(static_cast<char>(c)); ++p_; continue; }
      if (++p_ >= t_.size()) return false;
      const char e = t_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            std::uint32_t lo;
            if (!lit("\\u") || !hex4(lo) || lo < 0xdc00 || lo > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return false;
          }
          append_utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool array(Json& out, int depth) {
    ++p_;
    out = Json::array();
    ws();
    if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
    for (;;) {
      Json v;
      if (!value(v, depth + 1)) return false;
      out.push(std::move(v));
      ws();
      if (p_ >= t_.size()) return false;
      if (t_[p_] == ',') { ++p_; continue; }
      if (t_[p_] == ']') { ++p_; return true; }
      return false;
    }
  }

  bool object(Json& out, int depth) {
    ++p_;
    out = Json::object();
    ws();
    if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
    for (;;) {
      ws();
      if (p_ >= t_.size() || t_[p_] != '"') return false;
      std::string k;
      if (!str(k)) return false;
      ws();
      if (p_ >= t_.size() || t_[p_] != ':') return false;
      ++p_;
      Json v;
      if (!value(v, depth + 1)) return false;
      out.set(k, std::move(v));
      ws();
      if (p_ >= t_.size()) return false;
      if (t_[p_] == ',') { ++p_; continue; }
      if (t_[p_] == '}') { ++p_; return true; }
      return false;
    }
  }

  std::string_view t_;
  std::size_t p_ = 0;
};

}  // namespace

const Json& Json::operator[](const std::string& key) const {
  if (type_ == Type::Object)
    for (std::size_t i = 0; i < keys_.size(); ++i)
      if (keys_[i] == key) return vals_[i];
  return kNull;
}

bool Json::has(const std::string& key) const {
  if (type_ != Type::Object) return false;
  for (const auto& k : keys_)
    if (k == key) return true;
  return false;
}

const Json& Json::at(std::size_t i) const { return type_ == Type::Array && i < vals_.size() ? vals_[i] : kNull; }

Json& Json::set(const std::string& key, Json v) {
  if (type_ == Type::Null) type_ = Type::Object;
  if (type_ != Type::Object) return *this;
  for (std::size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      vals_[i] = std::move(v);
      return *this;
    }
  }
  keys_.push_back(key);
  vals_.push_back(std::move(v));
  return *this;
}

Json& Json::push(Json v) {
  if (type_ == Type::Null) type_ = Type::Array;
  if (type_ == Type::Array) vals_.push_back(std::move(v));
  return *this;
}

std::string Json::quote(std::string_view s) {
  std::string out = "\"";
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    const std::size_t n = utf8_len(s, i);
    if (n == 0) {
      out += "\xef\xbf\xbd";
      ++i;
    } else {
      out.append(s.substr(i, n));
      i += n;
    }
  }
  out.push_back('"');
  return out;
}

void Json::dump_to(std::string& out) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(n_)) { out += "null"; break; }
      char buf[40];
      if (std::floor(n_) == n_ && std::fabs(n_) < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n_));
      else std::snprintf(buf, sizeof buf, "%.15g", n_);
      out += buf;
      break;
    }
    case Type::String: out += quote(s_); break;
    case Type::Array:
      out.push_back('[');
      for (std::size_t i = 0; i < vals_.size(); ++i) {
        if (i) out.push_back(',');
        vals_[i].dump_to(out);
      }
      out.push_back(']');
      break;
    case Type::Object:
      out.push_back('{');
      for (std::size_t i = 0; i < vals_.size(); ++i) {
        if (i) out.push_back(',');
        out += quote(keys_[i]);
        out.push_back(':');
        vals_[i].dump_to(out);
      }
      out.push_back('}');
      break;
  }
}

std::string Json::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

std::optional<Json> Json::parse(std::string_view text) { return Parser(text).run(); }

}  // namespace deckhand::broadcast
