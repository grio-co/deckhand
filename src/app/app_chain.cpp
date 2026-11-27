// The effect chain editor: one panel for the chain of a deck, the master, the microphone, the sampler or a stem bus.
// Units can be added (built-in effects by name, scanned plug-ins), removed, reordered, switched on and off, given a wet
// amount and their parameters; chains can be saved and loaded by name; plug-ins can be rescanned. Everything it does goes
// through IEngine::fx_chain_edit / plugin_op, the same calls the controls and the REST routes use.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

#include "app_impl.hpp"
#include "deckhand/app/chain_presets.hpp"
#include "deckhand/app/controls.hpp"
#include "deckhand/app/plugin_service.hpp"

namespace deckhand {

using namespace deckhand::detail;

namespace {
constexpr int kSliderBase = 3000;  // held_ ids of the panel's sliders
constexpr int kPageSize = 8;

std::string percent(double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; }
}  // namespace

void App::open_chain(int target) {
  if (!fx_target_valid(target)) target = kFxMaster;
  chain_target_ = target;
  chain_unit_ = 0;
  chain_page_ = 0;
  chain_adding_ = false;
  chain_pinfo_unit_ = -1;
  if (panel_ != Panel::Chain) open_panel(Panel::Chain);
  chain_presets_cache_ = chain_presets_list();
}

bool App::chain_slider(const Rect& tr, double v, int id, double& out) {
  const ui::Theme& th = theme_;
  v = std::clamp(v, 0.0, 1.0);
  dl_.rrect(tr.x, tr.y + tr.h / 2 - 3, tr.w, 6, 3, rgba(10, 11, 14));
  dl_.rrect(tr.x, tr.y + tr.h / 2 - 3, tr.w * static_cast<float>(v), 6, 3, th.accent);
  dl_.circle(tr.x + tr.w * static_cast<float>(v), tr.y + tr.h / 2, 7, th.text);
  const Rect hit{tr.x - 8, tr.y, tr.w + 16, tr.h};
  if (ov_.pressed && hit.contains(ov_.x, ov_.y)) held_ = kSliderBase + id;
  bool changed = false;
  if (held_ == kSliderBase + id) {
    if (ov_.down) {
      out = std::clamp(static_cast<double>((ov_.x - tr.x) / tr.w), 0.0, 1.0);
      changed = true;
    } else {
      held_ = 0;
    }
  }
  return changed;
}

void App::draw_chain_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 16, rw = m.w - 32;
  float y = m.y + 44;
  const auto edit = [&](FxChainEdit e) {
    e.target = chain_target_;
    FxChainResult r;
    if (!engine_->fx_chain_edit(e, r)) status_note_ = r.error.empty() ? T("status.error", {{"message", "effect chain"}}) : r.error;
    return r;
  };

  // ---- target: deck 1..4, master, mic, sampler, and the stems of a deck
  {
    struct Tg { int target; std::string label; };
    std::vector<Tg> tg;
    for (int d = 0; d < kMaxDecks; ++d) tg.push_back({d, T("chain.deck", {{"n", d + 1}})});
    tg.push_back({kFxMaster, T("chain.master")});
    tg.push_back({kFxTargetMic, T("chain.mic")});
    tg.push_back({kFxTargetSampler, T("chain.sampler")});
    const float bw = (rw - 6.0f * static_cast<float>(tg.size() - 1)) / static_cast<float>(tg.size());
    int deck_of = chain_target_ < kMaxDecks ? chain_target_ : (chain_target_ >= kFxTargetStemBase ? (chain_target_ - kFxTargetStemBase) / kFxStemsPerDeck : -1);
    for (std::size_t i = 0; i < tg.size(); ++i) {
      const bool lit = tg[i].target == chain_target_ || (tg[i].target == deck_of && chain_target_ >= kFxTargetStemBase);
      if (ui_button({x + static_cast<float>(i) * (bw + 6), y, bw, 26}, tg[i].label, lit) && tg[i].target != chain_target_) {
        chain_target_ = tg[i].target;
        chain_unit_ = 0;
        chain_page_ = 0;
        chain_adding_ = false;
        chain_pinfo_unit_ = -1;
      }
    }
    y += 32;
    if (deck_of >= 0) {
      dl_.text(x, y + 5, T("chain.stems"), 12, th.dim);
      float sx = x + 70;
      const float sw = 44;
      if (ui_button({sx, y, sw + 14, 24}, T("chain.stem_mix"), chain_target_ < kMaxDecks)) {
        chain_target_ = deck_of;
        chain_unit_ = 0;
        chain_pinfo_unit_ = -1;
      }
      sx += sw + 20;
      for (int s = 0; s < kFxStemsPerDeck; ++s) {
        const int t = fx_target_stem(deck_of, s);
        if (ui_button({sx, y, 30, 24}, std::to_string(s + 1), chain_target_ == t)) {
          chain_target_ = t;
          chain_unit_ = 0;
          chain_pinfo_unit_ = -1;
        }
        sx += 34;
      }
    }
    y += 32;
  }

