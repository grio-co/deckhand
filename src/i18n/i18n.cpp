// i18n: pack parsing, message formatting, plural rules, number formatting and the thread-safe catalog.
#include "deckhand/i18n/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "deckhand/log/log.hpp"

namespace deckhand::i18n {
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxFile = 4u << 20;
constexpr size_t kMaxStrings = 20000;
constexpr size_t kMaxValue = 16384;
constexpr size_t kMaxKey = 128;
constexpr size_t kMaxHelp = 512;
constexpr size_t kMaxHelpBytes = 256u << 10;
constexpr int kMaxDepth = 8;

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::string lang_of(std::string_view tag) {
  std::string t = lower(tag);
  size_t d = t.find('-');
  return d == std::string::npos ? t : t.substr(0, d);
}

bool valid_key(std::string_view k) {
  if (k.empty() || k.size() > kMaxKey) return false;
  for (char c : k)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return false;
  return true;
}

// ---- plural rules (CLDR) ---------------------------------------------------------------------------
Plural rules_for(const std::string& l, double n) {
  const double an = std::fabs(n);
  const double ip = std::floor(an);
  const bool frac = an != ip;
  const long long i = static_cast<long long>(std::min(ip, 9e15));
  const bool millions = !frac && i != 0 && i % 1000000 == 0;
  if (l == "ja" || l == "zh" || l == "ko" || l == "vi" || l == "th" || l == "id") return Plural::Other;
  if (l == "fr") {
    if (an >= 0 && an < 2) return Plural::One;
    return millions ? Plural::Many : Plural::Other;
  }
  if (l == "pt") {
    if (an >= 0 && an < 2 && (!frac || i == 0 || i == 1)) return Plural::One;
    return millions ? Plural::Many : Plural::Other;
  }
  if (l == "es") {
    if (an == 1) return Plural::One;
    return millions ? Plural::Many : Plural::Other;
  }
  if (l == "it") {
    if (i == 1 && !frac) return Plural::One;
    return millions ? Plural::Many : Plural::Other;
  }
  if (l == "tr") return an == 1 ? Plural::One : Plural::Other;
  if (l == "ru" || l == "uk") {
    if (frac) return Plural::Other;
    const long long m10 = i % 10, m100 = i % 100;
    if (m10 == 1 && m100 != 11) return Plural::One;
    if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return Plural::Few;
    return Plural::Many;
  }
  if (l == "pl") {
    if (frac) return Plural::Other;
    if (i == 1) return Plural::One;
    const long long m10 = i % 10, m100 = i % 100;
    if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return Plural::Few;
    return Plural::Many;
  }
  if (l == "cs" || l == "sk") {
    if (frac) return Plural::Many;
    if (i == 1) return Plural::One;
    if (i >= 2 && i <= 4) return Plural::Few;
    return Plural::Other;
  }
  if (l == "ar") {
    if (frac) return Plural::Other;
    if (i == 0) return Plural::Zero;
    if (i == 1) return Plural::One;
    if (i == 2) return Plural::Two;
    const long long m100 = i % 100;
    if (m100 >= 3 && m100 <= 10) return Plural::Few;
    if (m100 >= 11 && m100 <= 99) return Plural::Many;
    return Plural::Other;
  }
  if (l == "he") {
    if (!frac && i == 1) return Plural::One;
    if (!frac && i == 2) return Plural::Two;
    if (frac && i == 0) return Plural::One;
    return Plural::Other;
  }
  // en, de, nl, sv, da, nb, fi, ... and the default
  return (i == 1 && !frac) ? Plural::One : Plural::Other;
}

// ---- number formatting -----------------------------------------------------------------------------
struct NumSyms {
  std::string dec, group;
  int min_group;  // minimum leading digits before the first separator (2 = "1234" stays ungrouped)
};

NumSyms symbols_for(std::string_view tag) {
  const std::string full = lower(tag);
  const std::string l = lang_of(tag);
  const std::string nnbsp = "\xE2\x80\xAF", nbsp = "\xC2\xA0";
  if (l == "fr") return {",", nnbsp, 1};
  if (l == "de" || l == "it" || l == "nl" || l == "tr" || l == "id") return {",", ".", 1};
  if (l == "es") return {",", ".", 2};
  if (l == "pt") return {",", full == "pt-pt" ? nbsp : ".", full == "pt-pt" ? 2 : 1};
  if (l == "ru" || l == "uk" || l == "cs" || l == "sk") return {",", nbsp, 1};
  if (l == "pl") return {",", nbsp, 2};
  return {".", ",", 1};  // en, ja, zh, ko, ar (latin digits), he, default
}

std::string number_to_string(double v, std::string_view tag, int max_frac, int min_frac) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E";
  max_frac = std::clamp(max_frac, 0, 9);
  min_frac = std::clamp(min_frac, 0, max_frac);
  char buf[512];
  std::snprintf(buf, sizeof buf, "%.*f", max_frac, std::fabs(v));
  std::string s = buf;
  size_t dot = s.find('.');
  std::string ip = dot == std::string::npos ? s : s.substr(0, dot);
  std::string fp = dot == std::string::npos ? "" : s.substr(dot + 1);
  while (static_cast<int>(fp.size()) > min_frac && !fp.empty() && fp.back() == '0') fp.pop_back();
  const NumSyms sy = symbols_for(tag);
  std::string g;
  if (static_cast<int>(ip.size()) >= 3 + sy.min_group) {
    const size_t n = ip.size();
    for (size_t k = 0; k < n; ++k) {
      g += ip[k];
      const size_t left = n - k - 1;
      if (left > 0 && left % 3 == 0) g += sy.group;
    }
  } else {
    g = ip;
  }
  std::string out;
  const bool neg = v < 0 && (g != "0" || !fp.empty());
  if (neg) out = "-";
  out += g;
  if (!fp.empty()) out += sy.dec + fp;
  return out;
}

