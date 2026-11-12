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
  // Additive shape helpers (all in points). Radii are clamped to half the shorter side.
  void rrect(float x, float y, float w, float h, float radius, Color c);
  void rrect_outline(float x, float y, float w, float h, float radius, Color c, float thickness = 1);
  void line(float x0, float y0, float x1, float y1, float thickness, Color c);
  void circle(float cx, float cy, float r, Color c);
  // Arc of a ring, angles in radians (0 = +x, clockwise on screen), from a0 to a1.
  void arc(float cx, float cy, float r, float a0, float a1, float thickness, Color c);
  // Convex polygon fill (triangle fan from the first point).
  void polygon(const float* xy, int count, Color c);
  // Draws text with its top-left at (x, y) for Left alignment; within max_width if > 0 (cropped).
  // Returns the drawn width in points.
  float text(float x, float y, const std::string& s, float size, Color c, Align align = Align::Left,
             float max_width = 0, int weight = 0);
  float text_width(const std::string& s, float size, int weight = 0);
  float text_height(const std::string& s, float size, int weight = 0);

  const std::vector<Vertex>& vertices() const { return vertices_; }
  const std::vector<std::uint32_t>& indices() const { return indices_; }
  const std::vector<DrawCmd>& commands() const { return commands_; }

  // ---- RGBA images (skin sprites). Texture ids start at 2; id 1 is the glyph atlas. ----
  // The pixels are copied; the shell receives them as one full-texture upload on the next frame.
  std::uint32_t add_image(int width, int height, std::vector<std::uint8_t> rgba);
  void remove_image(std::uint32_t id);  // the shell is told to destroy the texture on the next frame
  struct ImageUpload {
    std::uint32_t id;
    int width, height;
    const std::uint8_t* pixels;  // valid until the image is removed
  };
  std::vector<ImageUpload> take_image_uploads();            // images added since the last call
  std::vector<std::uint32_t> take_image_releases();         // images removed since the last call
  // Draws part of an image (u/v in 0..1) into a rectangle (points), tinted by `tint` (white = unchanged),
  // optionally rotated about the rectangle's centre by `radians` (clockwise on screen).
  void image(std::uint32_t id, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
             Color tint = 0xFFFFFFFFu, float radians = 0);

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
  void use_texture(std::uint32_t id);

  struct Stored {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    bool pending = true;
  };
  std::unordered_map<std::uint32_t, Stored> images_;
  std::vector<std::uint32_t> released_;
  std::uint32_t next_image_ = 2, cur_texture_ = 1;

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