  FxChainInfo ci = engine_->fx_chain(chain_target_);
  // ---- chain bypass and wet; plug-in rescan
  {
    FxChainEdit e;
    if (ui_toggle({x, y, 120, 24}, T("chain.bypass"), ci.bypass != 0)) {
      e.verb = FxChainVerb::SetChain;
      e.field = 0;
      e.value = ci.bypass ? 0 : 1;
      edit(e);
    }
    dl_.text(x + 140, y + 5, T("chain.wet"), 12, th.dim);
    double nv = 0;
    if (chain_slider({x + 190, y, 180, 24}, ci.wet, 0, nv)) {
      e = FxChainEdit{};
      e.verb = FxChainVerb::SetChain;
      e.field = 1;
      e.value = nv;
      edit(e);
    }
    dl_.text(x + 380, y + 5, percent(ci.wet), 12, th.text);
    const PluginScanStatus ps = plugin_service().status();
    if (ps.supported) {
      if (ui_button({x + rw - 150, y, 150, 24}, T("chain.rescan"), ps.running) && !ps.running) plugin_service().rescan(true);
      if (ps.running)
        dl_.text(x + 420, y + 5, skin_->fit(T("chain.scanning", {{"done", ps.done}, {"total", ps.total}, {"name", ps.current}}), 11, 0, rw - 580), 11, th.dim, Align::Left, rw - 580);
    }
    y += 34;
  }

  // ---- body: the units on the left, the selected unit (or the add list) on the right
  const float foot_h = 74;
  const Rect body{m.x + 8, y, m.w - 16, m.y + m.h - y - foot_h - 8};
  const float left_w = std::min(260.0f, body.w * 0.4f);
  const float row_h = 30;
  bool selected_found = false;
  for (int i = 0; i < ci.count && i < kFxChainMax; ++i) selected_found = selected_found || ci.unit[i].id == chain_unit_;
  if (!selected_found) chain_unit_ = ci.count > 0 ? ci.unit[0].id : 0;
  {
    float cy = body.y;
    if (ci.count == 0) dl_.text(x, cy + 6, T("chain.empty"), 12, th.dim);
    int move_from = -1, move_to = -1, remove_id = 0;
    for (int i = 0; i < ci.count && i < kFxChainMax; ++i) {
      const FxUnitInfo& u = ci.unit[i];
      const EffectInfo* info = effect_info(u.kind);
      std::string name = info ? info->name : "?";
      if (u.kind == EffectKind::Plugin && u.id == chain_unit_ && chain_pinfo_unit_ == u.id && !chain_pinfo_.name.empty()) name = chain_pinfo_.name;
      const Rect row{x, cy, left_w, row_h - 3};
      const bool sel = u.id == chain_unit_;
      dl_.rrect(row.x, row.y, row.w, row.h, 4, sel ? with_alpha(th.accent, 90) : th.panel2);
      dl_.text(row.x + 8, row.y + 6, skin_->fit(name, 12, 0, left_w - 118), 12, th.text, Align::Left, left_w - 118);
      const Rect on{row.x + left_w - 108, row.y + 3, 30, row.h - 6};
      const bool is_on = u.on != 0;
      dl_.rrect(on.x, on.y, on.w, on.h, 4, is_on ? th.good : th.panel);
      dl_.text(on.x + on.w / 2, on.y + 4, T("chain.on"), 10, is_on ? th.bg : th.dim, Align::Center, on.w);
      if (ov_.pressed && on.contains(ov_.x, ov_.y)) {
        FxChainEdit e;
        e.verb = FxChainVerb::Set;
        e.id = u.id;
        e.field = static_cast<int>(FxField::On);
        e.value = is_on ? 0 : 1;
        edit(e);
      } else if (ov_.pressed && Rect{row.x, row.y, left_w - 112, row.h}.contains(ov_.x, ov_.y)) {
        chain_unit_ = u.id;
        chain_page_ = 0;
        chain_adding_ = false;
        chain_pinfo_unit_ = -1;
      }
      if (i > 0 && ui_button({row.x + left_w - 74, row.y + 3, 22, row.h - 6}, "\xE2\x86\x91", false)) {
        move_from = u.id;
        move_to = i - 1;
      }
      if (i + 1 < ci.count && ui_button({row.x + left_w - 50, row.y + 3, 22, row.h - 6}, "\xE2\x86\x93", false)) {
        move_from = u.id;
        move_to = i + 1;
      }
      if (ui_button({row.x + left_w - 26, row.y + 3, 22, row.h - 6}, "x", false)) remove_id = u.id;
      cy += row_h;
    }
    if (move_from) {
      FxChainEdit e;
      e.verb = FxChainVerb::Move;
      e.id = move_from;
      e.pos = move_to;
      edit(e);
    }
    if (remove_id) {
      FxChainEdit e;
      e.verb = FxChainVerb::Remove;
      e.id = remove_id;
      edit(e);
    }
    if (ci.count < kFxChainMax) {
      if (ui_button({x, cy + 4, left_w, 28}, T("chain.add"), chain_adding_)) chain_adding_ = !chain_adding_;
    } else {
      dl_.text(x, cy + 10, skin_->fit(T("chain.full"), 11, 0, left_w), 11, th.dim, Align::Left, left_w);
    }
  }

