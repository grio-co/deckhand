// Chain presets: FxChainInfo <-> a small JSON text. Self-contained (no locale-dependent number handling, bounded
// depth and size) because the text may come from a file or a network request.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {
namespace {

std::string num(double v) {
  if (!std::isfinite(v)) v = 0;
  char b[40];
  std::snprintf(b, sizeof b, "%.7g", v);
  for (char* p = b; *p; ++p)
    if (*p == ',') *p = '.';  // a locale with a decimal comma
  return b;
}

struct JV {
  enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JV> a;
  std::vector<std::pair<std::string, JV>> o;
  const JV* get(const char* key) const {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

struct Parser {
  const std::string& t;
  std::size_t i = 0;
  std::string err;
  explicit Parser(const std::string& text) : t(text) {}
  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r')) ++i;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
    return false;
  }
  bool str(std::string& out) {
    if (i >= t.size() || t[i] != '"') return fail("expected a string");
    ++i;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c == '\\') {
        if (i >= t.size()) return fail("bad escape");
        const char e = t[i++];
        c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
      }
      if (out.size() > 256) return fail("string too long");
      out += c;
    }
    if (i >= t.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool number(double& v) {
    const std::size_t start = i;
    bool neg = false;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) neg = t[i++] == '-';
    double x = 0;
    int digits = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') x = x * 10 + (t[i++] - '0'), ++digits;
    if (i < t.size() && t[i] == '.') {
      ++i;
      double f = 0.1;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') x += (t[i++] - '0') * f, f *= 0.1, ++digits;
    }
    if (!digits) {
      i = start;
      return fail("expected a number");
    }
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
      ++i;
      bool eneg = false;
      if (i < t.size() && (t[i] == '-' || t[i] == '+')) eneg = t[i++] == '-';
      int e = 0;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9' && e < 400) e = e * 10 + (t[i++] - '0');
      x *= std::pow(10.0, eneg ? -e : e);
    }
    v = neg ? -x : x;
    return std::isfinite(v) || fail("number out of range");
  }
  bool lit(const char* w) {
    std::size_t k = 0;
    while (w[k]) {
      if (i + k >= t.size() || t[i + k] != w[k]) return false;
      ++k;
    }
    i += k;
    return true;
  }
  bool value(JV& v, int depth) {
    if (depth > 6) return fail("nested too deeply");
    ws();
    if (i >= t.size()) return fail("unexpected end");
    const char c = t[i];
    if (c == '{') {
      ++i;
      v.kind = JV::Obj;
      ws();
      if (i < t.size() && t[i] == '}') return ++i, true;
      for (;;) {
        ws();
        std::string key;
        if (!str(key)) return false;
        ws();
        if (i >= t.size() || t[i++] != ':') return fail("expected ':'");
        JV child;
        if (!value(child, depth + 1)) return false;
        if (v.o.size() >= 64) return fail("too many keys");
        v.o.emplace_back(std::move(key), std::move(child));
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == '}') return ++i, true;
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      v.kind = JV::Arr;
      ws();
      if (i < t.size() && t[i] == ']') return ++i, true;
      for (;;) {
        JV child;
        if (!value(child, depth + 1)) return false;
        if (v.a.size() >= 64) return fail("too many items");
        v.a.push_back(std::move(child));
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == ']') return ++i, true;
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      v.kind = JV::Str;
      return str(v.s);
    }
    if (lit("true")) return v.kind = JV::Bool, v.b = true, true;
    if (lit("false")) return v.kind = JV::Bool, v.b = false, true;
    if (lit("null")) return v.kind = JV::Null, true;
    v.kind = JV::Num;
    return number(v.n);
  }
};

double num_of(const JV* v, double def) {
  if (!v) return def;
  if (v->kind == JV::Num) return v->n;
  if (v->kind == JV::Bool) return v->b ? 1 : 0;
  return def;
}

}  // namespace

std::string fx_chain_to_json(const FxChainInfo& c) {
  std::string o = "{\"v\":1,\"bypass\":";
  o += c.bypass ? "true" : "false";
  o += ",\"wet\":" + num(c.wet) + ",\"fx\":[";
  const int n = std::clamp(c.count, 0, kFxChainMax);
  for (int i = 0; i < n; ++i) {
    const FxUnitInfo& u = c.unit[i];
    if (i) o += ',';
    o += "{\"type\":\"";
    o += effect_slug(u.kind);
    o += "\",\"on\":";
    o += u.on ? "true" : "false";
    o += ",\"wet\":" + num(u.wet) + ",\"param\":" + num(u.param) + ",\"p2\":" + num(u.param2) + ",\"p3\":" + num(u.param3) +
         ",\"beats\":" + num(u.beats) + ",\"seconds\":" + num(u.seconds) + "}";
  }
  o += "]}";
  return o;
}

bool fx_chain_from_json(const std::string& text, FxChainInfo& out, std::string& error) {
  out = FxChainInfo{};
  if (text.size() > 64 * 1024) return error = "preset too large", false;
  Parser p(text);
  JV root;
  if (!p.value(root, 0)) return error = p.err, false;
  p.ws();
  if (p.i != text.size()) return error = "trailing text after the preset", false;
  if (root.kind != JV::Obj) return error = "a preset is a JSON object", false;
  if (num_of(root.get("v"), 1) > 1.0) return error = "preset is from a newer version", false;
  const JV* fx = root.get("fx");
  if (!fx || fx->kind != JV::Arr) return error = "preset has no \"fx\" list", false;
  if (fx->a.size() > static_cast<std::size_t>(kFxChainMax)) return error = "a chain holds at most 8 effects", false;
  out.exists = 1;
  out.bypass = num_of(root.get("bypass"), 0) > 0.5;
  out.wet = static_cast<float>(std::clamp(num_of(root.get("wet"), 1), 0.0, 1.0));
  int id = 0;
  for (const JV& e : fx->a) {
    if (e.kind != JV::Obj) return error = "effect entries are objects", false;
    const JV* type = e.get("type");
    if (!type || type->kind != JV::Str) return error = "effect entry without a type", false;
    const EffectKind k = effect_from_slug(type->s);
    if (k == EffectKind::None) return error = "unknown effect \"" + type->s + "\"", false;
    const EffectInfo* info = effect_info(k);
    FxUnitInfo& u = out.unit[out.count++];
    u.id = ++id;
    u.kind = k;
    u.on = num_of(e.get("on"), 0) > 0.5;
    u.wet = static_cast<float>(std::clamp(num_of(e.get("wet"), info->default_wet), 0.0, 1.0));
    u.param = static_cast<float>(std::clamp(num_of(e.get("param"), info->default_param), 0.0, 1.0));
    u.param2 = static_cast<float>(std::clamp(num_of(e.get("p2"), k == EffectKind::Filter ? 0.3 : 0.5), 0.0, 1.0));
    u.param3 = static_cast<float>(std::clamp(num_of(e.get("p3"), 0.5), 0.0, 1.0));
    u.beats = static_cast<float>(std::clamp(num_of(e.get("beats"), info->default_beats), 1.0 / 16, 16.0));
    const double sec = num_of(e.get("seconds"), 0);
    u.seconds = sec > 0 ? std::min(sec, 10.0) : 0;
  }
  return true;
}

}  // namespace deckhand
