#include "deckhand/skin/windows.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace deckhand::skin {

namespace {
constexpr float kPi = 3.14159265358979f;

bool in_polygon(const std::vector<Point>& poly, float x, float y) {
  if (poly.size() < 3) return false;
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    if ((poly[i].y > y) != (poly[j].y > y) && x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) in = !in;
  }
  return in;
}

bool in_part(const ShapePart& p, float x, float y) {
  switch (p.type) {
    case ShapePart::Type::Rect: {
      if (!p.box.contains(x, y)) return false;
      const float r = std::min({p.radius, p.box.w / 2, p.box.h / 2});
      if (r <= 0) return true;
      const float cx = std::clamp(x, p.box.x + r, p.box.right() - r), cy = std::clamp(y, p.box.y + r, p.box.bottom() - r);
      return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }
    case ShapePart::Type::Ellipse: {
      const float rx = p.box.w / 2, ry = p.box.h / 2;
      if (rx <= 0 || ry <= 0) return false;
      const float nx = (x - (p.box.x + rx)) / rx, ny = (y - (p.box.y + ry)) / ry;
      return nx * nx + ny * ny <= 1.0f;
    }
    case ShapePart::Type::Polygon:
    case ShapePart::Type::Path: return in_polygon(p.points, x, y);
  }
  return false;
}
}  // namespace

// ---- masks and shapes ---------------------------------------------------------------------------------------
int Mask::sample(float u, float v) const {
  if (empty() || !(u >= 0) || !(v >= 0) || u >= 1 || v >= 1) return 0;
  const int x = std::min(w - 1, static_cast<int>(u * static_cast<float>(w))), y = std::min(h - 1, static_cast<int>(v * static_cast<float>(h)));
  return alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
}

bool mask_from_rgba(const std::uint8_t* rgba, int w, int h, Mask& out, std::string& error) {
  out = Mask{};
  if (!rgba || w <= 0 || h <= 0) {
    error = "empty mask image";
    return false;
  }
  if (w > limits::kMaxMaskDim || h > limits::kMaxMaskDim || static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > limits::kMaxMaskPixels) {
    error = "mask image larger than " + std::to_string(limits::kMaxMaskDim) + " px per side";
    return false;
  }
  out.w = w;
  out.h = h;
  out.alpha.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (std::size_t i = 0; i < out.alpha.size(); ++i) out.alpha[i] = rgba[i * 4 + 3];
  return true;
}

std::vector<Point> shape_outline(const Shape& sh, float W, float H, float scale) {
  std::vector<Point> p;
  switch (sh.kind) {
    case ShapeKind::Parts: break;
    case ShapeKind::Polygon:
      for (const Point& q : sh.points) p.push_back({q.x * W, q.y * H});
      break;
    case ShapeKind::Ellipse:
      for (int i = 0; i < 64; ++i) {
        const float a = 2 * kPi * static_cast<float>(i) / 64;
        p.push_back({W / 2 + std::cos(a) * W / 2, H / 2 + std::sin(a) * H / 2});
      }
      break;
    case ShapeKind::Chamfer: {
      const float k = std::min({sh.amount * scale, W / 2, H / 2});
      p = {{k, 0}, {W - k, 0}, {W, k}, {W, H - k}, {W - k, H}, {k, H}, {0, H - k}, {0, k}};
      break;
    }
    case ShapeKind::RoundRect: {
      const float r = std::min({sh.amount * scale, W / 2, H / 2});
      const Point centres[4] = {{W - r, r}, {W - r, H - r}, {r, H - r}, {r, r}};
      for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= 8; ++i) {
          const float a = (static_cast<float>(corner) * 90 + 270 + 90.0f * static_cast<float>(i) / 8) * kPi / 180;
          p.push_back({centres[corner].x + std::cos(a) * r, centres[corner].y + std::sin(a) * r});
        }
      break;
    }
  }
  return p;
}

