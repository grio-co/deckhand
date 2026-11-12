#pragma once
// Internationalisation: installable .dhlang packs, a fallback-aware catalog, an ICU-MessageFormat-like
// subset ({name}, {n, number[, decimals]}, {n, plural, one {..} other {..}}, {g, select, ..}), CLDR plural
// rules and locale number formatting. No public function throws.
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace deckhand::i18n {

enum class Direction { Ltr, Rtl };
enum class Plural { Zero, One, Two, Few, Many, Other };

struct Manifest {
  std::string tag;                   // BCP-47, e.g. "pt-BR"
  std::string name;                  // native name, e.g. "Deutsch"
  Direction dir = Direction::Ltr;
  std::string version;
  std::vector<std::string> fallback; // tried after the regional->base chain, before English
};

struct Pack {
  Manifest manifest;
  std::unordered_map<std::string, std::string> strings;  // stable id -> message
  std::unordered_map<std::string, std::string> help;     // topic id -> Markdown
  std::vector<std::string> warnings;                     // non-fatal parse notes
  std::string path;
};

// A message argument: number or text.
struct Value {
  bool is_number = false;
  double number = 0;
  std::string text;
  Value() = default;
  template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
  Value(T v) : is_number(true), number(static_cast<double>(v)) {}
  Value(const char* s) : text(s ? s : "") {}
  Value(std::string s) : text(std::move(s)) {}
  Value(std::string_view s) : text(s) {}
};
using Args = std::vector<std::pair<std::string, Value>>;

// ---- pure helpers --------------------------------------------------------------------------------
std::string normalize_tag(std::string_view tag);  // "pt_br" -> "pt-BR"
Plural plural_category(std::string_view tag, double n);
const char* plural_name(Plural p);
std::string format_number(double v, std::string_view tag, int max_frac = 3, int min_frac = 0);
// Format a message in `tag`'s conventions. Never throws; malformed input is emitted literally.
std::string format_message(std::string_view msg, const Args& args, std::string_view tag);
// Argument names a message uses (for validation).
std::set<std::string> placeholders(std::string_view msg);
// Empty when well formed, otherwise a description of the first problem.
std::string check_message_syntax(std::string_view msg);

// ---- packs ---------------------------------------------------------------------------------------
// Pack file: UTF-8 text. "@lang de", "@name Deutsch", "@dir ltr", "@version 1", "@fallback en,fr" headers,
// then "id = message" lines; '#' starts a comment line; "\n" and "\t" escapes. A .dhlang is either such a
// file or a directory holding lang.dhl plus help/<topic>.md.
std::optional<Pack> parse_pack(std::string_view text, std::string* error = nullptr);
std::optional<Pack> load_pack(const std::string& path, std::string* error = nullptr);

struct Report {
  std::vector<std::string> missing;               // ids in English but not in the pack
  std::vector<std::string> extra;                 // ids in the pack but not in English
  std::vector<std::string> placeholder_mismatch;  // "id: expected {a,b} got {a}"
  std::vector<std::string> syntax_errors;
  std::vector<std::string> missing_help;
  bool ok() const { return missing.empty() && extra.empty() && placeholder_mismatch.empty() && syntax_errors.empty(); }
};
Report validate(const Pack& pack, const Pack& english);

struct LangInfo {
  std::string tag, name;
  Direction dir = Direction::Ltr;
};

// Thread-safe: readers (tr/help/...) may run concurrently with set_language/add_pack, which publish a new
// immutable snapshot atomically.
class Catalog {
 public:
  Catalog();
  bool load_builtin(const std::string& path);                    // English fallback pack
  int load_directory(const std::string& dir, std::vector<std::string>* errors = nullptr);  // returns packs added
  void add_pack(Pack pack);
  bool set_language(std::string_view tag);  // false (unchanged) if neither tag nor a parent is installed
  std::string language() const;
  Direction direction() const;
  std::vector<LangInfo> available() const;
  bool has_language(std::string_view tag) const;

  std::string tr(std::string_view id) const;
  std::string tr(std::string_view id, const Args& args) const;
  bool has(std::string_view id) const;
  std::string help(std::string_view topic) const;  // Markdown, "" if none in the chain
  std::string format_number(double v, int max_frac = 3, int min_frac = 0) const;
  std::shared_ptr<const Pack> pack(std::string_view tag) const;

 private:
  struct State;
  std::shared_ptr<const State> snapshot() const;
  void rebuild_locked(std::string_view tag);
  mutable std::mutex mu_;
  std::shared_ptr<const State> state_;
};

}  // namespace deckhand::i18n
