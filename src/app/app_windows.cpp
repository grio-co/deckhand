// Free-form windows and modules: the arrangement of a skin's windows (default placement, persistence, snapping and group
// moves), per-window frames and input for multi-window shells, hit testing for transparent windows, the single-window
// fallback that docks the modules into one canvas, and the debug queries behind GET /v1/debug/*.
#include "app_impl.hpp"

#include <cstring>

#include "deckhand/gfx/png.hpp"

namespace deckhand {

namespace {
constexpr float kDefaultMainW = 1440, kDefaultMainH = 900;  // the classic opening size of a system-chrome main window

void copy_text(char* dst, std::size_t cap, const std::string& s) {
  const std::size_t n = std::min(cap - 1, s.size());
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (static_cast<unsigned char>(c) < 32) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o + "\"";
}
}  // namespace

const char* App::panel_role(Panel p) {
  switch (p) {
    case Panel::Settings: return "settings";
    case Panel::Sampler: return "sampler";
    case Panel::Learn: return "learn";
    case Panel::Tags: return "tags";
    case Panel::Relink: return "relink";
    case Panel::Export: return "export";
    case Panel::AddTo: return "add_to";
    case Panel::CueEdit: return "cue";
    case Panel::Columns: return "columns";
    case Panel::Smart: return "smart";
    case Panel::Shortcuts: return "shortcuts";
    case Panel::Tools: return "tools";
    case Panel::Suggest: return "suggest";
    case Panel::PrepMode: return "prep";
    case Panel::Controller: return "controller";
    case Panel::None: break;
  }
  return "";
}

Win* App::find_win(std::uint32_t id) {
  for (auto& w : wins_)
    if (w->id == id) return w.get();
  return nullptr;
}

Win* App::find_win_role(const std::string& role) {
  for (auto& w : wins_)
    if (!w->panel && w->role == role) return w.get();
  return nullptr;
}

static const skin::Window* win_def(const skin::Skin* sk, const Win& w) {
  return sk && w.skin_index >= 0 && static_cast<std::size_t>(w.skin_index) < sk->windows.size() ? &sk->windows[static_cast<std::size_t>(w.skin_index)] : nullptr;
}

float App::win_eff_scale(const Win& w) const {
  const skin::Window* def = win_def(skin_ ? skin_->skin() : nullptr, w);
  return std::max(0.1f, w.backing * (def && def->scale_fit ? w.st.scale : 1.0f));
}

// ---- arrangement ---------------------------------------------------------------------------------------------
void App::load_win_state(Win& w) {
  const skin::Skin* sk = skin_->skin();
  const skin::Window* def = win_def(sk, w);
  if (!def) return;
  const bool keep_pos = w.st.have_pos && w.role == "main";  // switching between main and four keeps where the window is
  const float kx = w.st.x, ky = w.st.y;
  w.st = skin::WinState{};
  w.placed = false;
  w.st.scale = def->scale_default;
  w.st.visible = def->visible || w.role == "main";
  if (keep_pos) {
    w.st.have_pos = true;
    w.st.x = kx;
    w.st.y = ky;
  }
  skin::WinState saved;
  if (!arrangement_reset_ && skin::decode_win_state(lib_->setting(skin::win_setting_key(skin_name_, def->id)), saved)) {
    w.st.scale = skin::clamp_user_scale(*def, saved.scale);
    w.st.w = saved.w;
    w.st.h = saved.h;
    w.st.z = saved.z;
    w.st.parent = saved.parent;
    if (w.role != "main") w.st.visible = saved.visible;
    if (saved.have_pos) {
      w.st.have_pos = true;
      w.placed = true;
      w.st.x = saved.x;
      w.st.y = saved.y;
    }
  }
  if (def->scale_fit) {
    const skin::Size sz = skin::window_size(*def, w.st.scale);
    w.pt_w = sz.w;
    w.pt_h = sz.h;
  } else {
    float sw = w.st.w > 0 ? w.st.w : def->width, sh = w.st.h > 0 ? w.st.h : def->height;
    if (w.role == "main" && def->chrome == skin::Chrome::System && w.st.w <= 0) {
      sw = std::max(sw, kDefaultMainW);
      sh = std::max(sh, kDefaultMainH);
    }
    const skin::Size sz = skin::clamp_free_size(*def, sw, sh);
    w.pt_w = sz.w;
    w.pt_h = sz.h;
  }
  w.st.z = std::max(w.st.z, 0);
  z_counter_ = std::max(z_counter_, w.st.z + 1);
}

void App::save_win_state(Win& w) {
  const skin::Window* def = win_def(skin_ ? skin_->skin() : nullptr, w);
  if (!def || !lib_) return;
  skin::WinState s = w.st;
  if (!def->scale_fit) {
    s.w = w.pt_w;
    s.h = w.pt_h;
  }
  lib_->set_setting(skin::win_setting_key(skin_name_, def->id), skin::encode_win_state(s));
  w.dirty = false;
}

void App::bind_window(Win& w, int skin_index) {
  const skin::Skin* sk = skin_->skin();
  const skin::Window& def = sk->windows[static_cast<std::size_t>(skin_index)];
  w.skin_index = skin_index;
  w.name = def.id;
  w.role = def.role;
  w.panel = false;
  w.view = std::make_unique<ui::SkinView>(dl_, i18n_);
  w.view->share_assets(*skin_);
  w.view->set_touch(touch_);
  w.shape_built = false;
  w.shape = skin::WindowShape{};
  load_win_state(w);
  w.title = skin::resolve_text(*sk, i18n_, def.title.empty() ? "@app.title" : def.title);
  ++w.move_seq;
}

void App::sync_windows() {
  if (!skin_ || !skin_->loaded() || legacy_) {
    // The code-drawn fallback UI: one plain main window.
    if (wins_.empty() || wins_.front()->skin_index != -1 || wins_.front()->view) {
      wins_.erase(std::remove_if(wins_.begin(), wins_.end(), [](const auto& w) { return !w->panel; }), wins_.end());
      auto w = std::make_unique<Win>();
      w->id = next_win_id_++;
      w->name = w->role = "main";
      w->pt_w = kDefaultMainW;
      w->pt_h = kDefaultMainH;
      w->st.visible = true;
      wins_.insert(wins_.begin(), std::move(w));
    }
    return;
  }
  const skin::Skin* sk = skin_->skin();
  const std::string main_id = skin_->has_window(variant_) ? variant_ : "main";
  if (arr_gen_ == skin_gen_ && arr_variant_ == main_id && !wins_.empty()) return;
  arr_gen_ = skin_gen_;
  arr_variant_ = main_id;
  // Which skin windows are wanted now: the active main layout and every module.
  std::vector<int> want;
  for (std::size_t i = 0; i < sk->windows.size(); ++i) {
    const skin::Window& d = sk->windows[i];
    if (d.role == "main" && d.id != main_id) continue;
    want.push_back(static_cast<int>(i));
  }
  std::vector<std::unique_ptr<Win>> next;
  for (const int i : want) {
    const skin::Window& d = sk->windows[static_cast<std::size_t>(i)];
    std::unique_ptr<Win> keep;
    for (auto& w : wins_)  // a window with this role survives a skin or layout change (the native window stays)
      if (w && !w->panel && w->role == d.role) {
        keep = std::move(w);
        break;
      }
    if (!keep) {
      keep = std::make_unique<Win>();
      keep->id = next_win_id_++;
    }
    bind_window(*keep, i);
    next.push_back(std::move(keep));
  }
  for (auto& w : wins_)
    if (w && w->panel) next.push_back(std::move(w));
  wins_ = std::move(next);
  arrangement_reset_ = false;
}

// Modules without a position yet open next to the window they are placed relative to.
void App::place_modules() {
  const skin::Skin* sk = skin_ ? skin_->skin() : nullptr;
  if (!sk) return;
  for (auto& w : wins_) {
    const skin::Window* def = win_def(sk, *w);
    if (!def || w->role == "main" || w->placed) continue;
    skin::Placement p = def->place;
    if (!p.set) {
      p.ref = "main";
      p.side = skin::Placement::Side::Right;
      p.dx = 16;
    }
    Win* ref = nullptr;
    for (auto& o : wins_)
      if (o.get() != w.get() && !o->panel && (o->name == p.ref || o->role == p.ref)) ref = o.get();
    if (!ref || !ref->st.have_pos) continue;
    const skin::Point at = skin::place_relative(p, {ref->st.x, ref->st.y, ref->pt_w, ref->pt_h}, w->pt_w, w->pt_h);
    w->st.x = at.x;
    w->st.y = at.y;
    w->st.have_pos = true;
    w->placed = true;
    w->st.parent = def->snap ? ref->name : std::string();
    ++w->move_seq;
  }
}

bool App::module_known(const std::string& role) const {
  for (const auto& w : wins_)
    if (!w->panel && w->role == role && w->role != "main") return true;
  return false;
}

bool App::module_visible(const std::string& role) const {
  for (const auto& w : wins_)
    if (!w->panel && w->role == role) return w->role == "main" || w->st.visible;
  return false;
}

void App::set_module_visible(const std::string& role, int how) {
  Win* w = find_win_role(role);
  if (!w || w->role == "main") return;
  const bool now = how == 2 ? !w->st.visible : how == 1;
  if (now == w->st.visible) return;
  w->st.visible = now;
  if (now) {
    w->st.z = z_counter_++;
    ++w->move_seq;
  }
  save_win_state(*w);
}

void App::do_window_action(const std::string& action) {
  if (action == "window.reset") {
    windows_reset();
    return;
  }
  const std::size_t colon = action.find(':');
  if (colon == std::string::npos) return;
  const std::string verb = action.substr(0, colon), role = action.substr(colon + 1);
  if (!module_known(role)) {
    status_note_ = T("status.window_missing", {{"name", role}});
    return;
  }
  set_module_visible(role, verb == "window.toggle" ? 2 : verb == "window.show" ? 1 : 0);
}

void App::windows_reset() {
  if (!skin_ || !skin_->loaded()) return;
  arrangement_reset_ = true;
  for (auto& w : wins_) {
    if (w->panel) continue;
    if (const skin::Window* def = win_def(skin_->skin(), *w)) lib_->set_setting(skin::win_setting_key(skin_name_, def->id), "");
    w->st.have_pos = w->role == "main" ? w->st.have_pos : false;
    w->placed = false;
  }
  arr_gen_ = ~0u;
  sync_windows();
  for (auto& w : wins_)
    if (w->role == "main" && !w->panel) ++w->move_seq;
}

// ---- panels as windows -----------------------------------------------------------------------------------------------
void App::sync_panel_window() {
  Win* pw = nullptr;
  for (auto& w : wins_)
    if (w->panel) pw = w.get();
  const char* role = panel_role(panel_);
  const bool hosted = panel_ != Panel::None && module_known(role);  // the skin has a window of that role: it hosts the panel
  if (panel_ == Panel::None || hosted) {
    if (pw) wins_.erase(std::remove_if(wins_.begin(), wins_.end(), [](const auto& w) { return w->panel; }), wins_.end());
    return;
  }
  std::string title;
  const Rect m = panel_size(100000, 100000, title);
  bool fresh = false;
  if (!pw) {
    auto w = std::make_unique<Win>();
    w->id = next_win_id_++;
    w->name = "panel";
    w->panel = true;
    w->st.visible = true;
    pw = w.get();
    wins_.push_back(std::move(w));
    fresh = true;
  }
  if (fresh || pw->role != role || std::fabs(pw->pt_w - m.w) > 0.5f || std::fabs(pw->pt_h - m.h) > 0.5f) {
    pw->role = role;
    pw->pt_w = m.w;
    pw->pt_h = m.h;
    pw->title = title;
    pw->popup = panel_ == Panel::Export || panel_ == Panel::AddTo;
    pw->st.have_pos = false;
    pw->has_anchor = pw->popup && panel_anchor_valid_;
    pw->anchor_x = panel_anchor_x_;
    pw->anchor_y = panel_anchor_y_;
    const Win* main = nullptr;
    for (const auto& o : wins_)
      if (!o->panel && o->role == "main") main = o.get();
    if (pw->has_anchor) {
      pw->st.x = pw->anchor_x;
      pw->st.y = pw->anchor_y;
      pw->st.have_pos = true;
    } else if (main && main->st.have_pos) {
      pw->st.x = main->st.x + (main->pt_w - pw->pt_w) / 2;
      pw->st.y = main->st.y + (main->pt_h - pw->pt_h) / 3;
      pw->st.have_pos = true;
    }
    pw->placed = true;
    pw->st.z = z_counter_++;
    ++pw->move_seq;
  }
}

// ---- the multi-window frame ---------------------------------------------------------------------------------------------
void App::begin_frame(double) {
  prepare_frame();
  controller_sync(now_seconds());
  serve_debug_requests();
  sync_windows();
  place_modules();
  sync_panel_window();
  any_down_ = false;
  for (const auto& w : wins_) any_down_ = any_down_ || w->raw.down;
  for (auto& w : wins_)  // moves and resizes are saved a moment after they stop
    if (w->dirty && now_ - w->dirty_at > 0.5 && !w->in_drag) save_win_state(*w);
}

std::uint32_t App::window_list(dh_window_info* out, std::uint32_t cap) {
  const skin::Skin* sk = skin_ && skin_->loaded() ? skin_->skin() : nullptr;
  std::uint32_t n = 0;
  for (auto& wp : wins_) {
    const Win& w = *wp;
    if (n < cap && out) {
      dh_window_info& o = out[n];
      o = dh_window_info{};
      const skin::Window* def = win_def(sk, w);
      o.id = w.id;
      copy_text(o.name, sizeof o.name, w.name);
      copy_text(o.role, sizeof o.role, w.role);
      copy_text(o.title, sizeof o.title, w.title.empty() ? T("app.title") : w.title);
      std::uint32_t f = 0;
      const bool hosted = !w.panel && panel_ != Panel::None && w.role == panel_role(panel_) && w.role != "main";
      if (w.role == "main" || w.panel || ((w.st.visible || hosted) && w.placed)) f |= DH_WIN_VISIBLE;  // a module opens once it knows where
      if (w.role == "main" && !w.panel) f |= DH_WIN_MAIN;
      if (w.panel) f |= DH_WIN_PANEL | (w.popup ? DH_WIN_POPUP : 0);
      if (def) {
        if (def->chrome == skin::Chrome::None) f |= DH_WIN_BORDERLESS;
        if (def->transparent) f |= DH_WIN_TRANSPARENT;
        if (def->transparent || !def->mask.empty() || !def->shape.empty()) f |= DH_WIN_SHAPED;
        if (def->floating) f |= DH_WIN_FLOATING;
        if (def->resizable && def->chrome == skin::Chrome::System) f |= DH_WIN_RESIZABLE;
        if (!def->shadow) f |= DH_WIN_NO_SHADOW;
        if (def->aspect_lock) f |= DH_WIN_ASPECT_LOCK;
        o.user_scale = w.st.scale;
        if (def->scale_fit) {
          o.min_w = def->width * def->scale_min;
          o.min_h = def->height * def->scale_min;
          o.max_w = def->width * def->scale_max;
          o.max_h = def->height * def->scale_max;
        } else {
          o.min_w = def->min_w;
          o.min_h = def->min_h;
          o.max_w = def->max_w;
          o.max_h = def->max_h;
        }
      } else {
        o.user_scale = 1;
        if (w.panel) f |= DH_WIN_RESIZABLE * 0;
      }
      if (w.st.have_pos && (w.role == "main" || w.placed || w.panel)) f |= DH_WIN_POS_VALID;
      o.flags = f;
      o.width = w.pt_w;
      o.height = w.pt_h;
      o.x = w.st.x;
      o.y = w.st.y;
      o.move_seq = w.move_seq;
      o.z = static_cast<std::uint32_t>(std::max(0, w.st.z));
    }
    ++n;
  }
  return n;
}

void App::window_display(std::uint32_t id, float w_px, float h_px, float backing) {
  Win* w = find_win(id);
  if (!w) return;
  w->px_w = w_px;
  w->px_h = h_px;
  w->backing = backing > 0 ? backing : 1;
}

void App::ensure_shape(Win& w) {
  if (w.shape_built) return;
  w.shape_built = true;
  const skin::Skin* sk = skin_ ? skin_->skin() : nullptr;
  const skin::Window* def = win_def(sk, w);
  if (!def) return;
  skin::Mask mask;
  if (!def->mask.empty() && !sk->root_dir.empty() && skin::is_safe_relative_path(def->mask)) {
    gfx::Image img;
    std::string why;
    if (gfx::load_png_file(sk->root_dir + "/" + def->mask, img, why)) {
      if (!skin::mask_from_rgba(img.rgba.data(), img.width, img.height, mask, why)) DH_LOG(Warn, "window '%s' mask: %s", def->id.c_str(), why.c_str());
    } else {
      DH_LOG(Warn, "window '%s' mask: %s", def->id.c_str(), why.c_str());
    }
  }
  w.shape = skin::make_window_shape(*sk, *def, std::move(mask));
}

void App::draw_window_pass(Win& w) {
  const float eff = win_eff_scale(w);
  // Swap the pass context in: every drawing routine reads the window size, scale and pointer from these members.
  const float sv_w = width_px_, sv_h = height_px_, sv_s = scale_;
  const ui::Input sv_raw = raw_, sv_in = in_;
  const bool sv_ov_fresh = panel_fresh_;
  (void)sv_ov_fresh;
  cur_win_ = &w;
  width_px_ = w.px_w;
  height_px_ = w.px_h;
  scale_ = eff;
  inset_[0] = inset_[1] = inset_[2] = inset_[3] = 0;
  raw_ = w.raw;
  raw_.shift = (mods_ & DH_MOD_SHIFT) != 0;
  in_ = raw_;
  const skin::Window* def = win_def(skin_ && skin_->loaded() ? skin_->skin() : nullptr, w);
  const float W = w.px_w / eff, H = w.px_h / eff;
  dl_.begin(w.px_w, w.px_h, eff);
  if (!(def && def->transparent)) dl_.rect(0, 0, W, H, w.panel ? theme_.panel : theme_.bg);
  if (w.panel) {
    draw_panel_window(w);
  } else if (!w.view) {
    draw_legacy();
  } else {
    draw_skin_ui();
    if (panel_ != Panel::None && w.role == panel_role(panel_)) {  // the skin's window of this role hosts the panel
      skin::Rect content{0, 0, W, H};
      w.view->find_rect("content", content);
      ov_ = raw_;
      if (panel_fresh_) {
        ov_.pressed = false;
        ov_.clicks = 0;
        panel_fresh_ = false;
      }
      std::string title;
      Rect m = panel_size(content.w, content.h, title);
      m.x = content.x + (content.w - m.w) / 2;
      m.y = content.y + (content.h - m.h) / 2;
      draw_panel_body(m, title);
    }
  }
  w.raw = raw_;
  w.raw.pressed = w.raw.released = false;
  w.raw.clicks = 0;
  w.raw.scroll = 0;
  w.raw.magnify = 0;
  cur_win_ = nullptr;
  width_px_ = sv_w;
  height_px_ = sv_h;
  scale_ = sv_s;
  raw_ = sv_raw;
  in_ = sv_in;
}

// A panel in its own window: it fills the window, there is no scrim and no click-outside (the window system does that).
void App::draw_panel_window(Win& w) {
  ov_ = raw_;
  if (panel_fresh_) {
    ov_.pressed = false;
    ov_.clicks = 0;
    panel_fresh_ = false;
  }
  if (panel_ == Panel::None) return;
  const float eff = scale_;
  const float W = w.px_w / eff, H = w.px_h / eff;
  std::string title;
  panel_size(100000, 100000, title);
  const Rect m{0, 0, W, H};
  panel_rect_ = m;
  draw_panel_body(m, title);
}

const dh_frame* App::window_frame(std::uint32_t id) {
  Win* w = find_win(id);
  if (!w || w->px_w < 1 || w->px_h < 1) return nullptr;
  const bool hosted = !w->panel && panel_ != Panel::None && w->role == panel_role(panel_);
  if (!(w->panel || w->role == "main" || ((w->st.visible || hosted) && w->placed))) return nullptr;
  draw_window_pass(*w);
  const skin::Window* def = win_def(skin_ && skin_->loaded() ? skin_->skin() : nullptr, *w);
  w->cmds.clear();
  for (const gfx::DrawCmd& c : dl_.commands()) {
    if (c.index_count == 0) continue;
    dh_draw_cmd o{};
    o.texture_id = c.texture;
    std::copy(std::begin(c.clip), std::end(c.clip), std::begin(o.clip));
    o.idx_offset = c.index_offset;
    o.idx_count = c.index_count;
    w->cmds.push_back(o);
  }
  const auto* vtx = reinterpret_cast<const dh_vertex*>(dl_.vertices().data());
  w->vtx.assign(vtx, vtx + dl_.vertices().size());
  w->idx = dl_.indices();
  w->frame = dh_frame{};
  w->frame.vtx = w->vtx.data();
  w->frame.vtx_count = static_cast<std::uint32_t>(w->vtx.size());
  w->frame.idx = w->idx.data();
  w->frame.idx_count = static_cast<std::uint32_t>(w->idx.size());
  w->frame.cmds = w->cmds.data();
  w->frame.cmd_count = static_cast<std::uint32_t>(w->cmds.size());
  if (def && def->transparent) {
    w->frame.clear_rgba[0] = w->frame.clear_rgba[1] = w->frame.clear_rgba[2] = w->frame.clear_rgba[3] = 0;  // premultiplied: all zero is clear
  } else {
    const Color bg = w->panel ? theme_.panel : theme_.bg;
    w->frame.clear_rgba[0] = static_cast<float>(bg & 0xFF) / 255.0f;
    w->frame.clear_rgba[1] = static_cast<float>((bg >> 8) & 0xFF) / 255.0f;
    w->frame.clear_rgba[2] = static_cast<float>((bg >> 16) & 0xFF) / 255.0f;
    w->frame.clear_rgba[3] = 1;
  }
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    w->frame.wants_text_input = search_focus_ || rename_id_ != 0 || focus_ != nullptr;
  }
  return &w->frame;
}