// ---- message parsing -------------------------------------------------------------------------------
// Returns index of the '}' matching the '{' at `open`, or npos.
size_t find_close(std::string_view s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == '\\') {
      ++i;
      continue;
    }
    if (s[i] == '{') ++depth;
    else if (s[i] == '}' && --depth == 0) return i;
  }
  return std::string_view::npos;
}

struct Parts {
  std::string name, type, rest;
};

Parts split_placeholder(std::string_view inner) {
  Parts p;
  int depth = 0;
  std::vector<size_t> cuts;
  for (size_t i = 0; i < inner.size() && cuts.size() < 2; ++i) {
    if (inner[i] == '\\') {
      ++i;
      continue;
    }
    if (inner[i] == '{') ++depth;
    else if (inner[i] == '}') --depth;
    else if (inner[i] == ',' && depth == 0) cuts.push_back(i);
  }
  if (cuts.empty()) {
    p.name = trim(inner);
  } else {
    p.name = trim(inner.substr(0, cuts[0]));
    if (cuts.size() == 1) {
      p.type = trim(inner.substr(cuts[0] + 1));
    } else {
      p.type = trim(inner.substr(cuts[0] + 1, cuts[1] - cuts[0] - 1));
      p.rest = std::string(inner.substr(cuts[1] + 1));
    }
  }
  return p;
}

using Options = std::vector<std::pair<std::string, std::string>>;

