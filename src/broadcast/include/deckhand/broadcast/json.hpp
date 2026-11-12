#pragma once
// Minimal JSON value, parser and writer for the broadcast module (OBS messages, overlay payloads).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deckhand::broadcast {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(bool v) : type_(Type::Bool), b_(v) {}
  Json(int v) : type_(Type::Number), n_(v) {}
  Json(long long v) : type_(Type::Number), n_(static_cast<double>(v)) {}
  Json(double v) : type_(Type::Number), n_(v) {}
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

  // Lenient accessors: return the default when the value has another type.
  bool as_bool(bool def = false) const { return is_bool() ? b_ : def; }
  double as_number(double def = 0) const { return is_number() ? n_ : def; }
  std::string as_string(const std::string& def = {}) const { return is_string() ? s_ : def; }

  // Object lookup; a missing key (or a non-object) yields a shared null value.
  const Json& operator[](const std::string& key) const;
  bool has(const std::string& key) const;
  // Array access; out of range yields null.
  const Json& at(std::size_t i) const;
  std::size_t size() const { return vals_.size(); }
  const std::vector<std::string>& keys() const { return keys_; }

  Json& set(const std::string& key, Json v);  // makes this an object if null
  Json& push(Json v);                         // makes this an array if null

  std::string dump() const;
  // Strict parse (depth-limited). nullopt on any syntax error or trailing garbage.
  static std::optional<Json> parse(std::string_view text);
  // Quoted, escaped string. Invalid UTF-8 is replaced by U+FFFD so the output is always valid JSON.
  static std::string quote(std::string_view s);

 private:
  void dump_to(std::string& out) const;
  Type type_ = Type::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<std::string> keys_;  // objects only, parallel to vals_
  std::vector<Json> vals_;
};

}  // namespace deckhand::broadcast
