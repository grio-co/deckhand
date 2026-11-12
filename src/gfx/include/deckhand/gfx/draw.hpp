#pragma once
// Minimal GPU-agnostic drawing: the UI is emitted as textured triangles plus a text
// atlas, and a platform shell renders them. Coordinates passed in are points; the
// emitted vertices are pixels (points * scale).
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace deckhand::gfx {

using Color = std::uint32_t;  // bytes in memory: R, G, B, A
constexpr Color rgba(int r, int g, int b, int a = 255) {
  return static_cast<Color>(r) | static_cast<Color>(g) << 8 | static_cast<Color>(b) << 16 |
         static_cast<Color>(a) << 24;
}
constexpr Color with_alpha(Color c, int a) { return (c & 0x00FFFFFFu) | static_cast<Color>(a) << 24; }

struct Vertex {
  float x, y, u, v;
  Color color;
};

struct DrawCmd {
  std::uint32_t texture = 1;
  float clip[4] = {0, 0, 0, 0};  // x0, y0, x1, y1 in pixels
  std::uint32_t index_offset = 0, index_count = 0;
};

struct Bitmap {
  int width = 0, height = 0;
  float ascent = 0;
  std::vector<std::uint8_t> alpha;
};

// Supplied by the platform: rasterise a UTF-8 string at a pixel size (weight 0 regular, 1 bold).
using TextRasterizer = std::function<bool(const std::string& utf8, float px, int weight, Bitmap& out)>;

enum class Align { Left, Center, Right };

class DrawList {
 public:
  static constexpr int kAtlasWidth = 2048, kAtlasHeight = 4096;

  explicit DrawList(TextRasterizer rasterizer);

  void begin(float width_px, float height_px, float scale);
  void push_clip(float x, float y, float w, float h);
  void pop_clip();

  void rect(float x, float y, float w, float h, Color c);
  void outline(float x, float y, float w, float h, Color c, float thickness = 1);
  void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
  // Draws text with its top-left at (x, y) for Left alignment; within max_width if > 0 (cropped).
  // Returns the drawn width in points.
  float text(float x, float y, const std::string& s, float size, Color c, Align align = Align::Left,
             float max_width = 0, int weight = 0);
  float text_width(const std::string& s, float size, int weight = 0);

  const std::vector<Vertex>& vertices() const { return vertices_; }
  const std::vector<std::uint32_t>& indices() const { return indices_; }
  const std::vector<DrawCmd>& commands() const { return commands_; }

  // Atlas (8-bit alpha). Dirty rows must be uploaded by the shell, then cleared.
  const std::uint8_t* atlas() const { return atlas_.data(); }
  bool atlas_dirty(int& y0, int& y1) const;
  void clear_atlas_dirty();
  float scale() const { return scale_; }

 private:
  struct Glyphs {
    float u0, v0, u1, v1;
    float w, h;  // pixels
  };
  const Glyphs* lookup(const std::string& s, float size, int weight);
  void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c);
  void reset_atlas();
  void new_command();

  TextRasterizer rasterizer_;
  std::vector<Vertex> vertices_;
  std::vector<std::uint32_t> indices_;
  std::vector<DrawCmd> commands_;
  std::vector<DrawCmd> clip_stack_;
  float width_ = 0, height_ = 0, scale_ = 1;

  std::vector<std::uint8_t> atlas_;
  std::unordered_map<std::string, Glyphs> cache_;
  int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
  int dirty_y0_ = 0, dirty_y1_ = 0;
  bool atlas_full_ = false;
  float white_u_ = 0, white_v_ = 0;
};

}  // namespace deckhand::gfx