bool parse_options(std::string_view rest, Options& out) {
  size_t i = 0;
  while (true) {
    while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    if (i >= rest.size()) return true;
    size_t s0 = i;
    while (i < rest.size() && rest[i] != '{' && !std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    std::string sel(rest.substr(s0, i - s0));
    while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    if (sel.empty() || i >= rest.size() || rest[i] != '{') return false;
    size_t close = find_close(rest, i);
    if (close == std::string_view::npos) return false;
    out.emplace_back(sel, std::string(rest.substr(i + 1, close - i - 1)));
    i = close + 1;
  }
}

const Value* find_arg(const Args& args, const std::string& name) {
  for (const auto& a : args)
    if (a.first == name) return &a.second;
  return nullptr;
}

bool is_name(const std::string& n) {
  if (n.empty()) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return true;
}

// Walks a message: collects argument names, reports the first syntax error.
bool analyze(std::string_view s, std::set<std::string>* names, std::string* err, int depth) {
  if (depth > kMaxDepth) {
    if (err) *err = "nesting too deep";
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') {
      ++i;
      continue;
    }
    if (s[i] == '}') {
      if (err) *err = "unmatched '}'";
      return false;
    }
    if (s[i] != '{') continue;
    size_t close = find_close(s, i);
    if (close == std::string_view::npos) {
      if (err) *err = "unclosed '{'";
      return false;
    }
    Parts p = split_placeholder(s.substr(i + 1, close - i - 1));
    if (!is_name(p.name)) {
      if (err) *err = "bad placeholder name '" + p.name + "'";
      return false;
    }
    if (names) names->insert(p.name);
    if (p.type == "plural" || p.type == "select") {
      Options opts;
      if (!parse_options(p.rest, opts) || opts.empty()) {
        if (err) *err = "bad " + p.type + " options for '" + p.name + "'";
        return false;
      }
      bool has_other = false;
      for (auto& o : opts) {
        if (o.first == "other") has_other = true;
        if (!analyze(o.second, names, err, depth + 1)) return false;
      }
      if (!has_other) {
        if (err) *err = p.type + " for '" + p.name + "' lacks 'other'";
        return false;
      }
    } else if (!p.type.empty() && p.type != "number") {
      if (err) *err = "unknown placeholder type '" + p.type + "'";
      return false;
    }
    i = close;
  }
  return true;
}

void format_into(std::string_view s, const Args& args, std::string_view tag, const double* hash_n, int depth,
                 std::string& out) {
  if (depth > kMaxDepth) {
    out.append(s);
    return;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '}' || s[i + 1] == '\\' || s[i + 1] == '#')) {
      out += s[++i];
      continue;
    }
    if (c == '#' && hash_n) {
      out += number_to_string(*hash_n, tag, 3, 0);
      continue;
    }
    if (c != '{') {
      out += c;
      continue;
    }
    size_t close = find_close(s, i);
    if (close == std::string_view::npos) {  // malformed: emit rest literally
      out.append(s.substr(i));
      return;
    }
    const std::string_view inner = s.substr(i + 1, close - i - 1);
    Parts p = split_placeholder(inner);
    const Value* v = find_arg(args, p.name);
    if (!v) {
      out.append(s.substr(i, close - i + 1));
    } else if (p.type.empty()) {
      out += v->is_number ? number_to_string(v->number, tag, 3, 0) : v->text;
    } else if (p.type == "number") {
      double n = v->is_number ? v->number : std::atof(v->text.c_str());
      int fr = -1;
      if (!p.rest.empty()) fr = std::atoi(trim(p.rest).c_str());
      out += fr >= 0 ? number_to_string(n, tag, fr, fr) : number_to_string(n, tag, 3, 0);
    } else if (p.type == "plural" || p.type == "select") {
      Options opts;
      if (parse_options(p.rest, opts)) {
        const std::string* chosen = nullptr;
        double n = 0;
        if (p.type == "plural") {
          n = v->is_number ? v->number : std::atof(v->text.c_str());
          char eq[64];
          std::snprintf(eq, sizeof eq, "=%g", n);
          for (auto& o : opts)
            if (o.first == eq) chosen = &o.second;
          if (!chosen) {
            const std::string cat = plural_name(plural_category(tag, n));
            for (auto& o : opts)
              if (o.first == cat) chosen = &o.second;
          }
        } else {
          const std::string key = v->is_number ? number_to_string(v->number, tag, 3, 0) : v->text;
          for (auto& o : opts)
            if (o.first == key) chosen = &o.second;
        }
        if (!chosen)
          for (auto& o : opts)
            if (o.first == "other") chosen = &o.second;
        if (chosen) format_into(*chosen, args, tag, p.type == "plural" ? &n : hash_n, depth + 1, out);
      } else {
        out.append(s.substr(i, close - i + 1));
      }
    } else {
      out.append(s.substr(i, close - i + 1));
    }
    i = close;
  }
}

