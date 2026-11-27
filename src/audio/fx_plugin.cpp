#include "fx_plugin.hpp"

#include <cmath>
#include <cstdio>

#include "fx.hpp"

namespace deckhand {

namespace {
void esc(std::string& o, const std::string& s) {
  o += '"';
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20) {
      o += ' ';
    } else {
      o += static_cast<char>(c);
    }
  }
  o += '"';
}
std::string num(double v) {
  if (!std::isfinite(v)) v = 0;
  char b[40];
  std::snprintf(b, sizeof b, "%.9g", v);
  for (char* p = b; *p; ++p)
    if (*p == ',') *p = '.';
  return b;
}
}  // namespace

bool fx_plugin_attach(FxUnit& u, const plugins::PluginInfo& info, double sr, std::string& error) {
  delete u.plug;
  u.plug = new PluginSlot;
  u.plug->info = info;
  u.plug->sr = sr;
  std::string err;
  u.plug->loaded = u.plug->eff.load(info, sr, &err);
  if (!u.plug->loaded) {
    u.plug->error = err.empty() ? "the plug-in did not open" : err;
    error = u.plug->error;
    return false;
  }
  u.plug->eff.set_on(u.on);
  u.plug->eff.set_wet(u.wet);
  return true;
}

void fx_plugin_rate(FxUnit& u, double sr) {
  PluginSlot* s = u.plug;
  if (!s || !s->loaded || s->sr == sr) return;
  std::vector<std::uint8_t> state;
  s->eff.save_state(state);
  std::string err;
  // Device stopped: nothing is running the effect. load() replaces the instance (the old one closes).
  if (!s->eff.load(s->info, sr, &err)) {
    s->loaded = false;
    s->error = err.empty() ? "the plug-in did not reopen at the new sample rate" : err;
    return;
  }
  if (!state.empty()) s->eff.restore_state(state.data(), state.size());
  s->eff.set_on(u.on);
  s->eff.set_wet(u.wet);
  s->sr = sr;
}

void fx_plugin_run(FxUnit& u, float* buf, int n) {
  PluginSlot* s = u.plug;
  if (!s || !s->loaded) return;
  s->eff.set_on(u.on);
  s->eff.set_wet(u.wet);
  s->eff.run(buf, n);
}

bool fx_plugin_busy(const FxUnit& u) { return u.plug && u.plug->loaded && (u.on || u.plug->eff.busy()); }

std::string fx_plugin_info_json(const FxUnit& u) {
  const PluginSlot* s = u.plug;
  std::string o = "{\"id\":";
  esc(o, s ? s->info.id : std::string());
  o += ",\"name\":";
  esc(o, s ? s->info.name : std::string());
  o += ",\"maker\":";
  esc(o, s ? s->info.maker : std::string());
  o += std::string(",\"loaded\":") + (s && s->loaded ? "true" : "false") + ",\"error\":";
  esc(o, s ? s->error : std::string("no plug-in"));
  if (s && s->loaded) {
    plugins::PluginInstance* in = const_cast<PluginSlot*>(s)->eff.instance();
    o += ",\"render_errors\":" + std::to_string(in->render_errors()) + ",\"latency\":" + num(in->latency_seconds()) + ",\"params\":[";
    const auto& ps = in->params();
    for (std::size_t i = 0; i < ps.size(); ++i) {
      const plugins::ParamInfo& p = ps[i];
      if (i) o += ',';
      const double plain = in->get_plain(p.id);
      o += "{\"index\":" + std::to_string(i) + ",\"name\":";
      esc(o, p.name);
      o += ",\"unit\":";
      esc(o, p.unit);
      o += ",\"group\":";
      esc(o, p.group);
      o += ",\"min\":" + num(p.min) + ",\"max\":" + num(p.max) + ",\"default\":" + num(p.def) + ",\"steps\":" + std::to_string(p.steps) +
           ",\"read_only\":" + (p.read_only ? "true" : "false") + ",\"log\":" + (p.logarithmic ? "true" : "false") +
           ",\"norm\":" + num(in->get_norm(p.id)) + ",\"plain\":" + num(plain) + ",\"text\":";
      esc(o, in->value_string(p.id, plain));
      o += ",\"values\":[";
      for (std::size_t k = 0; k < p.value_strings.size() && k < 64; ++k) {
        if (k) o += ',';
        esc(o, p.value_strings[k]);
      }
      o += "]}";
    }
    o += "]";
  } else {
    o += ",\"params\":[]";
  }
  return o + "}";
}

bool fx_plugin_get_state(FxUnit& u, std::string& out, std::string& error) {
  out.clear();
  if (!u.plug || !u.plug->loaded) return error = "the plug-in is not loaded", false;
  std::vector<std::uint8_t> v;
  if (!u.plug->eff.instance()->save_state(v)) return error = "the plug-in could not save its state", false;
  if (v.size() > kPluginBlobMax) return error = "the plug-in state is too large", false;
  out.assign(reinterpret_cast<const char*>(v.data()), v.size());
  return true;
}

bool fx_plugin_set_state(FxUnit& u, const std::string& blob, std::string& error) {
  if (!u.plug || !u.plug->loaded) return error = "the plug-in is not loaded", false;
  if (blob.empty() || blob.size() > kPluginBlobMax) return error = "bad state", false;
  plugins::PluginInstance* in = u.plug->eff.instance();
  if (!in->restore_state(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size())) return error = "the plug-in refused the state", false;
  in->flush_params();
  return true;
}

bool fx_plugin_set_param(FxUnit& u, int index, double norm) {
  if (!u.plug || !u.plug->loaded || index < 0 || index >= u.plug->eff.param_count() || !std::isfinite(norm)) return false;
  u.plug->eff.set_param(index, static_cast<float>(norm < 0 ? 0 : norm > 1 ? 1 : norm));
  return true;
}

}  // namespace deckhand
