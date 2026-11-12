// skin: .dhskin text parser, template expansion, validation and the in-memory model.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "deckhand/log/log.hpp"
#include "deckhand/skin/skin.hpp"

namespace deckhand::skin {
namespace fs = std::filesystem;

// ---- small public helpers --------------------------------------------------------------------------
const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Row: return "row";
    case Kind::Column: return "column";
    case Kind::Stack: return "stack";
    case Kind::Free: return "free";
    case Kind::Grid: return "grid";
    case Kind::Panel: return "panel";
    case Kind::Waveform: return "waveform";
    case Kind::Jog: return "jog";
    case Kind::Button: return "button";
    case Kind::Knob: return "knob";
    case Kind::Fader: return "fader";
    case Kind::Meter: return "meter";
    case Kind::Label: return "label";
    case Kind::Browser: return "browser";
    case Kind::Tree: return "tree";
    case Kind::Pads: return "pads";
    case Kind::Image: return "image";
    case Kind::Spacer: return "spacer";
    case Kind::Unknown: return "unknown";
  }
  return "unknown";
}
bool is_container(Kind k) { return k <= Kind::Panel; }
bool is_interactive(Kind k) {
  switch (k) {
    case Kind::Waveform: case Kind::Jog: case Kind::Button: case Kind::Knob: case Kind::Fader:
    case Kind::Browser: case Kind::Tree: case Kind::Pads: return true;
    default: return false;
  }
}

const Window* Skin::find_window(std::string_view id) const {
  for (auto& w : windows) if (w.id == id) return &w;
  return nullptr;
}
const Shape* Skin::find_shape(std::string_view n) const {
  for (auto& s : shapes) if (s.name == n) return &s;
  return nullptr;
}
const Atlas* Skin::find_atlas(std::string_view n) const {
  for (auto& a : atlases) if (a.name == n) return &a;
  return nullptr;
}

size_t ParseResult::error_count() const {
  size_t n = 0;
  for (auto& m : messages) n += m.severity == Message::Severity::Error;
  return n;
}
size_t ParseResult::warning_count() const { return messages.size() - error_count(); }
std::string ParseResult::describe() const {
  std::string s;
  for (auto& m : messages) {
    s += m.severity == Message::Severity::Error ? "error" : "warning";
    if (m.line > 0) s += " line " + std::to_string(m.line);
    s += ": " + m.text + "\n";
  }
  return s;
}

float animation_value(const Animation& a, float t) {
  const float dur = std::max(1.0f, a.duration_ms);
  float local = t - a.delay_ms;
  if (local <= 0) return a.from;
  float p;
  switch (a.loop) {
    case Loop::Once: p = std::min(1.0f, local / dur); break;
    case Loop::Repeat: p = std::fmod(local, dur) / dur; break;
    default: {
      float q = std::fmod(local, 2 * dur) / dur;
      p = q <= 1 ? q : 2 - q;
    }
  }
  switch (a.ease) {
    case Ease::Linear: break;
    case Ease::In: p = p * p; break;
    case Ease::Out: p = 1 - (1 - p) * (1 - p); break;
    case Ease::InOut: p = p * p * (3 - 2 * p); break;
  }
  return a.from + (a.to - a.from) * p;
}

bool is_safe_relative_path(std::string_view p) {
  if (p.empty() || p.size() > 256) return false;
  if (p[0] == '/' || p[0] == '~') return false;
  for (char c : p)
    if (c == '\\' || c == '\0' || c == ':' || static_cast<unsigned char>(c) < 32) return false;
  size_t a = 0;
  while (a <= p.size()) {
    size_t b = p.find('/', a);
    std::string_view seg = p.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (b == std::string_view::npos) break;
    a = b + 1;
  }
  return true;
}

bool valid_binding(std::string_view n) {
  if (n.empty() || n.size() > 64) return false;
  size_t i = 0;
  int segs = 0;
  while (i < n.size()) {
    size_t st = i;
    if (!(std::islower(static_cast<unsigned char>(n[i])) || n[i] == '_')) return false;
    while (i < n.size() && (std::islower(static_cast<unsigned char>(n[i])) || std::isdigit(static_cast<unsigned char>(n[i])) || n[i] == '_')) ++i;
    if (i == st) return false;
    if (i < n.size() && n[i] == '[') {
      size_t d = ++i;
      while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
      if (i == d || i - d > 3 || i >= n.size() || n[i] != ']') return false;
      ++i;
    }
    ++segs;
    if (i == n.size()) break;
    if (n[i] != '.') return false;
    ++i;
    if (i == n.size()) return false;
  }
  return segs >= 1;
}

i18n::Args parse_text_args(std::string_view s) {
  i18n::Args out;
  size_t a = 0;
  while (a < s.size() && out.size() < 16) {
    size_t c = s.find(',', a);
    std::string_view item = s.substr(a, c == std::string_view::npos ? std::string_view::npos : c - a);
    size_t eq = item.find('=');
    if (eq != std::string_view::npos && eq > 0) {
      std::string v(item.substr(eq + 1));
      char* end = nullptr;
      double d = std::strtod(v.c_str(), &end);
      if (!v.empty() && end && *end == '\0') out.emplace_back(std::string(item.substr(0, eq)), i18n::Value(d));
      else out.emplace_back(std::string(item.substr(0, eq)), i18n::Value(v));
    }
    if (c == std::string_view::npos) break;
    a = c + 1;
  }
  return out;
}