const dh_frame* App::end_frame() {
  collect_uploads(end_uploads_, end_releases_);
  end_frame_ = dh_frame{};
  end_frame_.uploads = end_uploads_.empty() ? nullptr : end_uploads_.data();
  end_frame_.upload_count = static_cast<std::uint32_t>(end_uploads_.size());
  end_frame_.releases = end_releases_.empty() ? nullptr : end_releases_.data();
  end_frame_.release_count = static_cast<std::uint32_t>(end_releases_.size());
  return &end_frame_;
}

// ---- input -------------------------------------------------------------------------------------------------------------
void App::window_pointer(std::uint32_t id, dh_ptr_kind kind, float x, float y, int clicks) {
  Win* w = find_win(id);
  if (!w) return;
  const float eff = win_eff_scale(*w);
  w->raw.x = x / eff;
  w->raw.y = y / eff;
  if (kind == DH_PTR_DOWN) {
    w->raw.down = w->raw.pressed = true;
    w->raw.clicks = clicks;
  } else if (kind == DH_PTR_UP || kind == DH_PTR_CANCEL) {
    w->raw.down = false;
    w->raw.released = true;
  }
}

void App::window_scroll(std::uint32_t id, float dy) {
  if (Win* w = find_win(id)) w->raw.scroll += dy / win_eff_scale(*w);
}