  const float rx = x + left_w + 18, rwid = rw - left_w - 18;
  if (chain_adding_) {
    // built-in effects, then the plug-ins that passed their probe
    const std::vector<PluginRow> plugs = plugin_service().list();
    int vetted = 0;
    for (const PluginRow& p : plugs) vetted += p.vetted ? 1 : 0;
    const int builtin = effect_count();
    const float item_h = 26;
    const float total = item_h * static_cast<float>(builtin + 2 + vetted + 1);
    const Rect list{rx - 6, body.y, rwid + 12, body.h};
    if (list.contains(ov_.x, ov_.y)) chain_add_scroll_ -= ov_.scroll;
    chain_add_scroll_ = std::clamp(chain_add_scroll_, 0.0f, std::max(0.0f, total - list.h));
    dl_.push_clip(list.x, list.y, list.w, list.h);
    float cy = list.y - chain_add_scroll_;
    dl_.text(rx, cy + 5, T("chain.add_builtin"), 11, th.dim);
    cy += item_h;
    const float cols = 2, cw = (rwid - 8) / cols;
    int c = 0;
    for (int i = 0; i < builtin; ++i) {
      const EffectInfo& e = effect_info_at(i);
      if (ui_button({rx + static_cast<float>(c) * (cw + 8), cy, cw, item_h - 3}, e.name, false)) {
        FxChainEdit ed;
        ed.verb = FxChainVerb::Insert;
        ed.kind = e.kind;
        ed.value = 1;
        const FxChainResult r = edit(ed);
        if (r.ok) {
          chain_unit_ = r.id;
          chain_adding_ = false;
          chain_pinfo_unit_ = -1;
        }
      }
      if (++c == 2) {
        c = 0;
        cy += item_h;
      }
    }
    if (c) cy += item_h;
    cy += 4;
    dl_.text(rx, cy + 5, T("chain.add_plugins"), 11, th.dim);
    cy += item_h;
    if (!plugin_service().status().supported) {
      dl_.text(rx, cy + 5, skin_->fit(T("chain.plugins_unsupported"), 11, 0, rwid), 11, th.dim, Align::Left, rwid);
    } else if (vetted == 0) {
      dl_.text(rx, cy + 5, skin_->fit(T("chain.no_plugins"), 11, 0, rwid), 11, th.dim, Align::Left, rwid);
    }
    for (const PluginRow& p : plugs) {
      if (!p.vetted) continue;
      if (ui_button({rx, cy, rwid, item_h - 3}, p.maker.empty() ? p.name : p.name + "  -  " + p.maker, false)) {
        FxChainEdit ed;
        ed.verb = FxChainVerb::Insert;
        ed.kind = EffectKind::Plugin;
        ed.text = p.id;
        ed.value = 1;
        const FxChainResult r = edit(ed);
        if (r.ok) {
          chain_unit_ = r.id;
          chain_adding_ = false;
          chain_pinfo_unit_ = -1;
        }
      }
      cy += item_h;
    }
    dl_.pop_clip();
  } else if (chain_unit_ != 0) {
    const FxUnitInfo* u = nullptr;
    for (int i = 0; i < ci.count && i < kFxChainMax; ++i)
      if (ci.unit[i].id == chain_unit_) u = &ci.unit[i];
    if (u) {
      float cy = body.y;
      const EffectInfo* info = effect_info(u->kind);
      const auto set_field = [&](FxField f, double v) {
        FxChainEdit e;
        e.verb = FxChainVerb::Set;
        e.id = u->id;
        e.field = static_cast<int>(f);
        e.value = v;
        edit(e);
      };
      const float lab_w = 130, val_w = 100;
      const auto row = [&](const std::string& label, double v, int id, const std::string& text, double& out) {
        dl_.text(rx, cy + 5, skin_->fit(label, 12, 0, lab_w), 12, th.dim, Align::Left, lab_w);
        const bool ch = chain_slider({rx + lab_w + 6, cy, rwid - lab_w - val_w - 14, 24}, v, id, out);
        dl_.text(rx + rwid - val_w, cy + 5, skin_->fit(text, 11, 0, val_w), 11, th.text, Align::Left, val_w);
        cy += 28;
        return ch;
      };
      double nv = 0;
      dl_.text(rx, cy + 2, skin_->fit(u->kind == EffectKind::Plugin && !chain_pinfo_.name.empty() ? chain_pinfo_.name : (info ? info->name : ""), 14, 1, rwid), 14, th.text, Align::Left, rwid, 1);
      cy += 28;
      if (row(T("chain.wet"), u->wet, 1, percent(u->wet), nv)) set_field(FxField::Wet, nv);
      if (u->kind == EffectKind::Plugin) {
        const double now = now_;
        if (chain_pinfo_unit_ != u->id || chain_pinfo_target_ != chain_target_ || (chain_pinfo_dirty_ && now - chain_pinfo_at_ > 0.15) || now - chain_pinfo_at_ > 2.0) {
          PluginOp op;
          op.target = chain_target_;
          op.id = u->id;
          PluginOpResult pr;
          PluginInfoView pv;
          if (engine_->plugin_op(op, pr) && parse_plugin_info(pr.data, pv)) {
            const int page_keep = chain_pinfo_unit_ == u->id ? chain_page_ : 0;
            chain_pinfo_ = std::move(pv);
            chain_page_ = page_keep;
          } else {
            chain_pinfo_ = PluginInfoView{};
            chain_pinfo_.error = pr.error;
          }
          chain_pinfo_unit_ = u->id;
          chain_pinfo_target_ = chain_target_;
          chain_pinfo_at_ = now;
          chain_pinfo_dirty_ = false;
        }
        if (!chain_pinfo_.loaded) {
          dl_.text(rx, cy + 4, skin_->fit(T("chain.plugin_error", {{"message", chain_pinfo_.error}}), 11, 0, rwid), 11, th.warn, Align::Left, rwid);
        } else {
          const int n = static_cast<int>(chain_pinfo_.params.size());
          const int pages = std::max(1, (n + kPageSize - 1) / kPageSize);
          chain_page_ = std::clamp(chain_page_, 0, pages - 1);
          for (int k = chain_page_ * kPageSize; k < std::min(n, (chain_page_ + 1) * kPageSize); ++k) {
            const PluginParamView& p = chain_pinfo_.params[static_cast<std::size_t>(k)];
            std::string text = p.text;
            if (!p.unit.empty() && text.find(p.unit) == std::string::npos) text += " " + p.unit;
            if (row(p.name, p.norm, 10 + k, text, nv) && !p.read_only) {
              FxChainEdit e;
              e.verb = FxChainVerb::Set;
              e.id = u->id;
              e.field = kFxFieldPluginParam + p.index;
              e.value = nv;
              edit(e);
              chain_pinfo_dirty_ = true;
            }
          }
          if (pages > 1) {
            const float py = body.y + body.h - 30;
            if (ui_button({rx, py, 30, 24}, "<", false)) chain_page_ = std::max(0, chain_page_ - 1);
            dl_.text(rx + 130, py + 5, T("chain.page", {{"page", chain_page_ + 1}, {"pages", pages}}), 12, th.dim, Align::Center, 160);
            if (ui_button({rx + 220, py, 30, 24}, ">", false)) chain_page_ = std::min(pages - 1, chain_page_ + 1);
          }
        }
      } else if (info) {
        if (info->param_name[0] && row(info->param_name, u->param, 2, percent(u->param), nv)) set_field(FxField::Param, nv);
        if (info->param2_name[0] && row(info->param2_name, u->param2, 3, percent(u->param2), nv)) set_field(FxField::Param2, nv);
        if (info->param3_name[0] && row(info->param3_name, u->param3, 4, percent(u->param3), nv)) set_field(FxField::Param3, nv);
        if (info->beat_synced) {  // beat division: halve / double
          dl_.text(rx, cy + 5, T("chain.beat_division"), 12, th.dim);
          const std::string txt = u->beats >= 1 ? fmt("%g", static_cast<double>(u->beats)) : "1/" + fmt("%g", std::round(1.0 / static_cast<double>(u->beats)));
          if (ui_button({rx + lab_w + 6, cy, 30, 24}, "-", false)) set_field(FxField::Beats, std::max(1.0 / 16, u->beats / 2.0));
          dl_.text(rx + lab_w + 66, cy + 5, txt, 12, th.text, Align::Center, 50);
          if (ui_button({rx + lab_w + 100, cy, 30, 24}, "+", false)) set_field(FxField::Beats, std::min(16.0, u->beats * 2.0));
          cy += 28;
        }
      }
    }
  } else if (ci.count == 0) {
    dl_.text(rx, body.y + 6, skin_->fit(T("chain.select"), 12, 0, rwid), 12, th.dim, Align::Left, rwid);
  }