bool parts_contain(const std::vector<ShapePart>& parts, float x, float y) {
  bool inside = false;
  for (const ShapePart& p : parts) {
    if (p.sub) {
      if (inside && in_part(p, x, y)) inside = false;
    } else if (!inside && in_part(p, x, y)) {
      inside = true;
    }
  }
  return inside;
}

bool WindowShape::opaque(float x, float y) const {
  if (!(x >= 0) || !(y >= 0) || x >= base_w || y >= base_h) return false;
  if (!mask.empty()) return mask.sample(x / base_w, y / base_h) >= threshold;
  if (!parts.empty()) return parts_contain(parts, x, y);
  if (outline.size() >= 3) return in_polygon(outline, x, y);
  return true;
}

WindowShape make_window_shape(const Skin& skin, const Window& w, Mask image_mask) {
  WindowShape s;
  s.base_w = w.width;
  s.base_h = w.height;
  s.threshold = std::clamp(w.mask_threshold, 1, 255);
  if (!image_mask.empty()) {
    s.mask = std::move(image_mask);
    return s;
  }
  if (!w.shape.empty()) {
    if (const Shape* sh = skin.find_shape(w.shape)) {
      if (sh->kind == ShapeKind::Parts) s.parts = sh->parts;
      else s.outline = shape_outline(*sh, w.width, w.height, 1);
    }
  }
  return s;
}

Hit classify_point(const WindowShape& shape, const Window& w, bool over_control, float x, float y) {
  if (!shape.opaque(x, y)) {
    // Controls and resize grips may overhang the art (a knob on the rim): they still count.
    if (over_control) return Hit::Control;
    if (!w.resize_handle.empty() || !w.resize.empty()) {
      if (parts_contain(w.resize, x, y)) return Hit::Resize;
    }
    return Hit::None;
  }
  if (over_control) return Hit::Control;
  if (parts_contain(w.resize, x, y)) return Hit::Resize;
  if (!w.resize_handle.empty() && w.resize_size > 0) {
    const float s = w.resize_size;
    const bool right = w.resize_handle[1] == 'r', bottom = w.resize_handle[0] == 'b';
    const float x0 = right ? w.width - s : 0, y0 = bottom ? w.height - s : 0;
    if (x >= x0 && x < x0 + s && y >= y0 && y < y0 + s) return Hit::Resize;
  }
  if (w.drag_mode == DragMode::Regions) return parts_contain(w.drag, x, y) ? Hit::Drag : Hit::Solid;
  return Hit::Drag;
}

// ---- size and scale ----------------------------------------------------------------------------------------------
float clamp_user_scale(const Window& w, float s) {
  if (!w.scale_fit) return 1.0f;
  if (!std::isfinite(s)) s = w.scale_default;
  return std::clamp(s, w.scale_min, w.scale_max);
}

Size window_size(const Window& w, float user_scale) {
  const float s = clamp_user_scale(w, user_scale);
  return {w.width * s, w.height * s};
}

Size clamp_free_size(const Window& w, float width, float height) {
  if (!std::isfinite(width)) width = w.width;
  if (!std::isfinite(height)) height = w.height;
  if (w.aspect_lock && w.height > 0) {
    const float a = w.width / w.height;
    if (width / std::max(1.0f, height) > a) width = height * a;
    else height = width / a;
  }
  width = std::max(width, std::max(w.min_w, 16.0f));
  height = std::max(height, std::max(w.min_h, 16.0f));
  if (w.max_w > 0) width = std::min(width, w.max_w);
  if (w.max_h > 0) height = std::min(height, w.max_h);
  return {width, height};
}

float scale_from_corner_drag(const Window& w, float start_scale, const char* corner, float dx, float dy) {
  const float sw = w.width * start_scale, sh = w.height * start_scale;
  const float diag = std::sqrt(sw * sw + sh * sh);
  if (diag <= 0 || !corner || !corner[0] || !corner[1]) return clamp_user_scale(w, start_scale);
  const float sx = corner[1] == 'r' ? 1.0f : -1.0f, sy = corner[0] == 'b' ? 1.0f : -1.0f;  // direction the corner moves outward
  const float along = (dx * sx * sw + dy * sy * sh) / diag;
  return clamp_user_scale(w, start_scale * (1.0f + along / diag));
}