void App::window_pinch(std::uint32_t id, float magnification) {
  if (Win* w = find_win(id)) w->raw.magnify += magnification;
}

void App::window_open_file(std::uint32_t id, const std::string& path, float x, float y) {
  Win* w = find_win(id);
  if (!w || !w->view) {
    open_file(path, -1, -1);
    return;
  }
  // The deck under the drop is found in that window's layout; open_file() reads the main layout, so lend it ours.
  const float eff = win_eff_scale(*w);
  Job j;
  j.kind = Job::AddFile;
  j.path = path;
  if (x >= 0 && y >= 0) {
    for (std::size_t i = w->view->current().widgets.size(); i-- > 0;) {
      const skin::ResolvedWidget& rw = w->view->current().widgets[i];
      if (rw.bind.empty() || !rw.rect.contains(x / eff, y / eff)) continue;
      const ui::Target t = ui::map_binding(rw.bind);
      if (t.deck >= 0 && t.deck < kMaxDecks) {
        j.deck = t.deck;
        break;
      }
    }
  }
  if (j.deck >= 0 && engine_->deck(j.deck).playing) j.deck = -1;
  post(j);
}

// ---- hit testing ------------------------------------------------------------------------------------------------------------
dh_hit App::hit_test(std::uint32_t id, float x_px, float y_px) {
  Win* w = find_win(id);
  if (!w) return DH_HIT_NONE;
  const float eff = win_eff_scale(*w);
  const float x = x_px / eff, y = y_px / eff;
  if (w->panel || !w->view) return x >= 0 && y >= 0 && x < w->px_w / eff && y < w->px_h / eff ? DH_HIT_CONTROL : DH_HIT_NONE;
  const skin::Window* def = win_def(skin_->skin(), *w);
  if (!def) return DH_HIT_NONE;
  w->view->layout(def->id, w->px_w / eff, w->px_h / eff, 0, 0, 0, 0);
  if (def->chrome == skin::Chrome::System && !def->transparent) return DH_HIT_CONTROL;  // the system frame does the dragging
  ensure_shape(*w);
  // The shape is in the window's declared size; the layout was solved at the actual size (identical for scale=fit).
  const float sx = w->px_w / eff > 0 ? def->width / (w->px_w / eff) : 1, sy = w->px_h / eff > 0 ? def->height / (w->px_h / eff) : 1;
  const bool over = w->view->pointer_over_interactive(x, y);
  skin::WindowShape shape = w->shape;
  if (shape.rectangular() && def->transparent) {  // no mask, no shape: what was painted is what is solid
    if (!over && !w->view->painted_at(x, y)) return DH_HIT_NONE;
  }
  switch (skin::classify_point(shape, *def, over, x * sx, y * sy)) {
    case skin::Hit::None: return DH_HIT_NONE;
    case skin::Hit::Drag: return DH_HIT_DRAG;
    case skin::Hit::Control: return DH_HIT_CONTROL;
    case skin::Hit::Resize: return DH_HIT_RESIZE;
    case skin::Hit::Solid: return DH_HIT_SOLID;
  }
  return DH_HIT_NONE;
}