std::string resolve_text(const Skin& skin, const i18n::Catalog& cat, std::string_view spec, const i18n::Args& args) {
  if (spec.empty() || spec[0] != '@') return std::string(spec);
  const std::string id(spec.substr(1));
  std::string lang = i18n::normalize_tag(cat.language());
  for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (std::string l = lang;; ) {
    auto it = skin.strings.find(l);
    if (it != skin.strings.end()) {
      auto s = it->second.find(id);
      if (s != it->second.end()) return i18n::format_message(s->second, args, lang);
    }
    if (l.empty()) break;
    size_t d = l.rfind('-');
    l = d == std::string::npos ? "" : l.substr(0, d);
  }
  return cat.tr(id, args);
}

namespace {

// ---- syntax tree -----------------------------------------------------------------------------------
struct Stmt {
  std::string name;
  int line = 0;
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> props;
  std::vector<Stmt> children;
  bool has_block = false;
};

struct SyntaxError {
  int line;
  std::string msg;
};

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}
  std::vector<Stmt> parse_all() {
    auto v = block(0, false);
    return v;
  }

 private:
  std::string_view s_;
  size_t i_ = 0;
  int line_ = 1;
  size_t count_ = 0;

  [[noreturn]] void fail(const std::string& m) { throw SyntaxError{line_, m}; }
  bool eof() const { return i_ >= s_.size(); }

  void skip_inline() {
    while (!eof()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r') ++i_;
      else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
        while (!eof() && s_[i_] != '\n') ++i_;
      } else break;
    }
  }
  void skip_all() {
    while (!eof()) {
      skip_inline();
      if (eof()) break;
      char c = s_[i_];
      if (c == '\n') { ++line_; ++i_; }
      else if (c == ';') ++i_;
      else break;
    }
  }
  std::string read_string() {
    ++i_;  // opening quote
    std::string r;
    while (true) {
      if (eof() || s_[i_] == '\n') fail("unterminated string");
      char c = s_[i_++];
      if (c == '"') break;
      if (c == '\\' && !eof()) {
        char n = s_[i_++];
        r += n == 'n' ? '\n' : n;
      } else r += c;
      if (r.size() > limits::kMaxStringLen) fail("string too long");
    }
    return r;
  }
  std::string read_word() {
    size_t st = i_;
    while (!eof()) {
      char c = s_[i_];
      if (c == '$' && i_ + 1 < s_.size() && s_[i_ + 1] == '{') {
        while (!eof() && s_[i_] != '}' && s_[i_] != '\n') ++i_;
        if (eof() || s_[i_] != '}') fail("unterminated ${...}");
        ++i_;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == ';' || c == '"' || c == '=') break;
      ++i_;
    }
    if (i_ - st > limits::kMaxStringLen) fail("word too long");
    return std::string(s_.substr(st, i_ - st));
  }
  std::vector<Stmt> block(int depth, bool nested) {
    if (depth > limits::kMaxDepth) fail("blocks nested too deeply");
    std::vector<Stmt> out;
    while (true) {
      skip_all();
      if (eof()) {
        if (nested) fail("missing '}'");
        return out;
      }
      if (s_[i_] == '}') {
        if (!nested) fail("unmatched '}'");
        ++i_;
        return out;
      }
      out.push_back(stmt(depth));
    }
  }
  Stmt stmt(int depth) {
    if (++count_ > limits::kMaxStatements) fail("too many statements");
    Stmt st;
    st.line = line_;
    if (s_[i_] == '"' || s_[i_] == '=' || s_[i_] == '{') fail("statement must start with a name");
    const size_t start = i_;
    st.name = read_word();
    if (st.name.empty()) fail("unexpected character");
    if (!eof() && s_[i_] == '=') {  // "key=value ..." line: a property-only statement
      i_ = start;
      st.name = "@props";
    }
    while (true) {
      skip_inline();
      if (eof()) break;
      char c = s_[i_];
      if (c == '\n') { ++i_; ++line_; break; }
      if (c == ';') { ++i_; break; }
      if (c == '}') break;
      if (c == '{') {
        ++i_;
        st.has_block = true;
        st.children = block(depth + 1, true);
        break;
      }
      if (c == '=') fail("unexpected '='");
      const bool quoted = c == '"';
      std::string w = quoted ? read_string() : read_word();
      if (!quoted && w.empty()) fail("unexpected character");
      if (!quoted && !eof() && s_[i_] == '=') {
        ++i_;
        if (eof() || std::isspace(static_cast<unsigned char>(s_[i_])) || s_[i_] == '{' || s_[i_] == '}') fail("missing value after '" + w + "='");
        std::string v = s_[i_] == '"' ? read_string() : read_word();
        st.props.emplace_back(std::move(w), std::move(v));
        if (st.props.size() > 64) fail("too many properties on one statement");
      } else {
        st.args.push_back(std::move(w));
        if (st.args.size() > 600) fail("too many arguments on one statement");
      }
    }
    return st;
  }
};

// ---- template expansion ----------------------------------------------------------------------------
struct Template {
  std::vector<std::string> params;
  const std::vector<Stmt>* body = nullptr;
};

void substitute(Stmt& s, const std::map<std::string, std::string>& vals) {
  auto sub = [&](std::string& v) {
    size_t p = 0;
    while ((p = v.find("${", p)) != std::string::npos) {
      size_t e = v.find('}', p);
      if (e == std::string::npos) break;
      auto it = vals.find(v.substr(p + 2, e - p - 2));
      if (it == vals.end()) { p = e + 1; continue; }
      v.replace(p, e - p + 1, it->second);
      p += it->second.size();
    }
  };
  for (auto& a : s.args) sub(a);
  for (auto& pr : s.props) sub(pr.second);
  for (auto& c : s.children) substitute(c, vals);
}

class Expander {
 public:
  std::map<std::string, Template> tpls;
  std::vector<std::string> errors;
  size_t emitted = 0;
  std::vector<std::string> stack;