std::string unescape_line(std::string_view v) {
  std::string r;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[i + 1];
      if (n == 'n') r += '\n';
      else if (n == 't') r += '\t';
      else { r += v[i]; r += n; }
      ++i;
    } else {
      r += v[i];
    }
  }
  return r;
}

bool read_file(const fs::path& p, size_t max, std::string& out) {
  std::error_code ec;
  auto sz = fs::file_size(p, ec);
  if (ec || sz > max) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

// ---- public helpers --------------------------------------------------------------------------------
std::string normalize_tag(std::string_view tag) {
  std::string t(tag);
  for (char& c : t)
    if (c == '_') c = '-';
  std::string out;
  size_t idx = 0, pos = 0;
  while (pos <= t.size()) {
    size_t d = t.find('-', pos);
    std::string part = t.substr(pos, d == std::string::npos ? std::string::npos : d - pos);
    for (char& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (idx > 0 && part.size() == 2) for (char& c : part) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    else if (idx > 0 && part.size() == 4 && !part.empty()) part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
    if (idx) out += '-';
    out += part;
    ++idx;
    if (d == std::string::npos) break;
    pos = d + 1;
  }
  return out;
}

Plural plural_category(std::string_view tag, double n) { return rules_for(lang_of(tag), n); }

const char* plural_name(Plural p) {
  switch (p) {
    case Plural::Zero: return "zero";
    case Plural::One: return "one";
    case Plural::Two: return "two";
    case Plural::Few: return "few";
    case Plural::Many: return "many";
    case Plural::Other: return "other";
  }
  return "other";
}

std::string format_number(double v, std::string_view tag, int max_frac, int min_frac) {
  return number_to_string(v, tag, max_frac, min_frac);
}

std::string format_message(std::string_view msg, const Args& args, std::string_view tag) {
  try {
    std::string out;
    out.reserve(msg.size() + 16);
    format_into(msg, args, tag, nullptr, 0, out);
    return out;
  } catch (...) {
    return std::string(msg);
  }
}

std::set<std::string> placeholders(std::string_view msg) {
  std::set<std::string> names;
  try {
    analyze(msg, &names, nullptr, 0);
  } catch (...) {
  }
  return names;
}

std::string check_message_syntax(std::string_view msg) {
  std::string err;
  try {
    analyze(msg, nullptr, &err, 0);
  } catch (...) {
    err = "internal error";
  }
  return err;
}

// ---- packs -----------------------------------------------------------------------------------------
std::optional<Pack> parse_pack(std::string_view text, std::string* error) {
  auto fail = [&](const std::string& m) -> std::optional<Pack> {
    if (error) *error = m;
    return std::nullopt;
  };
  try {
    if (text.size() > kMaxFile) return fail("pack too large");
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    Pack p;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
      pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
      ++line_no;
      std::string line = trim(raw);
      if (line.empty() || line[0] == '#') continue;
      if (line[0] == '@') {
        size_t sp = line.find_first_of(" \t");
        std::string key = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        std::string val = sp == std::string::npos ? "" : trim(line.substr(sp));
        if (key == "lang") p.manifest.tag = normalize_tag(val);
        else if (key == "name") p.manifest.name = val;
        else if (key == "dir") p.manifest.dir = lower(val) == "rtl" ? Direction::Rtl : Direction::Ltr;
        else if (key == "version") p.manifest.version = val;
        else if (key == "fallback") {
          size_t a = 0;
          while (a <= val.size()) {
            size_t c = val.find(',', a);
            std::string t = trim(val.substr(a, c == std::string::npos ? std::string::npos : c - a));
            if (!t.empty()) p.manifest.fallback.push_back(normalize_tag(t));
            if (c == std::string::npos) break;
            a = c + 1;
          }
        } else p.warnings.push_back("line " + std::to_string(line_no) + ": unknown header @" + key);
        continue;
      }
      size_t eq = line.find('=');
      if (eq == std::string::npos) return fail("line " + std::to_string(line_no) + ": expected 'id = text'");
      std::string key = trim(line.substr(0, eq));
      if (!valid_key(key)) return fail("line " + std::to_string(line_no) + ": bad id '" + key + "'");
      std::string val = unescape_line(trim(line.substr(eq + 1)));
      if (val.size() > kMaxValue) return fail("line " + std::to_string(line_no) + ": value too long");
      if (p.strings.size() >= kMaxStrings) return fail("too many strings");
      if (p.strings.count(key)) p.warnings.push_back("line " + std::to_string(line_no) + ": duplicate id " + key);
      p.strings[key] = std::move(val);
    }
    if (p.manifest.tag.empty()) return fail("missing @lang");
    if (p.manifest.name.empty()) p.manifest.name = p.manifest.tag;
    return p;
  } catch (const std::exception& e) {
    return fail(e.what());
  }
}

std::optional<Pack> load_pack(const std::string& path, std::string* error) {
  auto fail = [&](const std::string& m) -> std::optional<Pack> {
    if (error) *error = path + ": " + m;
    return std::nullopt;
  };
  try {
    std::error_code ec;
    fs::path root(path), file = root;
    const bool dir = fs::is_directory(root, ec);
    if (dir) file = root / "lang.dhl";
    std::string text;
    if (!read_file(file, kMaxFile, text)) return fail("cannot read pack file");
    std::string err;
    auto p = parse_pack(text, &err);
    if (!p) return fail(err);
    p->path = path;
    if (dir && fs::is_directory(root / "help", ec)) {
      for (auto& e : fs::directory_iterator(root / "help", ec)) {
        if (p->help.size() >= kMaxHelp) break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".md") continue;
        std::string topic = e.path().stem().string(), body;
        if (!valid_key(topic) || !read_file(e.path(), kMaxHelpBytes, body)) continue;
        p->help[topic] = std::move(body);
      }
    }
    return p;
  } catch (const std::exception& e) {
    return fail(e.what());
  }
}