// ---- dragging, snapping, events ------------------------------------------------------------------------------------------------
void App::window_drag(std::uint32_t id, dh_drag_kind kind, dh_drag_phase phase, float gx, float gy, std::uint32_t mods) {
  const skin::Skin* sk = skin_ ? skin_->skin() : nullptr;
  if (phase == DH_DRAG_BEGIN) {
    Win* w = find_win(id);
    if (!w) return;
    drag_ = DragSession{};
    drag_.win = w->id;
    drag_.kind = kind;
    drag_.gx = gx;
    drag_.gy = gy;
    drag_.start = {w->st.x, w->st.y, w->pt_w, w->pt_h};
    drag_.start_scale = w->st.scale;
    drag_.alone = (mods & DH_MOD_SHIFT) != 0;
    drag_.active = true;
    w->st.z = z_counter_++;
    if (kind == DH_DRAG_MOVE) {
      std::vector<int> parent(wins_.size(), -1);
      int root = -1;
      for (std::size_t i = 0; i < wins_.size(); ++i) {
        if (wins_[i].get() == w) root = static_cast<int>(i);
        for (std::size_t j = 0; j < wins_.size(); ++j)
          if (!wins_[i]->st.parent.empty() && wins_[j]->name == wins_[i]->st.parent) parent[i] = static_cast<int>(j);
      }
      if (!drag_.alone)
        for (const int c : skin::group_children(parent, root)) drag_.group.push_back({wins_[static_cast<std::size_t>(c)]->id, {wins_[static_cast<std::size_t>(c)]->st.x, wins_[static_cast<std::size_t>(c)]->st.y}});
      w->in_drag = true;
      for (const auto& g : drag_.group)
        if (Win* m = find_win(g.first)) m->in_drag = true;
    } else {
      const float cx = gx - drag_.start.x, cy = gy - drag_.start.y;
      drag_.corner = std::string(cy > drag_.start.h / 2 ? "b" : "t") + (cx > drag_.start.w / 2 ? "r" : "l");
      w->in_drag = true;
    }
    return;
  }
  if (!drag_.active) return;
  Win* w = find_win(drag_.win);
  if (!w) {
    drag_ = DragSession{};
    return;
  }
  const float dx = gx - drag_.gx, dy = gy - drag_.gy;
  const skin::Window* def = win_def(sk, *w);
  if (phase == DH_DRAG_UPDATE) {
    if (drag_.kind == DH_DRAG_MOVE) {
      float mx = dx, my = dy;
      drag_.attach.clear();
      if (!drag_.alone && def && def->snap) {
        std::vector<skin::SnapTarget> others;
        for (std::size_t i = 0; i < wins_.size(); ++i) {
          const Win& o = *wins_[i];
          const skin::Window* od = win_def(sk, o);
          bool grouped = o.id == w->id;
          for (const auto& g : drag_.group) grouped = grouped || g.first == o.id;
          if (grouped || !od || !od->snap || !o.st.have_pos || !(o.role == "main" || o.st.visible)) continue;
          skin::SnapTarget t;
          t.frame = {o.st.x, o.st.y, o.pt_w, o.pt_h};
          t.id = static_cast<int>(i);
          const float k = od->scale_fit ? o.st.scale : 1.0f;
          for (const skin::SnapPoint& p : od->snap_points) t.points.push_back({o.st.x + p.at.x * k, o.st.y + p.at.y * k});
          others.push_back(std::move(t));
        }
        std::vector<skin::Point> mine;
        const float k = def->scale_fit ? w->st.scale : 1.0f;
        for (const skin::SnapPoint& p : def->snap_points) mine.push_back({drag_.start.x + dx + p.at.x * k, drag_.start.y + dy + p.at.y * k});
        const skin::SnapResult r = skin::snap_frame({drag_.start.x + dx, drag_.start.y + dy, drag_.start.w, drag_.start.h}, mine, others);
        mx += r.dx;
        my += r.dy;
        if (r.attach >= 0 && static_cast<std::size_t>(r.attach) < wins_.size()) drag_.attach = wins_[static_cast<std::size_t>(r.attach)]->name;
      }
      w->st.x = drag_.start.x + mx;
      w->st.y = drag_.start.y + my;
      w->st.have_pos = true;
      ++w->move_seq;
      for (const auto& g : drag_.group)
        if (Win* m = find_win(g.first)) {
          m->st.x = g.second.x + mx;
          m->st.y = g.second.y + my;
          ++m->move_seq;
        }
    } else if (def) {
      const float sx = drag_.corner[1] == 'r' ? 1.0f : -1.0f, sy = drag_.corner[0] == 'b' ? 1.0f : -1.0f;
      float nw, nh;
      if (def->scale_fit) {
        const float s = skin::scale_from_corner_drag(*def, drag_.start_scale, drag_.corner.c_str(), dx, dy);
        w->st.scale = s;
        const skin::Size sz = skin::window_size(*def, s);
        nw = sz.w;
        nh = sz.h;
      } else {
        const skin::Size sz = skin::clamp_free_size(*def, drag_.start.w + sx * dx, drag_.start.h + sy * dy);
        nw = sz.w;
        nh = sz.h;
      }
      w->pt_w = nw;
      w->pt_h = nh;
      w->st.x = drag_.corner[1] == 'r' ? drag_.start.x : drag_.start.x + drag_.start.w - nw;  // the opposite corner stays put
      w->st.y = drag_.corner[0] == 'b' ? drag_.start.y : drag_.start.y + drag_.start.h - nh;
      ++w->move_seq;
    }
    return;
  }
  // END
  if (drag_.kind == DH_DRAG_MOVE) {
    w->st.parent = drag_.alone ? std::string() : drag_.attach;  // snapped to something: docked to it; otherwise free
    w->in_drag = false;
    save_win_state(*w);
    for (const auto& g : drag_.group)
      if (Win* m = find_win(g.first)) {
        m->in_drag = false;
        save_win_state(*m);
      }
  } else {
    w->in_drag = false;
    save_win_state(*w);
  }
  drag_ = DragSession{};
}