  // ---- presets
  {
    float py = m.y + m.h - foot_h;
    dl_.rect(m.x + 8, py - 4, m.w - 16, 1, th.line);
    dl_.text(x, py + 5, T("chain.preset"), 12, th.dim);
    ui_field({x + 70, py, 200, 24}, f_chain_preset_, T("chain.preset_name"));
    if (ui_button({x + 280, py, 80, 24}, T("chain.preset_save"), false)) {
      std::string err;
      const std::string clean = chain_preset_clean_name(f_chain_preset_.text);
      if (chain_preset_save(clean, fx_chain_save_full(*engine_, chain_target_), err)) {
        status_note_ = T("chain.preset_saved", {{"name", clean}});
        chain_presets_cache_ = chain_presets_list();
      } else {
        status_note_ = err;
      }
    }
    py += 32;
    if (chain_presets_cache_.empty()) dl_.text(x, py + 5, T("chain.presets_none"), 11, th.dim);
    float px = x;
    std::string load_name, del_name;
    for (const std::string& name : chain_presets_cache_) {
      const float w = std::min(150.0f, 28.0f + 7.0f * static_cast<float>(name.size()));
      if (px + w + 26 > x + rw) break;
      if (ui_button({px, py, w, 24}, name, false)) load_name = name;
      if (ui_button({px + w, py, 22, 24}, "x", false)) del_name = name;
      px += w + 30;
    }
    if (!load_name.empty()) {
      std::string body_text, err, note;
      if (chain_preset_read(load_name, body_text, err) && fx_chain_load_full(*engine_, chain_target_, body_text, &note)) {
        status_note_ = note.empty() ? T("chain.preset_loaded", {{"name", load_name}}) : T("chain.preset_notes", {{"name", load_name}, {"notes", note}});
        chain_unit_ = 0;
        chain_pinfo_unit_ = -1;
      } else {
        status_note_ = note.empty() ? err : note;
      }
    }
    if (!del_name.empty()) {
      std::string err;
      chain_preset_delete(del_name, err);
      chain_presets_cache_ = chain_presets_list();
    }
  }
}

}  // namespace deckhand