// ---- snapping --------------------------------------------------------------------------------------------------------
SnapResult snap_frame(const Frame& m, const std::vector<Point>& mpoints, const std::vector<SnapTarget>& others, float threshold) {
  SnapResult r;
  float best_x = threshold + 1, best_y = threshold + 1;
  int attach_x = -1, attach_y = -1;
  const auto overlap = [&](float a0, float a1, float b0, float b1) { return a0 < b1 + threshold && b0 < a1 + threshold; };
  for (const SnapTarget& o : others) {
    const Frame& f = o.frame;
    const bool v_overlap = overlap(m.y, m.bottom(), f.y, f.bottom());
    const bool h_overlap = overlap(m.x, m.right(), f.x, f.right());
    if (v_overlap) {
      const struct { float d; bool dock; } xs[] = {{f.right() - m.x, true}, {f.x - m.right(), true}, {f.x - m.x, false}, {f.right() - m.right(), false}};
      for (const auto& c : xs)
        if (std::fabs(c.d) <= threshold && std::fabs(c.d) < std::fabs(best_x)) {
          best_x = c.d;
          attach_x = c.dock ? o.id : -1;
          r.snapped_x = true;
        }
    }
    if (h_overlap) {
      const struct { float d; bool dock; } ys[] = {{f.bottom() - m.y, true}, {f.y - m.bottom(), true}, {f.y - m.y, false}, {f.bottom() - m.bottom(), false}};
      for (const auto& c : ys)
        if (std::fabs(c.d) <= threshold && std::fabs(c.d) < std::fabs(best_y)) {
          best_y = c.d;
          attach_y = c.dock ? o.id : -1;
          r.snapped_y = true;
        }
    }
  }
  // Anchor points beat edges: both axes at once, exact.
  float best_pt = threshold * threshold * 2 + 1;
  for (const SnapTarget& o : others)
    for (const Point& op : o.points)
      for (const Point& mp : mpoints) {
        const float ddx = op.x - mp.x, ddy = op.y - mp.y, d2 = ddx * ddx + ddy * ddy;
        if (std::fabs(ddx) <= threshold && std::fabs(ddy) <= threshold && d2 < best_pt) {
          best_pt = d2;
          best_x = ddx;
          best_y = ddy;
          attach_x = attach_y = o.id;
          r.snapped_x = r.snapped_y = true;
        }
      }
  if (r.snapped_x) r.dx = best_x;
  if (r.snapped_y) r.dy = best_y;
  r.attach = attach_x >= 0 ? attach_x : attach_y;
  return r;
}

std::vector<int> group_children(const std::vector<int>& parent, int root) {
  std::vector<int> out;
  std::vector<char> seen(parent.size(), 0);
  if (root >= 0 && static_cast<std::size_t>(root) < seen.size()) seen[static_cast<std::size_t>(root)] = 1;
  for (std::size_t pass = 0; pass < parent.size(); ++pass) {  // bounded: cycles cannot loop forever
    bool grew = false;
    for (std::size_t i = 0; i < parent.size(); ++i) {
      if (seen[i] || parent[i] < 0 || static_cast<std::size_t>(parent[i]) >= parent.size() || !seen[static_cast<std::size_t>(parent[i])]) continue;
      seen[i] = 1;
      out.push_back(static_cast<int>(i));
      grew = true;
    }
    if (!grew) break;
  }
  return out;
}

// ---- arrangement ---------------------------------------------------------------------------------------------------
Point place_relative(const Placement& p, const Frame& ref, float w, float h) {
  switch (p.side) {
    case Placement::Side::Below: return {ref.x + p.dx, ref.bottom() + p.dy};
    case Placement::Side::Above: return {ref.x + p.dx, ref.y - h + p.dy};
    case Placement::Side::Right: return {ref.right() + p.dx, ref.y + p.dy};
    case Placement::Side::Left: return {ref.x - w + p.dx, ref.y + p.dy};
    case Placement::Side::Over: break;
  }
  return {ref.x + p.dx, ref.y + p.dy};
}