void App::window_event(std::uint32_t id, dh_window_event kind, float a, float b) {
  Win* w = find_win(id);
  if (!w) return;
  switch (kind) {
    case DH_WE_MOVED:
      if (!w->in_drag && std::isfinite(a) && std::isfinite(b)) {
        const bool moved = std::fabs(w->st.x - a) > 0.5f || std::fabs(w->st.y - b) > 0.5f;
        w->st.x = a;
        w->st.y = b;
        w->st.have_pos = true;
        if (moved && w->reported && !w->panel) mark_dirty(*w);
      }
      w->reported = true;
      break;
    case DH_WE_RESIZED:
      if (!w->in_drag && std::isfinite(a) && std::isfinite(b) && a >= 16 && b >= 16) {
        const skin::Window* def = win_def(skin_ ? skin_->skin() : nullptr, *w);
        if (std::fabs(w->pt_w - a) > 0.5f || std::fabs(w->pt_h - b) > 0.5f) {
          w->pt_w = a;
          w->pt_h = b;
          if (def && def->scale_fit && def->width > 0) w->st.scale = skin::clamp_user_scale(*def, a / def->width);
          if (!w->panel) mark_dirty(*w);
        }
      }
      break;
    case DH_WE_FOCUS:
      if (a > 0.5f) {
        w->focused = true;
        w->st.z = z_counter_++;
      } else {
        const bool was = w->focused;
        w->focused = false;
        if (was && w->panel && w->popup) close_panel();  // a menu goes away when you click elsewhere
      }
      break;
    case DH_WE_CLOSE:
      if (w->panel) {
        close_panel();
      } else if (w->role != "main") {
        if (panel_ != Panel::None && w->role == panel_role(panel_)) close_panel();
        if (w->st.visible) set_module_visible(w->role, 0);
      }
      break;
    case DH_WE_MINIMIZED: break;
  }
}