  std::vector<Stmt> expand(const std::vector<Stmt>& in, int depth) {
    std::vector<Stmt> out;
    for (const Stmt& s : in) {
      if (!errors.empty()) return out;
      if (s.name == "use") {
        use(s, depth, out);
        continue;
      }
      if (++emitted > limits::kMaxNodes * 2) {
        errors.push_back("line " + std::to_string(s.line) + ": template expansion too large");
        return out;
      }
      Stmt c;
      c.name = s.name; c.line = s.line; c.args = s.args; c.props = s.props; c.has_block = s.has_block;
      c.children = expand(s.children, depth);
      out.push_back(std::move(c));
    }
    return out;
  }

 private:
  void use(const Stmt& s, int depth, std::vector<Stmt>& out) {
    const std::string at = "line " + std::to_string(s.line) + ": ";
    if (s.args.size() != 1) { errors.push_back(at + "use needs exactly one template name"); return; }
    const std::string& name = s.args[0];
    auto it = tpls.find(name);
    if (it == tpls.end()) { errors.push_back(at + "unknown template '" + name + "'"); return; }
    if (std::find(stack.begin(), stack.end(), name) != stack.end()) {
      std::string chain;
      for (auto& n : stack) chain += n + " -> ";
      errors.push_back(at + "template cycle: " + chain + name);
      return;
    }
    if (depth >= limits::kMaxTemplateDepth) { errors.push_back(at + "templates nested too deeply"); return; }
    std::map<std::string, std::string> vals;
    for (auto& pr : s.props) vals[pr.first] = pr.second;
    for (auto& p : it->second.params)
      if (!vals.count(p)) { errors.push_back(at + "template '" + name + "' needs parameter '" + p + "'"); return; }
    for (auto& kv : vals)
      if (std::find(it->second.params.begin(), it->second.params.end(), kv.first) == it->second.params.end()) {
        errors.push_back(at + "template '" + name + "' has no parameter '" + kv.first + "'");
        return;
      }
    std::vector<Stmt> body = *it->second.body;
    for (auto& b : body) substitute(b, vals);
    stack.push_back(name);
    auto expanded = expand(body, depth + 1);
    stack.pop_back();
    for (auto& e : expanded) out.push_back(std::move(e));
  }
};

// ---- semantic builder ------------------------------------------------------------------------------
Kind kind_from(const std::string& n, bool& known) {
  static const std::pair<const char*, Kind> table[] = {
      {"row", Kind::Row}, {"column", Kind::Column}, {"stack", Kind::Stack}, {"free", Kind::Free}, {"grid", Kind::Grid},
      {"panel", Kind::Panel}, {"waveform", Kind::Waveform}, {"jog", Kind::Jog}, {"button", Kind::Button},
      {"knob", Kind::Knob}, {"fader", Kind::Fader}, {"meter", Kind::Meter}, {"label", Kind::Label},
      {"browser", Kind::Browser}, {"tree", Kind::Tree}, {"pads", Kind::Pads}, {"image", Kind::Image},
      {"spacer", Kind::Spacer}};
  for (auto& e : table)
    if (n == e.first) { known = true; return e.second; }
  known = false;
  return Kind::Unknown;
}

const std::set<std::string>& extra_props() {
  static const std::set<std::string> s = {"orientation", "bipolar", "count", "detents", "steps", "channels", "ticks",
                                          "peak_hold", "zoom", "mode", "row_h", "cue", "sensitivity", "toggle", "kind", "text_on", "args", "columns",
                                          "editable"};
  return s;
}
const std::set<std::string>& formats() {
  static const std::set<std::string> s = {"text", "int", "fixed0", "fixed1", "fixed2", "fixed3", "time", "percent", "signed", "db"};
  return s;
}

class Builder {
 public:
  Skin sk;
  std::vector<Message> msgs;
  bool failed = false;

  void err(int line, const std::string& t) { failed = true; msgs.push_back({Message::Severity::Error, line, t}); }
  void warn(int line, const std::string& t) { msgs.push_back({Message::Severity::Warning, line, t}); }

  // ---- value parsing
  bool num(const std::string& v, float& out) const {
    if (v.empty()) return false;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (end && *end == '\0' && std::isfinite(d)) {
      if (std::fabs(d) > 1e6) return false;
      out = static_cast<float>(d);
      return true;
    }
    auto it = sk.numbers.find(v);
    if (it != sk.numbers.end()) { out = it->second; return true; }
    return false;
  }
  bool floats(const std::string& v, std::vector<float>& out) const {
    size_t a = 0;
    out.clear();
    while (a <= v.size()) {
      size_t c = v.find(',', a);
      float f;
      if (!num(v.substr(a, c == std::string::npos ? std::string::npos : c - a), f)) return false;
      out.push_back(f);
      if (out.size() > 8) return false;
      if (c == std::string::npos) break;
      a = c + 1;
    }
    return true;
  }
  bool color(const std::string& v, Color& out) const {
    if (v == "transparent") { out = 0; return true; }
    if (!v.empty() && v[0] == '#') {
      std::string h = v.substr(1);
      for (char c : h) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
      auto hx = [&](size_t i, size_t n) { return static_cast<uint32_t>(std::stoul(h.substr(i, n), nullptr, 16)); };
      if (h.size() == 3) { out = ((hx(0, 1) * 17) << 24) | ((hx(1, 1) * 17) << 16) | ((hx(2, 1) * 17) << 8) | 0xFF; return true; }
      if (h.size() == 6) { out = (hx(0, 6) << 8) | 0xFF; return true; }
      if (h.size() == 8) { out = hx(0, 8); return true; }
      return false;
    }
    auto it = sk.colors.find(v);
    if (it != sk.colors.end()) { out = it->second; return true; }
    return false;
  }
  bool length(const std::string& v, Length& out) const {
    if (v == "fill") { out = {Length::Unit::Flex, 1}; return true; }
    if (v == "auto") { out = {Length::Unit::Auto, 0}; return true; }
    float f;
    if (v.size() > 1 && v.back() == '%') {
      if (!num(v.substr(0, v.size() - 1), f)) return false;
      out = {Length::Unit::Percent, f};
      return true;
    }
    if (!num(v, f)) return false;
    out = {Length::Unit::Px, f};
    return true;
  }
  bool boolean(const std::string& v, bool& out) const {
    if (v == "true" || v == "yes" || v == "1") { out = true; return true; }
    if (v == "false" || v == "no" || v == "0") { out = false; return true; }
    return false;
  }
  bool size_pair(const std::string& v, float& w, float& h) const {
    size_t x = v.find('x');
    if (x == std::string::npos) return false;
    return num(v.substr(0, x), w) && num(v.substr(x + 1), h);
  }
  bool box4(const std::string& v, std::array<float, 4>& out) const {
    std::vector<float> f;
    if (!floats(v, f)) return false;
    if (f.size() == 1) out = {f[0], f[0], f[0], f[0]};
    else if (f.size() == 2) out = {f[0], f[1], f[0], f[1]};
    else if (f.size() == 4) out = {f[0], f[1], f[2], f[3]};
    else return false;
    return true;
  }