std::string encode_win_state(const WinState& s) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "x=%.1f y=%.1f s=%.3f w=%.1f h=%.1f v=%d z=%d p=%s", s.have_pos ? s.x : 0.0f, s.have_pos ? s.y : 0.0f, s.scale, s.w, s.h,
                s.visible ? 1 : 0, s.z, s.parent.empty() ? "-" : s.parent.c_str());
  std::string out = buf;
  if (!s.have_pos) out += " nopos=1";
  return out;
}

bool decode_win_state(std::string_view text, WinState& out) {
  if (text.empty() || text.size() > 400) return false;
  WinState s;
  std::string t(text);
  std::size_t at = 0;
  int fields = 0;
  bool seen_x = false, seen_y = false;
  s.have_pos = true;
  while (at < t.size()) {
    while (at < t.size() && t[at] == ' ') ++at;
    const std::size_t eq = t.find('=', at), sp = t.find(' ', at);
    if (eq == std::string::npos || (sp != std::string::npos && sp < eq)) return false;
    const std::string key = t.substr(at, eq - at);
    const std::string val = t.substr(eq + 1, (sp == std::string::npos ? t.size() : sp) - eq - 1);
    at = sp == std::string::npos ? t.size() : sp;
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    const bool numeric = !val.empty() && end && *end == '\0' && std::isfinite(d);
    if (key == "x" || key == "y") {
      if (!numeric || std::fabs(d) > 1e6) return false;
      (key == "x" ? s.x : s.y) = static_cast<float>(d);
      (key == "x" ? seen_x : seen_y) = true;
    } else if (key == "s") {
      if (!numeric || d < 0.1 || d > 10) return false;
      s.scale = static_cast<float>(d);
    } else if (key == "w" || key == "h") {
      if (!numeric || d < 0 || d > 20000) return false;
      (key == "w" ? s.w : s.h) = static_cast<float>(d);
    } else if (key == "v") {
      s.visible = val == "1";
    } else if (key == "z") {
      if (!numeric || std::fabs(d) > 1e6) return false;
      s.z = static_cast<int>(d);
    } else if (key == "p") {
      if (val != "-") {
        if (val.size() > 32) return false;
        for (const char c : val)
          if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return false;
        s.parent = val;
      }
    } else if (key == "nopos") {
      s.have_pos = false;
    } else {
      return false;
    }
    if (++fields > 12) return false;
  }
  if (fields == 0 || !seen_x || !seen_y) return false;  // a position is always saved as a pair
  out = s;
  return true;
}

std::string win_setting_key(const std::string& skin_name, const std::string& window_id) {
  std::string k = "ui.win.";
  for (const char c : skin_name) k += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
  k += '.';
  for (const char c : window_id) k += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
  return k;
}

// ---- curves ------------------------------------------------------------------------------------------------------------
Point PathCurve::at(float t) const {
  if (pts.empty()) return {};
  if (pts.size() == 1 || cum.size() != pts.size() || length() <= 0) return pts.front();
  const float d = std::clamp(t, 0.0f, 1.0f) * length();
  std::size_t i = static_cast<std::size_t>(std::upper_bound(cum.begin(), cum.end(), d) - cum.begin());
  if (i == 0) return pts.front();
  if (i >= pts.size()) return pts.back();
  const float seg = cum[i] - cum[i - 1];
  const float k = seg > 0 ? (d - cum[i - 1]) / seg : 0;
  return {pts[i - 1].x + (pts[i].x - pts[i - 1].x) * k, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * k};
}

