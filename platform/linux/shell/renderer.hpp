// Renders the core's draw list with one pipeline: vertex colour x A8 texture alpha, scissor per command.
// OpenGL 3.3 core, or OpenGL ES 3.0 when that is what the driver gives.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "deckhand/app/capi.h"

namespace deckhand::shell {

class Renderer {
 public:
  Renderer() = default;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  ~Renderer();

  // A GL context must be current. `gles` selects the ES 3.0 shader dialect.
  bool init(bool gles);
  void render(const dh_frame& frame, int width_px, int height_px);
  // Reads the back buffer (RGBA, top row first). Call after render(), before swapping.
  bool read_pixels(int width_px, int height_px, std::vector<std::uint8_t>& rgba) const;

 private:
  struct Texture {
    unsigned id = 0;
    std::uint32_t w = 0, h = 0;
  };
  void apply(const dh_tex_upload& u);

  unsigned program_ = 0, vao_ = 0, vbo_ = 0, ebo_ = 0;
  int viewport_loc_ = -1;
  std::unordered_map<std::uint32_t, Texture> textures_;
};

// Writes an RGBA image as a PNG (stored deflate blocks, no compression). Returns false on I/O failure.
bool write_png(const char* path, int width, int height, const std::uint8_t* rgba);

}  // namespace deckhand::shell
