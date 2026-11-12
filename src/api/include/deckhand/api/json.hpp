#pragma once
// Small JSON value type with a strict RFC 8259 parser and a serializer.
// Objects keep insertion order. Parsing and serializing never throw.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace deckhand::api {

struct JsonLimits {
  std::size_t max_depth = 64;       // nesting of arrays/objects
  std::size_t max_bytes = 1 << 20;  // input size
};

// True if `s` is well-formed UTF-8 (no overlongs, surrogates or code points > U+10FFFF).
bool valid_utf8(std::string_view s);

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : type_(Type::Bool), b_(v) {}
  template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Json(T v) : type_(Type::Number) {
    if constexpr (std::is_signed_v<T>) {
      integral_ = true;
      i_ = static_cast<std::int64_t>(v);
      d_ = static_cast<double>(v);
    } else if (static_cast<unsigned long long>(v) <= static_cast<unsigned long long>(INT64_MAX)) {
      integral_ = true;
      i_ = static_cast<std::int64_t>(v);
      d_ = static_cast<double>(v);
    } else {
      d_ = static_cast<double>(v);
    }
  }
  Json(double v) : type_(Type::Number), d_(v) {}
  Json(const char* v) : type_(Type::String), s_(v ? v : "") {}
  Json(std::string v) : type_(Type::String), s_(std::move(v)) {}

  static Json array() { Json j; j.type_ = Type::Array; return j; }
  static Json object() { Json j; j.type_ = Type::Object; return j; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }
  // True for numbers written without fraction/exponent that fit in int64.
  bool is_integer() const { return type_ == Type::Number && integral_; }

  // Lenient getters: return `def` when the value has another type.
  bool as_bool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
  double as_double(double def = 0) const { return type_ == Type::Number ? d_ : def; }
  std::int64_t as_int(std::int64_t def = 0) const;
  const std::string& as_string() const;  // empty string for non-strings

  // Arrays and objects: element count. 0 for scalars.
  std::size_t size() const { return items_.size(); }
  // Arrays: element i. Objects: value i (insertion order). Null if out of range.
  const Json& at(std::size_t i) const;
  const std::string& key_at(std::size_t i) const;  // objects only
  // Object lookup. nullptr / Null value when missing or not an object.
  const Json* find(std::string_view key) const;
  const Json& operator[](std::string_view key) const;
  bool contains(std::string_view key) const { return find(key) != nullptr; }

  // Mutators. push_back turns a Null into an array; set turns a Null into an object.
  // Both are ignored on other types. set replaces an existing key in place.
  Json& push_back(Json v);
  Json& set(std::string key, Json v);

  std::string dump() const;
  // Parses a complete document. On failure returns Null and fills *error (if given).
  static Json parse(std::string_view text, std::string* error = nullptr, const JsonLimits& limits = {});
  // Same, but reports success separately (needed to tell "null" from failure).
  static bool try_parse(std::string_view text, Json* out, std::string* error = nullptr,
                        const JsonLimits& limits = {});

  bool operator==(const Json& o) const;
  bool operator!=(const Json& o) const { return !(*this == o); }

 private:
  void dump_to(std::string& out) const;
  friend class JsonParser;

  Type type_ = Type::Null;
  bool b_ = false;
  bool integral_ = false;
  std::int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::vector<Json> items_;         // array elements or object values
  std::vector<std::string> keys_;   // object keys, parallel to items_
};

}  // namespace deckhand::api
