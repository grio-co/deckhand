// skin: layout solver. Turns the node tree into a flat list of resolved rectangles in physical pixels.
#include <algorithm>
#include <cmath>
#include <limits>

#include "deckhand/skin/skin.hpp"

namespace deckhand::skin {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

struct Ctx {
  const Skin& sk;
  float scale;
  Layout& out;
  const std::vector<char>* hidden = nullptr;  // by node index: take no space in a row, column or grid
  bool is_hidden(int node) const {
    return hidden && static_cast<size_t>(node) < hidden->size() && (*hidden)[static_cast<size_t>(node)];
  }
};

Style make_style(const Skin& sk, const StyleSpec& base, const StyleSpec* over, float scale) {
  Style s;
  auto apply = [&](const StyleSpec& t) {
    if (t.fg) s.fg = *t.fg;
    if (t.bg) s.bg = *t.bg;
    if (t.border) s.border = *t.border;
    if (t.glow) s.glow = *t.glow;
    if (t.border_w) s.border_w = *t.border_w;
    if (t.radius) s.radius = *t.radius;
    if (t.opacity) s.opacity = *t.opacity;
    if (t.scale) s.scale = *t.scale;
    if (t.font_size) s.font_size = *t.font_size;
    if (t.glow_r) s.glow_r = *t.glow_r;
    if (t.font) {
      s.font = *t.font;
      auto it = sk.fonts.find(s.font);
      if (it != sk.fonts.end() && !t.font_size) s.font_size = it->second.size;
    }
    if (t.sprite) s.sprite = *t.sprite;
    if (t.align) s.align = *t.align;
  };
  apply(base);
  if (over) apply(*over);
  s.border_w *= scale;
  s.radius *= scale;
  s.font_size *= scale;
  s.glow_r *= scale;
  return s;
}

Rect deflate(Rect r, float l, float t, float rr, float b) {
  r.x += l;
  r.y += t;
  r.w = std::max(0.0f, r.w - l - rr);
  r.h = std::max(0.0f, r.h - t - b);
  return r;
}

// Apply margin and aspect to an allocated slot.
Rect finalize(const Node& n, Rect slot, float scale) {
  Rect r = deflate(slot, n.margin[0] * scale, n.margin[1] * scale, n.margin[2] * scale, n.margin[3] * scale);
  if (n.aspect > 0 && r.w > 0 && r.h > 0) {
    float w = r.w, h = r.h;
    if (w / h > n.aspect) w = h * n.aspect;
    else h = w / n.aspect;
    r.x += (r.w - w) / 2;
    r.y += (r.h - h) / 2;
    r.w = w;
    r.h = h;
  }
  return r;
}

int emit(Ctx& c, int node, const Rect& r, int parent, bool parent_flow) {
  const Node& n = c.sk.nodes[node];
  ResolvedWidget w;
  w.node = node;
  w.parent = parent;
  w.parent_is_flow = parent_flow;
  w.kind = n.kind;
  w.type = n.type;
  w.id = n.id;
  w.bind = n.bind;
  w.bind2 = n.bind2;
  w.text = n.text;
  w.format = n.format;
  w.show = n.show;
  w.tooltip = n.tooltip;
  w.shape = n.shape;
  w.rect = r;
  w.rotation = n.rotate;
  w.transition_ms = n.transition_ms * 1.0f;
  w.props = n.props;
  w.animations = n.anims;
  w.container = is_container(n.kind);
  w.interactive = is_interactive(n.kind);
  for (int s = 0; s < kStateCount; ++s)
    w.style[s] = make_style(c.sk, n.base, s == 0 ? nullptr : &n.state[s], c.scale);
  const Style& st = w.style[0];
  w.drawable = !w.container || (st.bg & 0xFF) || (st.border & 0xFF && st.border_w > 0) || !st.sprite.empty() || (st.glow & 0xFF);
  c.out.widgets.push_back(std::move(w));
  return static_cast<int>(c.out.widgets.size()) - 1;
}

void place(Ctx& c, int node, const Rect& slot, int parent, bool parent_flow);

float px_or(const Length& l, float parent_size, float scale, float fallback) {
  if (l.unit == Length::Unit::Px) return l.v * scale;
  if (l.unit == Length::Unit::Percent) return l.v / 100.0f * parent_size;
  return fallback;
}

void flow(Ctx& c, const Node& n, int me, const Rect& content, bool horiz) {
  // Hidden children (show= false) keep no space and no gap; they are still emitted, collapsed to nothing.
  std::vector<int> kids;
  for (int ch : n.children)
    if (!c.is_hidden(ch)) kids.push_back(ch);
  const size_t count = kids.size();
  std::vector<Rect> slots(count);
  // Children are emitted in their written order whatever is hidden, so widget indices never move.
  const auto emit_all = [&] {
    size_t vi = 0;
    for (int ch : n.children) {
      if (c.is_hidden(ch)) place(c, ch, Rect{content.x, content.y, 0, 0}, me, true);
      else place(c, ch, slots[vi++], me, true);
    }
  };
  if (count == 0) {
    emit_all();
    return;
  }
  const float s = c.scale;
  const float gap = n.gap * s;
  const float cmain = horiz ? content.w : content.h;
  const float ccross = horiz ? content.h : content.w;
  struct Item { float fixed = -1, flex = 0, min_o = 0, max_o = kInf, size = 0; bool frozen = false; };
  std::vector<Item> items(count);
  float fixed_total = 0;
  for (size_t i = 0; i < count; ++i) {
    const Node& k = c.sk.nodes[kids[i]];
    const Length& L = horiz ? k.w : k.h;
    const float mm = horiz ? (k.margin[0] + k.margin[2]) * s : (k.margin[1] + k.margin[3]) * s;
    Item& it = items[i];
    it.min_o = (horiz ? k.min_w : k.min_h) * s + mm;
    const float mx = horiz ? k.max_w : k.max_h;
    it.max_o = mx > 0 ? mx * s + mm : kInf;
    if (L.unit == Length::Unit::Px || L.unit == Length::Unit::Percent) {
      it.fixed = std::clamp(px_or(L, cmain, s, 0) + mm, it.min_o, it.max_o);
      fixed_total += it.fixed;
    } else {
      it.flex = L.unit == Length::Unit::Flex ? L.v : (k.flex > 0 ? k.flex : 1.0f);
      if (it.flex <= 0) it.flex = 1;
    }
  }
  const float avail = std::max(0.0f, cmain - gap * static_cast<float>(count - 1));
  float shrink = 1;
  if (fixed_total > avail && fixed_total > 0) shrink = avail / fixed_total;
  float remaining = std::max(0.0f, avail - fixed_total * shrink);
  for (auto& it : items)
    if (it.fixed >= 0) { it.size = it.fixed * shrink; it.frozen = true; }
  // distribute the remainder over flex items, freezing any that hit min/max
  for (size_t round = 0; round <= count; ++round) {
    float total_flex = 0;
    for (auto& it : items) if (!it.frozen) total_flex += it.flex;
    if (total_flex <= 0) break;
    bool clamped = false;
    for (auto& it : items) {
      if (it.frozen) continue;
      float share = remaining * it.flex / total_flex;
      float cl = std::clamp(share, it.min_o, it.max_o);
      if (cl != share) {
        it.size = cl; it.frozen = true; remaining -= cl; clamped = true;
        remaining = std::max(0.0f, remaining);
        break;  // restart distribution with the new remainder
      }
    }
    if (!clamped) {
      for (auto& it : items) if (!it.frozen) { it.size = remaining * it.flex / total_flex; it.frozen = true; }
      remaining = 0;
      break;
    }
  }
  float sum = 0;
  for (auto& it : items) sum += it.size;
  if (sum > avail && sum > 0) { for (auto& it : items) it.size *= avail / sum; sum = avail; }
  float left = std::max(0.0f, avail - sum);
  float start = 0, between = gap;
  switch (n.justify) {
    case Justify::Start: break;
    case Justify::Center: start = left / 2; break;
    case Justify::End: start = left; break;
    case Justify::Between: if (count > 1) between += left / static_cast<float>(count - 1); break;
  }
  float pos = start;
  for (size_t i = 0; i < count; ++i) {
    const Node& k = c.sk.nodes[kids[i]];
    const Length& CL = horiz ? k.h : k.w;
    const float cm = horiz ? (k.margin[1] + k.margin[3]) * s : (k.margin[0] + k.margin[2]) * s;
    float cross = ccross;
    if ((CL.unit == Length::Unit::Px || CL.unit == Length::Unit::Percent) ) cross = std::min(ccross, px_or(CL, ccross, s, ccross) + cm);
    else if (n.align != Align::Stretch) cross = ccross;
    float cpos = 0;
    if (n.align == Align::Center) cpos = (ccross - cross) / 2;
    else if (n.align == Align::End) cpos = ccross - cross;
    slots[i] = horiz ? Rect{content.x + pos, content.y + cpos, items[i].size, cross}
                     : Rect{content.x + cpos, content.y + pos, cross, items[i].size};
    pos += items[i].size + between;
  }
  emit_all();
}

Rect anchored(const Node& k, const Rect& content, float scale, Anchor def) {
  Anchor a = k.anchor.value_or(def);
  const float mw = (k.margin[0] + k.margin[2]) * scale, mh = (k.margin[1] + k.margin[3]) * scale;
  float w = px_or(k.w, content.w, scale, content.w - mw) + mw;
  float h = px_or(k.h, content.h, scale, content.h - mh) + mh;
  w = std::min(w, content.w);
  h = std::min(h, content.h);
  if (a == Anchor::Fill) return content;
  const float ox = px_or(k.x, content.w, scale, 0), oy = px_or(k.y, content.h, scale, 0);
  float x = content.x, y = content.y;
  const bool left = a == Anchor::TL || a == Anchor::L || a == Anchor::BL;
  const bool right = a == Anchor::TR || a == Anchor::R || a == Anchor::BR;
  const bool top = a == Anchor::TL || a == Anchor::T || a == Anchor::TR;
  const bool bottom = a == Anchor::BL || a == Anchor::B || a == Anchor::BR;
  x = left ? content.x + ox : right ? content.right() - w - ox : content.x + (content.w - w) / 2 + ox;
  y = top ? content.y + oy : bottom ? content.bottom() - h - oy : content.y + (content.h - h) / 2 + oy;
  x = std::clamp(x, content.x, std::max(content.x, content.right() - w));
  y = std::clamp(y, content.y, std::max(content.y, content.bottom() - h));
  return {x, y, w, h};
}

void grid(Ctx& c, const Node& n, int me, const Rect& content) {
  std::vector<int> kids;  // hidden children take no cell
  for (int ch : n.children)
    if (!c.is_hidden(ch)) kids.push_back(ch);
  const size_t count = kids.size();
  std::vector<Rect> slots(count);
  const auto emit_all = [&] {
    size_t vi = 0;
    for (int ch : n.children) {
      if (c.is_hidden(ch)) place(c, ch, Rect{content.x, content.y, 0, 0}, me, true);
      else place(c, ch, slots[vi++], me, true);
    }
  };
  if (count == 0) {
    emit_all();
    return;
  }
  const float gap = n.gap * c.scale;
  const int cols = n.cols > 0 ? std::min(n.cols, 64) : static_cast<int>(std::min<size_t>(count, 64));
  constexpr int kRowCap = 64;
  std::vector<char> occ(static_cast<size_t>(cols) * kRowCap, 0);
  struct Cell { int col, row, cs, rs; };
  std::vector<Cell> cells(count);
  int rows = std::max(1, n.rows);
  auto fits = [&](int col, int row, int cs, int rs) {
    if (col + cs > cols || row + rs > kRowCap) return false;
    for (int r = row; r < row + rs; ++r) for (int q = col; q < col + cs; ++q) if (occ[static_cast<size_t>(r) * cols + q]) return false;
    return true;
  };
  int cur = 0;
  for (size_t i = 0; i < count; ++i) {
    const Node& k = c.sk.nodes[kids[i]];
    int cs = std::min(k.colspan, cols), rs = std::min(k.rowspan, kRowCap);
    int col = 0, row = 0;
    if (k.col >= 0 && k.row >= 0) { col = std::min(k.col, cols - cs); row = std::min(k.row, kRowCap - rs); }
    else {
      bool found = false;
      for (int p = cur; p < cols * kRowCap && !found; ++p) {
        col = p % cols; row = p / cols;
        if (fits(col, row, cs, rs)) { found = true; cur = p; }
      }
      if (!found) { col = 0; row = 0; }
    }
    for (int r = row; r < row + rs && r < kRowCap; ++r) for (int q = col; q < col + cs; ++q) occ[static_cast<size_t>(r) * cols + q] = 1;
    cells[i] = {col, row, cs, rs};
    rows = std::max(rows, row + rs);
  }
  const float cw = std::max(0.0f, (content.w - gap * (cols - 1)) / cols);
  const float ch = std::max(0.0f, (content.h - gap * (rows - 1)) / rows);
  for (size_t i = 0; i < count; ++i) {
    const Cell& q = cells[i];
    slots[i] = Rect{content.x + q.col * (cw + gap), content.y + q.row * (ch + gap), cw * q.cs + gap * (q.cs - 1), ch * q.rs + gap * (q.rs - 1)};
  }
  emit_all();
}

void place(Ctx& c, int node, const Rect& slot, int parent, bool parent_flow) {
  const Node& n = c.sk.nodes[node];
  Rect r = finalize(n, slot, c.scale);
  const int me = emit(c, node, r, parent, parent_flow);
  if (!is_container(n.kind)) return;
  const float s = c.scale;
  Rect content = deflate(r, n.pad[0] * s, n.pad[1] * s, n.pad[2] * s, n.pad[3] * s);
  switch (n.kind) {
    case Kind::Row: flow(c, n, me, content, true); break;
    case Kind::Column: flow(c, n, me, content, false); break;
    case Kind::Grid: grid(c, n, me, content); break;
    default: {
      const Anchor def = n.kind == Kind::Free ? Anchor::TL : Anchor::C;
      for (int ch : n.children) {
        const Node& k = c.sk.nodes[ch];
        place(c, ch, anchored(k, content, s, def), me, false);
      }
    }
  }
}

std::vector<Point> shape_polygon(const Skin& sk, const Shape& sh, float W, float H, float scale) {
  std::vector<Point> p;
  const float pi = 3.14159265358979f;
  switch (sh.kind) {
    case ShapeKind::Polygon:
      for (auto& q : sh.points) p.push_back({q.x * W, q.y * H});
      break;
    case ShapeKind::Ellipse:
      for (int i = 0; i < 64; ++i) {
        float a = 2 * pi * static_cast<float>(i) / 64;
        p.push_back({W / 2 + std::cos(a) * W / 2, H / 2 + std::sin(a) * H / 2});
      }
      break;
    case ShapeKind::Chamfer: {
      float k = std::min({sh.amount * scale, W / 2, H / 2});
      p = {{k, 0}, {W - k, 0}, {W, k}, {W, H - k}, {W - k, H}, {k, H}, {0, H - k}, {0, k}};
      break;
    }
    case ShapeKind::RoundRect: {
      float r = std::min({sh.amount * scale, W / 2, H / 2});
      const Point centres[4] = {{W - r, r}, {W - r, H - r}, {r, H - r}, {r, r}};
      for (int corner = 0; corner < 4; ++corner)
        for (int i = 0; i <= 8; ++i) {
          float a = (static_cast<float>(corner) * 90 + 270 + 90.0f * static_cast<float>(i) / 8) * pi / 180;
          p.push_back({centres[corner].x + std::cos(a) * r, centres[corner].y + std::sin(a) * r});
        }
      break;
    }
  }
  (void)sk;
  return p;
}

bool in_polygon(const std::vector<Point>& poly, float x, float y) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    if ((poly[i].y > y) != (poly[j].y > y) &&
        x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
      in = !in;
  }
  return in;
}

}  // namespace

