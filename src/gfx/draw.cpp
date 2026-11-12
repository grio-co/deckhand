#include "deckhand/gfx/draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace deckhand::gfx {

DrawList::DrawList(TextRasterizer rasterizer) : rasterizer_(std::move(rasterizer)) {
  atlas_.resize(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight);
  reset_atlas();
}

void DrawList::reset_atlas() {
  std::fill(atlas_.begin(), atlas_.end(), std::uint8_t{0});
  cache_.clear();
  // A solid block at the origin: untextured shapes sample its centre.
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) atlas_[static_cast<std::size_t>(y) * kAtlasWidth + x] = 255;
  white_u_ = 2.0f / kAtlasWidth;
  white_v_ = 2.0f / kAtlasHeight;
  shelf_x_ = 6;
  shelf_y_ = 0;
  shelf_h_ = 6;
  dirty_y0_ = 0;
  dirty_y1_ = kAtlasHeight;
  atlas_full_ = false;
}

bool DrawList::atlas_dirty(int& y0, int& y1) const {
  y0 = dirty_y0_;
  y1 = dirty_y1_;
  return dirty_y1_ > dirty_y0_;
}

void DrawList::clear_atlas_dirty() { dirty_y0_ = dirty_y1_ = 0; }

void DrawList::begin(float width_px, float height_px, float scale) {
  if (atlas_full_) reset_atlas();  // start the frame clean rather than invalidating drawn text
  width_ = width_px;
  height_ = height_px;
  scale_ = scale > 0 ? scale : 1;
  vertices_.clear();
  indices_.clear();
  commands_.clear();
  clip_stack_.clear();
  DrawCmd c;
  c.clip[2] = width_px;
  c.clip[3] = height_px;
  clip_stack_.push_back(c);
  new_command();
}

void DrawList::new_command() {
  DrawCmd c = clip_stack_.back();
  c.index_offset = static_cast<std::uint32_t>(indices_.size());
  c.index_count = 0;
  if (!commands_.empty() && commands_.back().index_count == 0) commands_.back() = c;
  else commands_.push_back(c);
}

void DrawList::push_clip(float x, float y, float w, float h) {
  const DrawCmd& top = clip_stack_.back();
  DrawCmd c;
  c.clip[0] = std::max(top.clip[0], std::floor(x * scale_));
  c.clip[1] = std::max(top.clip[1], std::floor(y * scale_));
  c.clip[2] = std::max(c.clip[0], std::min(top.clip[2], std::ceil((x + w) * scale_)));
  c.clip[3] = std::max(c.clip[1], std::min(top.clip[3], std::ceil((y + h) * scale_)));
  clip_stack_.push_back(c);
  new_command();
}

void DrawList::pop_clip() {
  if (clip_stack_.size() > 1) clip_stack_.pop_back();
  new_command();
}

void DrawList::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) {
  const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
  vertices_.push_back({x0, y0, u0, v0, c});
  vertices_.push_back({x1, y0, u1, v0, c});
  vertices_.push_back({x1, y1, u1, v1, c});
  vertices_.push_back({x0, y1, u0, v1, c});
  for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices_.push_back(base + i);
  commands_.back().index_count += 6;
}

void DrawList::rect(float x, float y, float w, float h, Color c) {
  if (w <= 0 || h <= 0) return;
  quad(std::round(x * scale_), std::round(y * scale_), std::round((x + w) * scale_), std::round((y + h) * scale_),
       white_u_, white_v_, white_u_, white_v_, c);
}

void DrawList::outline(float x, float y, float w, float h, Color c, float t) {
  rect(x, y, w, t, c);
  rect(x, y + h - t, w, t, c);
  rect(x, y + t, t, h - 2 * t, c);
  rect(x + w - t, y + t, t, h - 2 * t, c);
}

void DrawList::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
  const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
  vertices_.push_back({x0 * scale_, y0 * scale_, white_u_, white_v_, c});
  vertices_.push_back({x1 * scale_, y1 * scale_, white_u_, white_v_, c});
  vertices_.push_back({x2 * scale_, y2 * scale_, white_u_, white_v_, c});
  for (std::uint32_t i : {0u, 1u, 2u}) indices_.push_back(base + i);
  commands_.back().index_count += 3;
}