void App::window_set_visible(std::uint32_t id, bool visible) {
  Win* w = find_win(id);
  if (w && !w->panel) set_module_visible(w->role, visible ? 1 : 0);
}

// ---- single-window fallback: every visible module docked into one canvas over a solid backdrop ---------------------------------
void App::compose_single() {
  const skin::Skin* sk = skin_->skin();
  Win* main = nullptr;
  std::vector<Win*> docked;
  for (auto& w : wins_) {
    if (w->panel || !w->view) continue;
    if (w->role == "main") main = w.get();
    else if (w->st.visible) docked.push_back(w.get());
  }
  if (!main) return;
  struct Cell {
    Win* w;
    Rect r;
  };
  std::vector<Cell> cells;
  const skin::Window* md = win_def(sk, *main);
  if (!md) return;
  cells.push_back({main, {0, 0, md->width, md->height}});
  for (Win* m : docked) {
    const skin::Window* d = win_def(sk, *m);
    if (!d) continue;
    skin::Placement p = d->place;
    if (!p.set) {
      p.ref = "main";
      p.side = skin::Placement::Side::Right;
      p.dx = 16;
    }
    Rect ref = cells[0].r;
    for (const Cell& c : cells)
      if (c.w->name == p.ref || c.w->role == p.ref) ref = c.r;
    const float k = d->scale_fit ? d->scale_default : 1.0f;
    const skin::Size sz{d->width * k, d->height * k};
    const skin::Point at = skin::place_relative(p, {ref.x, ref.y, ref.w, ref.h}, sz.w, sz.h);
    cells.push_back({m, {at.x, at.y, sz.w, sz.h}});
  }
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (const Cell& c : cells) {
    x0 = std::min(x0, c.r.x);
    y0 = std::min(y0, c.r.y);
    x1 = std::max(x1, c.r.right());
    y1 = std::max(y1, c.r.bottom());
  }
  const float bw = x1 - x0, bh = y1 - y0;
  const float disp_w = width_px_ / scale_, disp_h = height_px_ / scale_;
  const float fit = std::clamp(std::min(disp_w / std::max(1.0f, bw), disp_h / std::max(1.0f, bh)), 0.3f, 1.0f);
  const float W = disp_w / fit_, H = disp_h / fit_;  // canvas size in the units drawn this frame
  const float ox = std::max(0.0f, (W - bw) / 2) - x0, oy = std::max(0.0f, (H - bh) / 2) - y0;
  for (const Cell& c : cells) {
    Win& w = *c.w;
    cur_win_ = &w;
    w.view->set_origin(c.r.x + ox, c.r.y + oy);
    w.view->layout(w.name, c.r.w, c.r.h, 0, 0, 0, 0);
    dl_.push_clip(c.r.x + ox, c.r.y + oy, c.r.w, c.r.h);
    w.view->frame(*this, in_, now_);
    dl_.pop_clip();
    cur_win_ = nullptr;
  }
  fit_ = fit;  // the pointer of the next frame is mapped with it
}