  // ---- style properties; returns true when `key` is a style key (even if its value was bad)
  bool style_prop(StyleSpec& st, const std::string& key, const std::string& v, int line) {
    auto bad = [&] { err(line, "bad value '" + v + "' for " + key); return true; };
    Color c; float f;
    if (key == "color") { if (!color(v, c)) return bad(); st.fg = c; return true; }
    if (key == "bg") { if (!color(v, c)) return bad(); st.bg = c; return true; }
    if (key == "border") { if (!color(v, c)) return bad(); st.border = c; return true; }
    if (key == "glow") { if (!color(v, c)) return bad(); st.glow = c; return true; }
    if (key == "border_w") { if (!num(v, f) || f < 0 || f > 64) return bad(); st.border_w = f; return true; }
    if (key == "radius") { if (!num(v, f) || f < 0 || f > 4096) return bad(); st.radius = f; return true; }
    if (key == "opacity") { if (!num(v, f) || f < 0 || f > 1) return bad(); st.opacity = f; return true; }
    if (key == "scale") { if (!num(v, f) || f <= 0 || f > 8) return bad(); st.scale = f; return true; }
    if (key == "font_size") { if (!num(v, f) || f < 4 || f > 512) return bad(); st.font_size = f; return true; }
    if (key == "glow_r") { if (!num(v, f) || f < 0 || f > 256) return bad(); st.glow_r = f; return true; }
    if (key == "font") { st.font = v; return true; }
    if (key == "sprite") { st.sprite = v; return true; }
    if (key == "text_align") {
      if (v == "left") st.align = TextAlign::Left;
      else if (v == "center") st.align = TextAlign::Center;
      else if (v == "right") st.align = TextAlign::Right;
      else return bad();
      return true;
    }
    return false;
  }

  bool binding(const std::string& v, int line, const char* what) {
    if (!valid_binding(v)) { err(line, std::string("malformed ") + what + " binding '" + v + "'"); return false; }
    return true;
  }

  void node_prop(Node& n, const std::string& key, const std::string& v, int line) {
    auto bad = [&] { err(line, "bad value '" + v + "' for " + key); };
    float f;
    if (style_prop(n.base, key, v, line)) return;
    if (key == "id") n.id = v;
    else if (key == "bind") { if (binding(v, line, "bind")) n.bind = v; }
    else if (key == "bind2") { if (binding(v, line, "bind2")) n.bind2 = v; }
    else if (key == "show") {
      std::string b = !v.empty() && v[0] == '!' ? v.substr(1) : v;
      if (binding(b, line, "show")) n.show = v;
    }
    else if (key == "text" || key == "tooltip") {
      if (!v.empty() && v[0] == '@') {
        for (size_t i = 1; i < v.size(); ++i)
          if (!(std::isalnum(static_cast<unsigned char>(v[i])) || v[i] == '_' || v[i] == '.' || v[i] == '-')) { err(line, "bad string id '" + v + "'"); return; }
      }
      (key == "text" ? n.text : n.tooltip) = v;
    }
    else if (key == "format") { if (!formats().count(v)) warn(line, "unknown format '" + v + "'"); n.format = v; }
    else if (key == "w") { if (!length(v, n.w)) bad(); }
    else if (key == "h") { if (!length(v, n.h)) bad(); }
    else if (key == "x") { if (!length(v, n.x)) bad(); }
    else if (key == "y") { if (!length(v, n.y)) bad(); }
    else if (key == "min_w") { if (!num(v, f) || f < 0) bad(); else n.min_w = f; }
    else if (key == "min_h") { if (!num(v, f) || f < 0) bad(); else n.min_h = f; }
    else if (key == "max_w") { if (!num(v, f) || f < 0) bad(); else n.max_w = f; }
    else if (key == "max_h") { if (!num(v, f) || f < 0) bad(); else n.max_h = f; }
    else if (key == "pad") { if (!box4(v, n.pad)) bad(); }
    else if (key == "margin") { if (!box4(v, n.margin)) bad(); }
    else if (key == "gap") { if (!num(v, f) || f < 0) bad(); else n.gap = f; }
    else if (key == "aspect") { if (!num(v, f) || f <= 0 || f > 100) bad(); else n.aspect = f; }
    else if (key == "flex") { if (!num(v, f) || f < 0 || f > 1000) bad(); else n.flex = f; }
    else if (key == "rotate") { if (!num(v, f)) bad(); else n.rotate = f; }
    else if (key == "transition") { if (!num(v, f) || f < 0 || f > 10000) bad(); else n.transition_ms = f; }
    else if (key == "align") {
      if (v == "stretch") n.align = Align::Stretch; else if (v == "start") n.align = Align::Start;
      else if (v == "center") n.align = Align::Center; else if (v == "end") n.align = Align::End; else bad();
    }
    else if (key == "justify") {
      if (v == "start") n.justify = Justify::Start; else if (v == "center") n.justify = Justify::Center;
      else if (v == "end") n.justify = Justify::End; else if (v == "between") n.justify = Justify::Between; else bad();
    }
    else if (key == "anchor") {
      static const std::pair<const char*, Anchor> t[] = {{"fill", Anchor::Fill}, {"tl", Anchor::TL}, {"t", Anchor::T}, {"tr", Anchor::TR},
          {"l", Anchor::L}, {"c", Anchor::C}, {"r", Anchor::R}, {"bl", Anchor::BL}, {"b", Anchor::B}, {"br", Anchor::BR}};
      bool ok = false;
      for (auto& e : t) if (v == e.first) { n.anchor = e.second; ok = true; }
      if (!ok) bad();
    }
    else if (key == "col" || key == "row" || key == "colspan" || key == "rowspan" || key == "cols" || key == "rows") {
      if (!num(v, f) || f < 0 || f > 64 || f != std::floor(f)) { bad(); return; }
      int i = static_cast<int>(f);
      if ((key == "colspan" || key == "rowspan" || key == "cols" || key == "rows") && i < 1) { bad(); return; }
      if (key == "col") n.col = i; else if (key == "row") n.row = i; else if (key == "colspan") n.colspan = i;
      else if (key == "rowspan") n.rowspan = i; else if (key == "cols") n.cols = i; else n.rows = i;
      if (key == "cols" || key == "rows") n.props[key] = v;
    }
    else if (key == "anim") {
      size_t a = 0;
      while (a <= v.size()) {
        size_t c = v.find(',', a);
        std::string nm = v.substr(a, c == std::string::npos ? std::string::npos : c - a);
        if (!nm.empty()) n.anim_names.push_back(nm);
        if (n.anim_names.size() > 8) { err(line, "too many animations on one widget"); return; }
        if (c == std::string::npos) break;
        a = c + 1;
      }
    }
    else if (key == "shape") n.shape = v;
    else if (extra_props().count(key)) n.props[key] = v;
    else warn(line, "unknown property '" + key + "' on '" + n.type + "' ignored");
  }