namespace {
constexpr float kPi = 3.14159265358979f;
}

void DrawList::polygon(const float* xy, int n, Color c) {
  for (int i = 1; i + 1 < n; ++i) triangle(xy[0], xy[1], xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], c);
}

void DrawList::rrect(float x, float y, float w, float h, float radius, Color c) {
  if (w <= 0 || h <= 0) return;
  radius = std::min({radius, w / 2, h / 2});
  if (radius < 0.75f) {
    rect(x, y, w, h, c);
    return;
  }
  const int seg = std::clamp(static_cast<int>(radius * scale_ / 2) + 3, 4, 16);
  std::vector<float> pts;
  pts.reserve(static_cast<std::size_t>(seg + 1) * 8);
  const float cx[4] = {x + w - radius, x + w - radius, x + radius, x + radius};
  const float cy[4] = {y + radius, y + h - radius, y + h - radius, y + radius};
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i <= seg; ++i) {
      const float a = (static_cast<float>(k) * 0.5f + 1.5f) * kPi + static_cast<float>(i) / static_cast<float>(seg) * 0.5f * kPi;
      pts.push_back(cx[k] + std::cos(a) * radius);
      pts.push_back(cy[k] + std::sin(a) * radius);
    }
  }
  polygon(pts.data(), static_cast<int>(pts.size() / 2), c);
}

void DrawList::rrect_outline(float x, float y, float w, float h, float radius, Color c, float t) {
  if (w <= 0 || h <= 0) return;
  radius = std::min({radius, w / 2, h / 2});
  if (radius < 0.75f) {
    outline(x, y, w, h, c, t);
    return;
  }
  const int seg = std::clamp(static_cast<int>(radius * scale_ / 2) + 3, 4, 16);
  const float cx[4] = {x + w - radius, x + w - radius, x + radius, x + radius};
  const float cy[4] = {y + radius, y + h - radius, y + h - radius, y + radius};
  const float ri = std::max(0.0f, radius - t);
  std::vector<float> outer, inner;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i <= seg; ++i) {
      const float a = (static_cast<float>(k) * 0.5f + 1.5f) * kPi + static_cast<float>(i) / static_cast<float>(seg) * 0.5f * kPi;
      outer.push_back(cx[k] + std::cos(a) * radius);
      outer.push_back(cy[k] + std::sin(a) * radius);
      inner.push_back(cx[k] + std::cos(a) * ri);
      inner.push_back(cy[k] + std::sin(a) * ri);
    }
  }
  const std::size_t n = outer.size() / 2;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = (i + 1) % n;
    triangle(outer[2 * i], outer[2 * i + 1], outer[2 * j], outer[2 * j + 1], inner[2 * i], inner[2 * i + 1], c);
    triangle(inner[2 * i], inner[2 * i + 1], outer[2 * j], outer[2 * j + 1], inner[2 * j], inner[2 * j + 1], c);
  }
}

void DrawList::line(float x0, float y0, float x1, float y1, float t, Color c) {
  const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f) return;
  const float nx = -dy / len * t / 2, ny = dx / len * t / 2;
  triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
  triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

void DrawList::circle(float cx, float cy, float r, Color c) {
  if (r <= 0) return;
  const int seg = std::clamp(static_cast<int>(r * scale_ / 2) + 8, 12, 64);
  for (int i = 0; i < seg; ++i) {
    const float a0 = 2 * kPi * static_cast<float>(i) / static_cast<float>(seg), a1 = 2 * kPi * static_cast<float>(i + 1) / static_cast<float>(seg);
    triangle(cx, cy, cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r, cy + std::sin(a1) * r, c);
  }
}

