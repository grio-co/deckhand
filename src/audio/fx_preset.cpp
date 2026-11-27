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
  std::size_t maxstr = 256;
  std::size_t maxitems = 64;
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
      if (out.size() > maxstr) return fail("string too long");
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
        if (v.o.size() >= maxitems) return fail("too many keys");
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
        if (v.a.size() >= maxitems) return fail("too many items");
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

std::string fx_chain_to_json(const FxChainInfo& c, const FxPresetExtra* extra) {
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
         ",\"beats\":" + num(u.beats) + ",\"seconds\":" + num(u.seconds);
    if (u.kind == EffectKind::Plugin && extra) {
      o += ",\"plugin\":\"";
      for (const char ch : extra->plugin_id[i]) {
        if (ch == '"' || ch == '\\') o += '\\';
        if (static_cast<unsigned char>(ch) >= 0x20) o += ch;
      }
      o += '"';
      if (!extra->state_b64[i].empty()) o += ",\"state\":\"" + extra->state_b64[i] + "\"";
    }
    o += "}";
  }
  o += "]}";
  return o;
}

static bool chain_from_json(const std::string& text, FxChainInfo& out, std::string& error, FxPresetExtra* extra, bool full) {
  out = FxChainInfo{};
  if (text.size() > (full ? (16u << 20) : 64u * 1024)) return error = "preset too large", false;
  Parser p(text);
  if (full) p.maxstr = 12u << 20;
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
    if (k == EffectKind::Plugin && extra) {
      const JV* pid = e.get("plugin");
      if (pid && pid->kind == JV::Str) extra->plugin_id[out.count - 1] = pid->s;
      const JV* st = e.get("state");
      if (full && st && st->kind == JV::Str) extra->state_b64[out.count - 1] = st->s;
    }
  }
  return true;
}

bool fx_chain_from_json(const std::string& text, FxChainInfo& out, std::string& error, FxPresetExtra* extra) {
  return chain_from_json(text, out, error, extra, false);
}
bool fx_chain_from_json_full(const std::string& text, FxChainInfo& out, std::string& error, FxPresetExtra* extra) {
  return chain_from_json(text, out, error, extra, true);
}

std::string base64_encode(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (i + 1 == in.size()) {
    const unsigned v = static_cast<unsigned char>(in[i]) << 16;
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
  } else if (i + 2 == in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
  }
  return o;
}

bool base64_decode(const std::string& in, std::string& out) {
  out.clear();
  unsigned acc = 0;
  int bits = 0;
  for (const char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') break;
    else return false;
    acc = (acc << 6) | static_cast<unsigned>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

// ---- plug-in info ----------------------------------------------------------------------------------------------------
bool parse_plugin_info(const std::string& text, PluginInfoView& out) {
  out = PluginInfoView{};
  if (text.size() > (4u << 20)) return false;
  Parser p(text);
  p.maxitems = 8192;
  JV root;
  if (!p.value(root, 0) || root.kind != JV::Obj) return false;
  const auto str = [](const JV* v) { return v && v->kind == JV::Str ? v->s : std::string(); };
  out.id = str(root.get("id"));
  out.name = str(root.get("name"));
  out.maker = str(root.get("maker"));
  out.error = str(root.get("error"));
  out.loaded = num_of(root.get("loaded"), 0) > 0.5;
  out.render_errors = static_cast<std::uint64_t>(std::max(0.0, num_of(root.get("render_errors"), 0)));
  out.latency = num_of(root.get("latency"), 0);
  if (const JV* ps = root.get("params"); ps && ps->kind == JV::Arr) {
    for (const JV& e : ps->a) {
      if (e.kind != JV::Obj) continue;
      PluginParamView v;
      v.index = static_cast<int>(num_of(e.get("index"), static_cast<double>(out.params.size())));
      v.name = str(e.get("name"));
      v.unit = str(e.get("unit"));
      v.group = str(e.get("group"));
      v.text = str(e.get("text"));
      v.min = num_of(e.get("min"), 0);
      v.max = num_of(e.get("max"), 1);
      v.def = num_of(e.get("default"), 0);
      v.norm = num_of(e.get("norm"), 0);
      v.plain = num_of(e.get("plain"), 0);
      v.steps = static_cast<int>(num_of(e.get("steps"), 0));
      v.read_only = num_of(e.get("read_only"), 0) > 0.5;
      v.log = num_of(e.get("log"), 0) > 0.5;
      if (const JV* vs = e.get("values"); vs && vs->kind == JV::Arr)
        for (const JV& x : vs->a) v.values.push_back(x.kind == JV::Str ? x.s : std::string());
      out.params.push_back(std::move(v));
    }
  }
  return true;
}

// ---- whole chains with plug-in states ------------------------------------------------------------------------------
namespace {
std::string json_string_field(const std::string& j, const char* key) {  // "key":"value" in the plug-in info JSON (no escapes in ids)
  const std::string k = std::string("\"") + key + "\":\"";
  const std::size_t a = j.find(k);
  if (a == std::string::npos) return {};
  const std::size_t b = j.find('"', a + k.size());
  return b == std::string::npos ? std::string() : j.substr(a + k.size(), b - a - k.size());
}
}  // namespace

std::string fx_chain_save_full(IEngine& engine, int target) {
  const FxChainInfo info = engine.fx_chain(target);
  FxPresetExtra extra;
  for (int i = 0; i < info.count && i < kFxChainMax; ++i) {
    if (info.unit[i].kind != EffectKind::Plugin) continue;
    PluginOp op;
    op.target = target;
    op.id = info.unit[i].id;
    PluginOpResult r;
    if (engine.plugin_op(op, r)) extra.plugin_id[i] = json_string_field(r.data, "id");
    op.kind = PluginOpKind::GetState;
    PluginOpResult st;
    if (engine.plugin_op(op, st) && !st.data.empty()) extra.state_b64[i] = base64_encode(st.data);
  }
  return fx_chain_to_json(info, &extra);
}

bool fx_chain_load_full(IEngine& engine, int target, const std::string& text, std::string* error) {
  FxChainInfo spec;
  FxPresetExtra extra;
  std::string err;
  if (!fx_chain_from_json_full(text, spec, err, &extra)) {
    if (error) *error = err;
    return false;
  }
  FxChainEdit e;
  e.verb = FxChainVerb::Load;
  e.target = target;
  // ids only: the states go over the side channel (to_json writes whatever is in the extra, so blank them in a copy)
  FxPresetExtra ids = extra;
  for (auto& s : ids.state_b64) s.clear();
  e.text = fx_chain_to_json(spec, &ids);
  FxChainResult r;
  if (!engine.fx_chain_edit(e, r)) {
    if (error) *error = r.error;
    return false;
  }
  std::string problems;
  for (int i = 0; i < spec.count && i < r.info.count; ++i) {
    if (spec.unit[i].kind != EffectKind::Plugin || extra.state_b64[i].empty()) continue;
    std::string blob;
    PluginOp op;
    op.kind = PluginOpKind::SetState;
    op.target = target;
    op.id = r.info.unit[i].id;
    PluginOpResult pr;
    if (!base64_decode(extra.state_b64[i], blob) || blob.empty()) {
      problems += "unit " + std::to_string(i + 1) + ": bad state; ";
      continue;
    }
    op.data = std::move(blob);
    if (!engine.plugin_op(op, pr)) problems += "unit " + std::to_string(i + 1) + ": " + pr.error + "; ";
  }
  if (error) *error = problems;
  return true;
}

}  // namespace deckhand