// ---- debug queries (admin API) -----------------------------------------------------------------------------------------------------
std::string App::run_on_ui(std::function<std::string()> fn) {
  auto task = std::make_shared<std::packaged_task<std::string()>>(std::move(fn));
  std::future<std::string> fut = task->get_future();
  {
    std::lock_guard<std::mutex> lock(dbg_mutex_);
    dbg_queue_.push_back([task] { (*task)(); });
  }
  if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return {};
  return fut.get();
}

void App::serve_debug_requests() {
  std::vector<std::function<void()>> todo;
  {
    std::lock_guard<std::mutex> lock(dbg_mutex_);
    todo.swap(dbg_queue_);
  }
  for (auto& f : todo) f();
}

std::string App::api_debug_windows() {
  return run_on_ui([this] {
    std::string out = "[";
    bool first = true;
    for (const auto& wp : wins_) {
      const Win& w = *wp;
      if (!first) out += ",";
      first = false;
      char buf[400];
      std::snprintf(buf, sizeof buf, "{\"id\":%u,\"name\":%s,\"role\":%s,\"visible\":%s,\"panel\":%s,\"x\":%.1f,\"y\":%.1f,\"width\":%.1f,\"height\":%.1f,\"scale\":%.3f,\"docked_to\":%s,\"position_known\":%s}",
                    w.id, json_str(w.name).c_str(), json_str(w.role).c_str(), (w.role == "main" || w.st.visible || w.panel) ? "true" : "false",
                    w.panel ? "true" : "false", w.st.x, w.st.y, w.pt_w, w.pt_h, w.st.scale, json_str(w.st.parent).c_str(), w.st.have_pos ? "true" : "false");
      out += buf;
    }
    return out + "]";
  });
}