Report validate(const Pack& pack, const Pack& english) {
  Report r;
  try {
    for (const auto& [id, en] : english.strings) {
      auto it = pack.strings.find(id);
      if (it == pack.strings.end()) {
        r.missing.push_back(id);
        continue;
      }
      std::string e = check_message_syntax(it->second);
      if (!e.empty()) r.syntax_errors.push_back(id + ": " + e);
      auto want = placeholders(en), got = placeholders(it->second);
      if (want != got) {
        auto join = [](const std::set<std::string>& s) {
          std::string j;
          for (auto& x : s) j += (j.empty() ? "" : ",") + x;
          return "{" + j + "}";
        };
        r.placeholder_mismatch.push_back(id + ": expected " + join(want) + " got " + join(got));
      }
    }
    for (const auto& kv : pack.strings)
      if (!english.strings.count(kv.first)) r.extra.push_back(kv.first);
    for (const auto& kv : english.help)
      if (!pack.help.count(kv.first)) r.missing_help.push_back(kv.first);
    std::sort(r.missing.begin(), r.missing.end());
    std::sort(r.extra.begin(), r.extra.end());
  } catch (...) {
  }
  return r;
}

// ---- catalog ---------------------------------------------------------------------------------------
struct Catalog::State {
  std::map<std::string, std::shared_ptr<const Pack>> packs;  // lower-case tag -> pack
  std::string active = "en";
  Direction dir = Direction::Ltr;
  std::vector<std::shared_ptr<const Pack>> chain;  // lookup order
};

namespace {

std::string parent_tag(const std::string& t) {
  size_t d = t.rfind('-');
  return d == std::string::npos ? "" : t.substr(0, d);
}

void expand(const std::map<std::string, std::shared_ptr<const Pack>>& packs, const std::string& tag,
            std::vector<std::string>& order, int depth) {
  if (depth > 6 || tag.empty()) return;
  std::vector<std::string> own;
  for (std::string t = tag; !t.empty(); t = parent_tag(t)) {
    if (std::find(order.begin(), order.end(), t) == order.end()) {
      order.push_back(t);
      own.push_back(t);
    }
  }
  for (const auto& t : own) {
    auto it = packs.find(t);
    if (it == packs.end()) continue;
    for (const auto& f : it->second->manifest.fallback) expand(packs, lower(f), order, depth + 1);
  }
}

}  // namespace

Catalog::Catalog() : state_(std::make_shared<State>()) {}