  int build_node(const Stmt& s) {
    if (sk.nodes.size() >= limits::kMaxNodes) { err(s.line, "too many layout nodes"); return -1; }
    bool known;
    Kind k = kind_from(s.name, known);
    const int idx = static_cast<int>(sk.nodes.size());
    sk.nodes.emplace_back();
    sk.nodes[idx].kind = k;
    sk.nodes[idx].type = s.name;
    sk.nodes[idx].line = s.line;
    if (!known) {
      warn(s.line, "unknown widget '" + s.name + "' ignored");
      return idx;
    }
    if (!s.args.empty()) warn(s.line, "unexpected bare argument '" + s.args[0] + "' on '" + s.name + "'");
    Node tmp = sk.nodes[idx];
    for (auto& pr : s.props) node_prop(tmp, pr.first, pr.second, s.line);
    std::vector<int> kids;
    for (const Stmt& c : s.children) {
      if (c.name == "state") {
        if (c.args.size() != 1) { err(c.line, "state needs a name"); continue; }
        static const char* names[] = {"normal", "hover", "pressed", "on", "disabled"};
        int si = -1;
        for (int i = 0; i < kStateCount; ++i) if (c.args[0] == names[i]) si = i;
        if (si < 0) { warn(c.line, "unknown state '" + c.args[0] + "'"); continue; }
        StyleSpec& dst = si == 0 ? tmp.base : tmp.state[si];
        auto apply = [&](const std::vector<std::pair<std::string, std::string>>& props, int line) {
          for (auto& pr : props)
            if (!style_prop(dst, pr.first, pr.second, line)) warn(line, "property '" + pr.first + "' not allowed in state; ignored");
        };
        apply(c.props, c.line);
        for (const Stmt& pc : c.children) if (pc.name == "@props") apply(pc.props, pc.line);
        continue;
      }
      if (c.name == "@props") {
        for (auto& pr : c.props) node_prop(tmp, pr.first, pr.second, c.line);
        continue;
      }
      if (!is_container(k)) { warn(c.line, "'" + s.name + "' cannot have children; '" + c.name + "' ignored"); continue; }
      int ci = build_node(c);
      if (ci >= 0) kids.push_back(ci);
    }
    tmp.children = std::move(kids);
    sk.nodes[idx] = std::move(tmp);
    return idx;
  }

  // ---- top-level sections
  void tokens(const Stmt& s) {
    for (const Stmt& c : s.children) {
      if (sk.colors.size() + sk.numbers.size() + sk.fonts.size() >= limits::kMaxTokens) { err(c.line, "too many tokens"); return; }
      if (c.args.empty()) { err(c.line, "token needs a name"); continue; }
      const std::string& nm = c.args[0];
      if (c.name == "color") {
        Color col;
        if (c.args.size() != 2 || !color(c.args[1], col)) err(c.line, "bad color token '" + nm + "'");
        else sk.colors[nm] = col;
      } else if (c.name == "number") {
        float f;
        if (c.args.size() != 2 || !num(c.args[1], f)) err(c.line, "bad number token '" + nm + "'");
        else sk.numbers[nm] = f;
      } else if (c.name == "font") {
        FontToken ft;
        for (auto& pr : c.props) {
          float f; bool b;
          if (pr.first == "family") ft.family = pr.second;
          else if (pr.first == "file") ft.file = pr.second;
          else if (pr.first == "size" && num(pr.second, f) && f >= 4 && f <= 512) ft.size = f;
          else if (pr.first == "bold" && boolean(pr.second, b)) ft.bold = b;
          else if (pr.first == "italic" && boolean(pr.second, b)) ft.italic = b;
          else err(c.line, "bad font property '" + pr.first + "=" + pr.second + "'");
        }
        if (!ft.file.empty()) {
          if (!is_safe_relative_path(ft.file)) err(c.line, "unsafe font path '" + ft.file + "'");
          else if (!allow_files) err(c.line, "single-file skins cannot reference files ('" + ft.file + "')");
        }
        sk.fonts[nm] = ft;
      } else warn(c.line, "unknown token kind '" + c.name + "'");
    }
  }