void DrawList::arc(float cx, float cy, float r, float a0, float a1, float t, Color c) {
  if (r <= 0 || a0 == a1) return;
  if (a1 < a0) std::swap(a0, a1);
  const int seg = std::clamp(static_cast<int>((a1 - a0) / (2 * kPi) * (r * scale_ + 16)) + 2, 2, 96);
  const float ro = r + t / 2, ri = std::max(0.0f, r - t / 2);
  float ox = cx + std::cos(a0) * ro, oy = cy + std::sin(a0) * ro, ix = cx + std::cos(a0) * ri, iy = cy + std::sin(a0) * ri;
  for (int i = 1; i <= seg; ++i) {
    const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(seg);
    const float nox = cx + std::cos(a) * ro, noy = cy + std::sin(a) * ro, nix = cx + std::cos(a) * ri, niy = cy + std::sin(a) * ri;
    triangle(ox, oy, nox, noy, ix, iy, c);
    triangle(ix, iy, nox, noy, nix, niy, c);
    ox = nox; oy = noy; ix = nix; iy = niy;
  }
}

float DrawList::text_height(const std::string& s, float size, int weight) {
  const Glyphs* g = lookup(s, size, weight);
  return g ? g->h / scale_ : size * 1.25f;
}

const DrawList::Glyphs* DrawList::lookup(const std::string& s, float size, int weight) {
  if (s.empty() || !rasterizer_) return nullptr;
  const int px4 = static_cast<int>(std::lround(size * scale_ * 4));
  std::string key = s;
  key.push_back('\x1f');
  key += std::to_string(px4 * 2 + (weight ? 1 : 0));
  const auto it = cache_.find(key);
  if (it != cache_.end()) return &it->second;
  if (atlas_full_) return nullptr;

  Bitmap bmp;
  if (!rasterizer_(s, static_cast<float>(px4) / 4.0f, weight, bmp) || bmp.width <= 0 || bmp.height <= 0) return nullptr;
  const int w = std::min(bmp.width, kAtlasWidth - 8), h = bmp.height;
  if (shelf_x_ + w + 2 > kAtlasWidth) {
    shelf_y_ += shelf_h_ + 2;
    shelf_x_ = 0;
    shelf_h_ = 0;
  }
  if (shelf_y_ + h + 2 > kAtlasHeight) {
    atlas_full_ = true;  // rebuilt at the start of the next frame
    return nullptr;
  }
  for (int y = 0; y < h; ++y) {
    std::memcpy(&atlas_[static_cast<std::size_t>(shelf_y_ + y) * kAtlasWidth + static_cast<std::size_t>(shelf_x_)],
                &bmp.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width)], static_cast<std::size_t>(w));
  }
  if (dirty_y1_ <= dirty_y0_) {
    dirty_y0_ = shelf_y_;
    dirty_y1_ = shelf_y_ + h;
  } else {
    dirty_y0_ = std::min(dirty_y0_, shelf_y_);
    dirty_y1_ = std::max(dirty_y1_, shelf_y_ + h);
  }
  Glyphs g;
  g.u0 = static_cast<float>(shelf_x_) / kAtlasWidth;
  g.v0 = static_cast<float>(shelf_y_) / kAtlasHeight;
  g.u1 = static_cast<float>(shelf_x_ + w) / kAtlasWidth;
  g.v1 = static_cast<float>(shelf_y_ + h) / kAtlasHeight;
  g.w = static_cast<float>(w);
  g.h = static_cast<float>(h);
  shelf_x_ += w + 2;
  shelf_h_ = std::max(shelf_h_, h);
  return &cache_.emplace(std::move(key), g).first->second;
}

float DrawList::text_width(const std::string& s, float size, int weight) {
  const Glyphs* g = lookup(s, size, weight);
  return g ? g->w / scale_ : 0;
}

float DrawList::text(float x, float y, const std::string& s, float size, Color c, Align align, float max_width,
                     int weight) {
  const Glyphs* g = lookup(s, size, weight);
  if (!g) return 0;
  float w = g->w, u1 = g->u1;
  if (max_width > 0 && w > max_width * scale_) {  // crop to the available width
    w = std::floor(max_width * scale_);
    u1 = g->u0 + (g->u1 - g->u0) * (w / g->w);
  }
  float px = std::round(x * scale_);
  if (align == Align::Center) px = std::round(x * scale_ - w / 2);
  if (align == Align::Right) px = std::round(x * scale_ - w);
  const float py = std::round(y * scale_);
  quad(px, py, px + w, py + g->h, g->u0, g->v0, u1, g->v1, c);
  return w / scale_;
}

}  // namespace deckhand::gfx