std::shared_ptr<const Catalog::State> Catalog::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

void Catalog::rebuild_locked(std::string_view tag) {
  auto s = std::make_shared<State>(*state_);
  s->active = lower(normalize_tag(tag));
  std::vector<std::string> order;
  expand(s->packs, s->active, order, 0);
  expand(s->packs, "en", order, 0);
  s->chain.clear();
  for (const auto& t : order) {
    auto it = s->packs.find(t);
    if (it != s->packs.end()) s->chain.push_back(it->second);
  }
  auto self = s->packs.find(s->active);
  s->dir = self != s->packs.end() ? self->second->manifest.dir : Direction::Ltr;
  state_ = std::move(s);
}

bool Catalog::load_builtin(const std::string& path) {
  std::string err;
  auto p = load_pack(path, &err);
  if (!p) {
    DH_LOG(Warn, "i18n: built-in pack failed: %s", err.c_str());
    return false;
  }
  add_pack(std::move(*p));
  return true;
}

int Catalog::load_directory(const std::string& dir, std::vector<std::string>* errors) {
  int n = 0;
  try {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(dir, ec))
      if (e.path().extension() == ".dhlang") entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (auto& e : entries) {
      std::string err;
      auto p = load_pack(e.string(), &err);
      if (!p) {
        if (errors) errors->push_back(err);
        DH_LOG(Warn, "i18n: %s", err.c_str());
        continue;
      }
      add_pack(std::move(*p));
      ++n;
    }
  } catch (...) {
  }
  return n;
}

void Catalog::add_pack(Pack pack) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::string key = lower(normalize_tag(pack.manifest.tag));
  auto s = std::make_shared<State>(*state_);
  s->packs[key] = std::make_shared<const Pack>(std::move(pack));
  state_ = std::move(s);
  rebuild_locked(state_->active);
}

bool Catalog::set_language(std::string_view tag) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string t = lower(normalize_tag(tag));
  bool found = false;
  for (std::string c = t; !c.empty(); c = parent_tag(c))
    if (state_->packs.count(c)) found = true;
  if (!found) return false;
  rebuild_locked(t);
  return true;
}

std::string Catalog::language() const { return normalize_tag(snapshot()->active); }
Direction Catalog::direction() const { return snapshot()->dir; }

std::vector<LangInfo> Catalog::available() const {
  std::vector<LangInfo> v;
  for (auto& kv : snapshot()->packs) v.push_back({kv.second->manifest.tag, kv.second->manifest.name, kv.second->manifest.dir});
  return v;
}

bool Catalog::has_language(std::string_view tag) const { return snapshot()->packs.count(lower(normalize_tag(tag))) != 0; }

bool Catalog::has(std::string_view id) const {
  auto s = snapshot();
  const std::string k(id);
  for (auto& p : s->chain)
    if (p->strings.count(k)) return true;
  return false;
}

std::string Catalog::tr(std::string_view id) const { return tr(id, Args{}); }

std::string Catalog::tr(std::string_view id, const Args& args) const {
  try {
    auto s = snapshot();
    const std::string k(id);
    for (auto& p : s->chain) {
      auto it = p->strings.find(k);
      if (it == p->strings.end()) continue;
      const std::string& m = it->second;
      if (args.empty() && m.find_first_of("{\\") == std::string::npos) return m;
      return format_message(m, args, p->manifest.tag);
    }
  } catch (...) {
  }
  return std::string(id);
}

std::string Catalog::help(std::string_view topic) const {
  auto s = snapshot();
  const std::string k(topic);
  for (auto& p : s->chain) {
    auto it = p->help.find(k);
    if (it != p->help.end()) return it->second;
  }
  return {};
}

std::string Catalog::format_number(double v, int max_frac, int min_frac) const {
  return number_to_string(v, snapshot()->active, max_frac, min_frac);
}

std::shared_ptr<const Pack> Catalog::pack(std::string_view tag) const {
  auto s = snapshot();
  auto it = s->packs.find(lower(normalize_tag(tag)));
  return it == s->packs.end() ? nullptr : it->second;
}

}  // namespace deckhand::i18n