  void shape(const Stmt& s) {
    if (s.args.size() != 1) { err(s.line, "shape needs a name"); return; }
    if (sk.shapes.size() >= 32) { err(s.line, "too many shapes"); return; }
    Shape sh;
    sh.name = s.args[0];
    if (s.children.size() != 1) { err(s.line, "shape '" + sh.name + "' needs exactly one definition"); return; }
    const Stmt& d = s.children[0];
    if (d.name == "polygon") {
      sh.kind = ShapeKind::Polygon;
      if (d.args.size() < 3 || d.args.size() > limits::kMaxShapePoints) { err(d.line, "polygon needs 3.." + std::to_string(limits::kMaxShapePoints) + " points"); return; }
      for (auto& a : d.args) {
        std::vector<float> f;
        if (!floats(a, f) || f.size() != 2 || f[0] < 0 || f[0] > 1 || f[1] < 0 || f[1] > 1) { err(d.line, "bad polygon point '" + a + "' (want x,y in 0..1)"); return; }
        sh.points.push_back({f[0], f[1]});
      }
    } else if (d.name == "ellipse") sh.kind = ShapeKind::Ellipse;
    else if (d.name == "roundrect" || d.name == "chamfer") {
      sh.kind = d.name == "roundrect" ? ShapeKind::RoundRect : ShapeKind::Chamfer;
      float f = 0;
      for (auto& pr : d.props)
        if (pr.first == "size" || pr.first == "radius") {
          if (!num(pr.second, f) || f < 0 || f > 4096) { err(d.line, "bad shape size"); return; }
        } else warn(d.line, "unknown shape property '" + pr.first + "'");
      sh.amount = f;
    } else { err(d.line, "unknown shape '" + d.name + "'"); return; }
    sk.shapes.push_back(std::move(sh));
  }

  void atlas(const Stmt& s) {
    if (s.args.size() != 1) { err(s.line, "atlas needs a name"); return; }
    if (sk.atlases.size() >= limits::kMaxAtlases) { err(s.line, "too many atlases"); return; }
    Atlas a;
    a.name = s.args[0];
    for (auto& pr : s.props) {
      if (pr.first == "src") a.src = pr.second;
      else warn(s.line, "unknown atlas property '" + pr.first + "'");
    }
    if (!is_safe_relative_path(a.src)) { err(s.line, "unsafe or missing atlas path '" + a.src + "'"); return; }
    if (!allow_files) { err(s.line, "single-file skins cannot reference files ('" + a.src + "')"); return; }
    size_t total = 0;
    for (auto& x : sk.atlases) total += x.regions.size();
    for (const Stmt& r : s.children) {
      if (r.name != "region") { warn(r.line, "unknown atlas entry '" + r.name + "'"); continue; }
      if (++total > limits::kMaxRegions) { err(r.line, "too many atlas regions"); return; }
      Region g;
      float v[4];
      bool ok = r.args.size() == 5;
      if (ok) { g.name = r.args[0]; for (int i = 0; i < 4 && ok; ++i) ok = num(r.args[i + 1], v[i]); }
      if (!ok || v[0] < 0 || v[1] < 0 || v[2] <= 0 || v[3] <= 0 || v[0] + v[2] > 16384 || v[1] + v[3] > 16384) {
        err(r.line, "bad region (want: region name x y w h)");
        continue;
      }
      g.src = {v[0], v[1], v[2], v[3]};
      for (auto& pr : r.props) {
        std::vector<float> f;
        if (pr.first == "slice" && box4(pr.second, g.slice) && g.slice[0] + g.slice[2] <= v[2] && g.slice[1] + g.slice[3] <= v[3]) g.nine_slice = true;
        else err(r.line, "bad region property '" + pr.first + "=" + pr.second + "'");
      }
      a.regions.push_back(std::move(g));
    }
    sk.atlases.push_back(std::move(a));
  }

  void animation(const Stmt& s) {
    if (s.args.size() != 1) { err(s.line, "animation needs a name"); return; }
    if (sk.animations.size() >= limits::kMaxAnimations) { err(s.line, "too many animations"); return; }
    Animation a;
    a.name = s.args[0];
    static const std::set<std::string> propsok = {"opacity", "scale", "offset_x", "offset_y", "rotate", "glow"};
    static const std::set<std::string> triggers = {"always", "hover", "pressed", "on", "beat"};
    for (auto& pr : s.props) {
      float f;
      const std::string& k = pr.first; const std::string& v = pr.second;
      if (k == "property") { if (!propsok.count(v)) err(s.line, "unknown animation property '" + v + "'"); a.property = v; }
      else if (k == "from") { if (!num(v, f)) err(s.line, "bad from"); else a.from = f; }
      else if (k == "to") { if (!num(v, f)) err(s.line, "bad to"); else a.to = f; }
      else if (k == "duration") { if (!num(v, f) || f < 1 || f > 60000) err(s.line, "bad duration (1..60000 ms)"); else a.duration_ms = f; }
      else if (k == "delay") { if (!num(v, f) || f < 0 || f > 60000) err(s.line, "bad delay"); else a.delay_ms = f; }
      else if (k == "ease") {
        if (v == "linear") a.ease = Ease::Linear; else if (v == "in") a.ease = Ease::In;
        else if (v == "out") a.ease = Ease::Out; else if (v == "inout") a.ease = Ease::InOut; else err(s.line, "bad ease '" + v + "'");
      }
      else if (k == "loop") {
        if (v == "once") a.loop = Loop::Once; else if (v == "repeat") a.loop = Loop::Repeat;
        else if (v == "pingpong") a.loop = Loop::PingPong; else err(s.line, "bad loop '" + v + "'");
      }
      else if (k == "trigger") { if (!triggers.count(v)) err(s.line, "unknown trigger '" + v + "'"); a.trigger = v; }
      else warn(s.line, "unknown animation property '" + k + "'");
    }
    if (a.property.empty()) { err(s.line, "animation '" + a.name + "' lacks property="); return; }
    sk.animations.push_back(std::move(a));
  }