Layout solve_layout(const Skin& skin, size_t window_index, float width, float height, float scale,
                    const std::vector<char>* hidden_nodes) {
  Layout out;
  try {
    if (window_index >= skin.windows.size()) {
      out.warnings.push_back("no such window");
      return out;
    }
    const Window& w = skin.windows[window_index];
    if (!(scale > 0) || scale > 16) scale = 1;
    width = std::max(1.0f, width);
    height = std::max(1.0f, height);
    out.window = w.id;
    out.width = width;
    out.height = height;
    out.scale = scale;
    if (!w.shape.empty())
      if (const Shape* sh = skin.find_shape(w.shape)) out.shape = shape_polygon(skin, *sh, width, height, scale);
    if (width < w.min_w * scale || height < w.min_h * scale) out.warnings.push_back("window smaller than the skin's minimum size");
    Ctx c{skin, scale, out, hidden_nodes};
    if (w.root >= 0) place(c, w.root, Rect{0, 0, width, height}, -1, false);
    // final safety: nothing outside the window
    for (auto& rw : out.widgets) {
      float x0 = std::clamp(rw.rect.x, 0.0f, width), y0 = std::clamp(rw.rect.y, 0.0f, height);
      float x1 = std::clamp(rw.rect.right(), 0.0f, width), y1 = std::clamp(rw.rect.bottom(), 0.0f, height);
      rw.rect = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
  } catch (...) {
    out.warnings.push_back("layout failed");
  }
  return out;
}

int hit_test(const Layout& layout, float x, float y) {
  if (x < 0 || y < 0 || x >= layout.width || y >= layout.height) return -1;
  if (layout.shape.size() >= 3 && !in_polygon(layout.shape, x, y)) return -1;
  for (int i = static_cast<int>(layout.widgets.size()) - 1; i >= 0; --i) {
    const auto& w = layout.widgets[i];
    if (w.interactive && w.rect.contains(x, y)) return i;
  }
  return -1;
}

}  // namespace deckhand::skin