float PathCurve::project(float x, float y) const {
  if (pts.size() < 2 || length() <= 0) return 0;
  float best = 1e30f, best_d = 0;
  for (std::size_t i = 1; i < pts.size(); ++i) {
    const float ax = pts[i - 1].x, ay = pts[i - 1].y, bx = pts[i].x, by = pts[i].y;
    const float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    const float k = l2 > 0 ? std::clamp(((x - ax) * vx + (y - ay) * vy) / l2, 0.0f, 1.0f) : 0.0f;
    const float px = ax + vx * k, py = ay + vy * k;
    const float d2 = (x - px) * (x - px) + (y - py) * (y - py);
    if (d2 < best) {
      best = d2;
      best_d = cum[i - 1] + (cum[i] - cum[i - 1]) * k;
    }
  }
  return std::clamp(best_d / length(), 0.0f, 1.0f);
}

bool parse_path(std::string_view d, PathCurve& out, std::string& error) {
  out = PathCurve{};
  if (d.empty() || d.size() > limits::kMaxStringLen) {
    error = "empty or oversized path";
    return false;
  }
  const std::string s(d);
  std::size_t i = 0;
  const auto skip = [&] {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
  };
  const auto number = [&](float& v) {
    skip();
    if (i >= s.size()) return false;
    char* end = nullptr;
    const double x = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i || !std::isfinite(x) || std::fabs(x) > 1e5) return false;
    i = static_cast<std::size_t>(end - s.c_str());
    v = static_cast<float>(x);
    return true;
  };
  Point cur{}, start{};
  bool have = false;
  const auto push = [&](Point p) {
    if (out.pts.size() >= limits::kMaxPathPoints) return false;
    out.pts.push_back(p);
    cur = p;
    return true;
  };
  char cmd = 0;
  while (true) {
    skip();
    if (i >= s.size()) break;
    if (std::isalpha(static_cast<unsigned char>(s[i]))) cmd = s[i++];
    else if (cmd == 0) {
      error = "path must start with a command";
      return false;
    }
    float a, b, c2, d2, e, f;
    bool ok = true;
    switch (cmd) {
      case 'M':
        ok = number(a) && number(b);
        if (ok) {
          start = {a, b};
          have = true;
          ok = push({a, b});
          cmd = 'L';  // further pairs are line-tos
        }
        break;
      case 'L':
        ok = have && number(a) && number(b) && push({a, b});
        break;
      case 'H':
        ok = have && number(a) && push({a, cur.y});
        break;
      case 'V':
        ok = have && number(a) && push({cur.x, a});
        break;
      case 'Q':
        ok = have && number(a) && number(b) && number(c2) && number(d2);
        if (ok) {
          const Point p0 = cur;
          for (int k = 1; k <= 16 && ok; ++k) {
            const float t = static_cast<float>(k) / 16, u = 1 - t;
            ok = push({u * u * p0.x + 2 * u * t * a + t * t * c2, u * u * p0.y + 2 * u * t * b + t * t * d2});
          }
        }
        break;
      case 'C':
        ok = have && number(a) && number(b) && number(c2) && number(d2) && number(e) && number(f);
        if (ok) {
          const Point p0 = cur;
          for (int k = 1; k <= 20 && ok; ++k) {
            const float t = static_cast<float>(k) / 20, u = 1 - t;
            ok = push({u * u * u * p0.x + 3 * u * u * t * a + 3 * u * t * t * c2 + t * t * t * e,
                       u * u * u * p0.y + 3 * u * u * t * b + 3 * u * t * t * d2 + t * t * t * f});
          }
        }
        break;
      case 'Z':
        ok = have && push(start);
        cmd = 0;
        break;
      default:
        error = std::string("unsupported path command '") + cmd + "' (use upper-case M L H V Q C Z)";
        return false;
    }
    if (!ok) {
      error = "bad or too long path data";
      return false;
    }
  }
  if (out.pts.size() < 2) {
    error = "a path needs at least two points";
    return false;
  }
  out.cum.assign(out.pts.size(), 0.0f);
  for (std::size_t k = 1; k < out.pts.size(); ++k)
    out.cum[k] = out.cum[k - 1] + std::hypot(out.pts[k].x - out.pts[k - 1].x, out.pts[k].y - out.pts[k - 1].y);
  return true;
}

}  // namespace deckhand::skin