  void strings(const Stmt& s) {
    std::string lang;
    for (auto& pr : s.props) {
      if (pr.first == "lang") {
        lang = i18n::normalize_tag(pr.second);
        for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else warn(s.line, "unknown strings property '" + pr.first + "'");
    }
    size_t total = 0;
    for (auto& kv : sk.strings) total += kv.second.size();
    for (const Stmt& c : s.children) {
      if (++total > limits::kMaxStrings) { err(c.line, "too many skin strings"); return; }
      if (c.args.size() != 1) { err(c.line, "string needs: id \"text\""); continue; }
      sk.strings[lang][c.name] = c.args[0];
    }
  }

  void window(const Stmt& s) {
    if (s.args.size() != 1) { err(s.line, "window needs an id"); return; }
    if (sk.windows.size() >= limits::kMaxWindows) { err(s.line, "too many windows"); return; }
    Window w;
    w.id = s.args[0];
    if (sk.find_window(w.id)) { err(s.line, "duplicate window '" + w.id + "'"); return; }
    bool have_size = false;
    for (auto& pr : s.props) {
      if (pr.first == "size") {
        have_size = size_pair(pr.second, w.width, w.height);
        if (!have_size || w.width < limits::kMinWindowDim || w.height < limits::kMinWindowDim || w.width > limits::kMaxWindowDim || w.height > limits::kMaxWindowDim) {
          err(s.line, "bad window size '" + pr.second + "' (" + std::to_string(int(limits::kMinWindowDim)) + ".." + std::to_string(int(limits::kMaxWindowDim)) + ")");
          have_size = false;
          w.width = 1280; w.height = 720;
        }
      } else if (pr.first == "min") {
        if (!size_pair(pr.second, w.min_w, w.min_h) || w.min_w < 0 || w.min_h < 0 || w.min_w > limits::kMaxWindowDim || w.min_h > limits::kMaxWindowDim) err(s.line, "bad min size");
      } else if (pr.first == "shape") w.shape = pr.second;
      else if (pr.first == "title") w.title = pr.second;
      else warn(s.line, "unknown window property '" + pr.first + "'");
    }
    (void)have_size;
    if (s.children.size() != 1) { err(s.line, "window '" + w.id + "' needs exactly one root layout node"); return; }
    w.root = build_node(s.children[0]);
    if (w.root >= 0) sk.windows.push_back(std::move(w));
  }

  void finalize() {
    auto check_style = [&](const StyleSpec& st, int line) {
      if (st.font && !st.font->empty() && !sk.fonts.count(*st.font)) err(line, "unknown font token '" + *st.font + "'");
      if (st.sprite && !st.sprite->empty()) {
        size_t c = st.sprite->find(':');
        const Atlas* a = c == std::string::npos ? nullptr : sk.find_atlas(st.sprite->substr(0, c));
        bool ok = false;
        if (a) for (auto& r : a->regions) if (r.name == st.sprite->substr(c + 1)) ok = true;
        if (!ok) err(line, "unknown sprite '" + *st.sprite + "' (want atlas:region)");
      }
    };
    for (Node& n : sk.nodes) {
      check_style(n.base, n.line);
      for (auto& st : n.state) check_style(st, n.line);
      for (auto& an : n.anim_names) {
        int found = -1;
        for (size_t i = 0; i < sk.animations.size(); ++i) if (sk.animations[i].name == an) found = static_cast<int>(i);
        if (found < 0) err(n.line, "unknown animation '" + an + "'");
        else n.anims.push_back(found);
      }
      if (!n.shape.empty() && !sk.find_shape(n.shape)) err(n.line, "unknown shape '" + n.shape + "'");
    }
    for (auto& w : sk.windows)
      if (!w.shape.empty() && !sk.find_shape(w.shape)) err(0, "window '" + w.id + "' uses unknown shape '" + w.shape + "'");
    if (sk.windows.empty() && !failed) err(0, "skin defines no window");
    if (sk.manifest.name.empty()) err(0, "manifest lacks a name (meta name=...)");
  }

  bool allow_files = true;
};

}  // namespace