std::string App::api_debug_hit(const std::string& window, float x, float y) {
  return run_on_ui([this, window, x, y] {
    Win* w = nullptr;
    for (auto& wp : wins_)
      if (wp->name == window || wp->role == window) w = wp.get();
    if (!w) return std::string("{\"error\":\"no such window\"}");
    // Window points -> window pixels, using the pixel size the shell reported (or the core's own size before the first frame).
    const float eff = win_eff_scale(*w);
    if (w->px_w < 1) {
      w->px_w = w->pt_w * w->backing;
      w->px_h = w->pt_h * w->backing;
    }
    const float scale_to_px = w->px_w / std::max(1.0f, w->pt_w);
    const dh_hit h = hit_test(w->id, x * scale_to_px, y * scale_to_px);
    static const char* names[] = {"none", "drag", "control", "resize", "solid"};
    std::string control;
    if (w->view) {
      const float lx = x * scale_to_px / eff, ly = y * scale_to_px / eff;
      const auto& ws = w->view->current().widgets;
      for (std::size_t i = ws.size(); i-- > 0;)
        if (ws[i].interactive && ws[i].rect.contains(lx, ly) && w->view->pointer_over_interactive(lx, ly)) {
          const auto act = ws[i].props.find("action");
          control = !ws[i].bind.empty() ? ws[i].bind : act != ws[i].props.end() ? act->second : !ws[i].id.empty() ? ws[i].id : ws[i].type;
          break;
        }
    }
    char buf[300];
    std::snprintf(buf, sizeof buf, "{\"window\":%s,\"x\":%.1f,\"y\":%.1f,\"hit\":\"%s\",\"opaque\":%s,\"control\":%s}", json_str(w->name).c_str(), x, y,
                  names[std::clamp(static_cast<int>(h), 0, 4)], h == DH_HIT_NONE ? "false" : "true", json_str(control).c_str());
    return std::string(buf);
  });
}

}  // namespace deckhand