ParseResult parse_skin(std::string_view text, const ParseOptions& opt) {
  ParseResult res;
  try {
    Builder b;
    b.allow_files = opt.allow_files;
    auto fail = [&](int line, const std::string& m) {
      b.err(line, m);
      res.messages = std::move(b.msgs);
    };
    if (text.size() > limits::kMaxFileBytes) {
      fail(0, "skin file larger than " + std::to_string(limits::kMaxFileBytes) + " bytes");
      return res;
    }
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::vector<Stmt> raw;
    try {
      Parser p(text);
      raw = p.parse_all();
    } catch (const SyntaxError& e) {
      fail(e.line, e.msg);
      return res;
    }
    // header
    if (raw.empty() || raw[0].name != "dhskin" || raw[0].args.size() != 1) {
      fail(raw.empty() ? 0 : raw[0].line, "file must start with 'dhskin 1'");
      return res;
    }
    if (raw[0].args[0] != "1") {
      fail(raw[0].line, "unsupported skin format version '" + raw[0].args[0] + "'");
      return res;
    }
    // templates
    Expander ex;
    std::vector<Stmt> rest;
    for (size_t i = 1; i < raw.size(); ++i) {
      Stmt& s = raw[i];
      if (s.name == "template") {
        if (s.args.empty() || !s.has_block) { b.err(s.line, "template needs a name and a body"); continue; }
        Template t;
        t.params.assign(s.args.begin() + 1, s.args.end());
        if (ex.tpls.count(s.args[0])) { b.err(s.line, "duplicate template '" + s.args[0] + "'"); continue; }
        t.body = &s.children;
        ex.tpls[s.args[0]] = t;
      } else rest.push_back(std::move(s));
    }
    // template bodies are expanded where used; templates nested in templates are cycles caught there
    std::vector<Stmt> top = ex.expand(rest, 0);
    for (auto& e : ex.errors) {
      int line = 0;
      if (e.rfind("line ", 0) == 0) line = std::atoi(e.c_str() + 5);
      size_t c = e.find(": ");
      b.err(line, c == std::string::npos ? e : e.substr(c + 2));
    }
    if (!ex.errors.empty()) {
      res.messages = std::move(b.msgs);
      return res;
    }
    // unresolved ${...} left over
    std::vector<const Stmt*> todo;
    for (auto& s : top) todo.push_back(&s);
    while (!todo.empty()) {
      const Stmt* s = todo.back(); todo.pop_back();
      auto has = [](const std::string& v) { return v.find("${") != std::string::npos; };
      bool bad = false;
      for (auto& a : s->args) bad |= has(a);
      for (auto& p : s->props) bad |= has(p.second);
      if (bad) b.err(s->line, "unresolved template parameter");
      for (auto& c : s->children) todo.push_back(&c);
    }
    for (const Stmt& s : top) {
      if (s.name == "meta") {
        for (auto& pr : s.props) {
          if (pr.first == "name") b.sk.manifest.name = pr.second;
          else if (pr.first == "author") b.sk.manifest.author = pr.second;
          else if (pr.first == "license") b.sk.manifest.license = pr.second;
          else if (pr.first == "min_version") b.sk.manifest.min_version = pr.second;
          else if (pr.first == "description") b.sk.manifest.description = pr.second;
          else b.warn(s.line, "unknown meta key '" + pr.first + "'");
        }
      } else if (s.name == "tokens") b.tokens(s);
      else if (s.name == "shape") b.shape(s);
      else if (s.name == "atlas") b.atlas(s);
      else if (s.name == "animation") b.animation(s);
      else if (s.name == "strings") b.strings(s);
      else if (s.name == "window") continue;  // after tokens/shapes/animations are known
      else b.warn(s.line, "unknown section '" + s.name + "' ignored");
    }
    for (const Stmt& s : top)
      if (s.name == "window") b.window(s);
    b.finalize();
    res.messages = std::move(b.msgs);
    if (!b.failed) res.skin = std::make_shared<const Skin>(std::move(b.sk));
  } catch (const std::exception& e) {
    res.skin = nullptr;
    res.messages.push_back({Message::Severity::Error, 0, std::string("internal error: ") + e.what()});
  } catch (...) {
    res.skin = nullptr;
    res.messages.push_back({Message::Severity::Error, 0, "internal error"});
  }
  return res;
}

ParseResult load_skin(const std::string& path) {
  ParseResult res;
  auto fail = [&](const std::string& m) {
    res.skin = nullptr;
    res.messages.push_back({Message::Severity::Error, 0, m});
    return res;
  };
  try {
    std::error_code ec;
    fs::path p(path);
    const bool dir = fs::is_directory(p, ec);
    fs::path file = dir ? p / "skin.dhs" : p;
    auto sz = fs::file_size(file, ec);
    if (ec) return fail("cannot open " + file.string());
    if (sz > limits::kMaxFileBytes) return fail("skin file too large");
    std::ifstream f(file, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    ParseOptions opt;
    opt.allow_files = dir;
    res = parse_skin(ss.str(), opt);
    if (!res.ok() || !dir) return res;
    auto mut = std::make_shared<Skin>(*res.skin);
    fs::path root = fs::weakly_canonical(p, ec);
    mut->root_dir = root.string();
    bool bad = false;
    auto check_file = [&](const std::string& rel) {
      fs::path full = fs::weakly_canonical(root / rel, ec);
      const std::string rs = root.string() + "/";
      if (ec || full.string().compare(0, rs.size(), rs) != 0) {
        res.messages.push_back({Message::Severity::Error, 0, "'" + rel + "' resolves outside the skin directory"});
        bad = true;
        return;
      }
      if (!fs::is_regular_file(full, ec)) {
        res.messages.push_back({Message::Severity::Error, 0, "missing file '" + rel + "'"});
        bad = true;
        return;
      }
      if (fs::file_size(full, ec) > limits::kMaxAssetBytes) {
        res.messages.push_back({Message::Severity::Error, 0, "'" + rel + "' is larger than " + std::to_string(limits::kMaxAssetBytes) + " bytes"});
        bad = true;
      }
    };
    for (auto& a : mut->atlases) check_file(a.src);
    for (auto& kv : mut->fonts) if (!kv.second.file.empty()) check_file(kv.second.file);
    if (bad) res.skin = nullptr;
    else res.skin = mut;
  } catch (const std::exception& e) {
    return fail(std::string("internal error: ") + e.what());
  } catch (...) {
    return fail("internal error");
  }
  return res;
}

}  // namespace deckhand::skin
