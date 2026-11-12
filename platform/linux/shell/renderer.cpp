#include "renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace deckhand::shell {
namespace {

// Every GL entry point is resolved at run time through SDL, so the binary links no GL library and the
// same code serves desktop OpenGL and OpenGL ES. GL 1.x functions have prototypes in the header (their
// pointer type comes from decltype); newer ones use the PFN typedefs.
#define DH_GL_V1(X)                                                                                              \
  X(glGenTextures) X(glDeleteTextures) X(glBindTexture) X(glTexImage2D) X(glTexSubImage2D) X(glTexParameteri)  \
  X(glPixelStorei) X(glViewport) X(glEnable) X(glDisable) X(glClearColor) X(glClear) X(glScissor)               \
  X(glDrawElements) X(glReadPixels)
#define DH_GL_V2(X)                                                                                              \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLCREATESHADERPROC, glCreateShader)                           \
  X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader)                           \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                       \
  X(PFNGLDELETESHADERPROC, glDeleteShader) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                           \
  X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)                               \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                   \
  X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram)                               \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM2FPROC, glUniform2f)                       \
  X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                             \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)          \
  X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
  X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)     \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)

#define X(name) decltype(&::name) p_##name = nullptr;
DH_GL_V1(X)
#undef X
#define X(type, name) type p_##name = nullptr;
DH_GL_V2(X)
#undef X

bool load_functions() {
  bool ok = true;
#define X(name)                                                                       \
  p_##name = reinterpret_cast<decltype(p_##name)>(SDL_GL_GetProcAddress(#name));      \
  if (!p_##name) {                                                                    \
    SDL_Log("DeckHand: OpenGL function %s is missing", #name);                        \
    ok = false;                                                                       \
  }
  DH_GL_V1(X)
#undef X
#define X(type, name)                                                                 \
  p_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                    \
  if (!p_##name) {                                                                    \
    SDL_Log("DeckHand: OpenGL function %s is missing", #name);                        \
    ok = false;                                                                       \
  }
  DH_GL_V2(X)
#undef X
  return ok;
}

// From here on the unprefixed names mean the loaded pointers.
#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glPixelStorei p_glPixelStorei
#define glViewport p_glViewport
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glClearColor p_glClearColor
#define glClear p_glClear
#define glScissor p_glScissor
#define glDrawElements p_glDrawElements
#define glReadPixels p_glReadPixels
#define glActiveTexture p_glActiveTexture
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glDeleteShader p_glDeleteShader
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glDeleteProgram p_glDeleteProgram
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform2f p_glUniform2f
#define glUniform1i p_glUniform1i
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glDeleteBuffers p_glDeleteBuffers
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glBlendFuncSeparate p_glBlendFuncSeparate

const char* kVertexBody = R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform vec2 u_viewport;
out vec2 v_uv;
out vec4 v_color;
void main() {
  gl_Position = vec4(a_pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - a_pos.y / u_viewport.y * 2.0, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
}
)";

// Same arithmetic as the Metal shell: vertex colour, alpha multiplied by the texel's red channel. The
// framebuffer is not sRGB, so values pass straight through exactly as on the bgra8Unorm Metal layer.
const char* kFragmentBody = R"(
uniform sampler2D u_tex;
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() {
  o_color = vec4(v_color.rgb, v_color.a * texture(u_tex, v_uv).r);
}
)";

unsigned compile(GLenum kind, const std::string& source) {
  const unsigned shader = glCreateShader(kind);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof log - 1, nullptr, log);
    SDL_Log("DeckHand: shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t n) {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}

void chunk(std::FILE* f, const char* type, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> head;
  put32(head, static_cast<std::uint32_t>(body.size()));
  std::fwrite(head.data(), 1, 4, f);
  std::fwrite(type, 1, 4, f);
  std::uint32_t crc = crc32_update(0, reinterpret_cast<const std::uint8_t*>(type), 4);
  if (!body.empty()) {
    std::fwrite(body.data(), 1, body.size(), f);
    crc = crc32_update(crc, body.data(), body.size());
  }
  std::vector<std::uint8_t> tail;
  put32(tail, crc);
  std::fwrite(tail.data(), 1, 4, f);
}

}  // namespace

Renderer::~Renderer() {
  if (!program_) return;
  for (auto& [id, t] : textures_) glDeleteTextures(1, &t.id);
  glDeleteBuffers(1, &vbo_);
  glDeleteBuffers(1, &ebo_);
  glDeleteVertexArrays(1, &vao_);
  glDeleteProgram(program_);
}

bool Renderer::init(bool gles) {
  if (!load_functions()) return false;
  const std::string header = gles ? "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n"
                                  : "#version 330 core\n";
  const unsigned vs = compile(GL_VERTEX_SHADER, header + kVertexBody);
  const unsigned fs = compile(GL_FRAGMENT_SHADER, header + kFragmentBody);
  if (!vs || !fs) return false;
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024] = {0};
    glGetProgramInfoLog(program_, sizeof log - 1, nullptr, log);
    SDL_Log("DeckHand: shader link failed: %s", log);
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }
  viewport_loc_ = glGetUniformLocation(program_, "u_viewport");
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glGenBuffers(1, &ebo_);
  return true;
}

void Renderer::apply(const dh_tex_upload& u) {
  if (!u.pixels || u.w == 0 || u.h == 0) return;
  const bool a8 = u.fmt == DH_TEX_A8;
  Texture& t = textures_[u.id];
  if (!t.id) glGenTextures(1, &t.id);
  glBindTexture(GL_TEXTURE_2D, t.id);
  if (t.w != u.tex_w || t.h != u.tex_h) {
    t.w = u.tex_w;
    t.h = u.tex_h;
    glTexImage2D(GL_TEXTURE_2D, 0, a8 ? GL_R8 : GL_RGBA8, static_cast<GLsizei>(u.tex_w),
                 static_cast<GLsizei>(u.tex_h), 0, a8 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(u.stride / (a8 ? 1 : 4)));
  glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(u.x), static_cast<GLint>(u.y), static_cast<GLsizei>(u.w),
                  static_cast<GLsizei>(u.h), a8 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, u.pixels);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void Renderer::render(const dh_frame& f, int width, int height) {
  for (std::uint32_t i = 0; i < f.upload_count; ++i) apply(f.uploads[i]);
  for (std::uint32_t i = 0; i < f.release_count; ++i) {
    const auto it = textures_.find(f.releases[i]);
    if (it == textures_.end()) continue;
    glDeleteTextures(1, &it->second.id);
    textures_.erase(it);
  }

  glViewport(0, 0, width, height);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(f.clear_rgba[0], f.clear_rgba[1], f.clear_rgba[2], f.clear_rgba[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  if (f.vtx_count == 0 || f.idx_count == 0 || width <= 0 || height <= 0) return;

  glUseProgram(program_);
  glUniform2f(viewport_loc_, static_cast<float>(width), static_cast<float>(height));
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_SCISSOR_TEST);
  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(f.vtx_count) * sizeof(dh_vertex), f.vtx, GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(f.idx_count) * 4, f.idx, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);

  for (std::uint32_t i = 0; i < f.cmd_count; ++i) {
    const dh_draw_cmd& c = f.cmds[i];
    const auto it = textures_.find(c.texture_id);
    if (it == textures_.end() || c.idx_count == 0) continue;
    const int x0 = std::max(0, static_cast<int>(c.clip[0])), y0 = std::max(0, static_cast<int>(c.clip[1]));
    const int x1 = std::min(width, static_cast<int>(c.clip[2])), y1 = std::min(height, static_cast<int>(c.clip[3]));
    if (x1 <= x0 || y1 <= y0) continue;
    glScissor(x0, height - y1, x1 - x0, y1 - y0);  // GL's origin is the bottom-left corner
    glBindTexture(GL_TEXTURE_2D, it->second.id);
    // The vertex base is applied by offsetting the attribute pointers (glDrawElementsBaseVertex is not in ES 3.0).
    const auto* base = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(c.vtx_offset) * sizeof(dh_vertex));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(dh_vertex), base + offsetof(dh_vertex, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(dh_vertex), base + offsetof(dh_vertex, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(dh_vertex), base + offsetof(dh_vertex, rgba));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(c.idx_count), GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(static_cast<std::uintptr_t>(c.idx_offset) * 4));
  }
  glDisable(GL_SCISSOR_TEST);
}

bool Renderer::read_pixels(int w, int h, std::vector<std::uint8_t>& rgba) const {
  if (w <= 0 || h <= 0) return false;
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
  rgba.resize(raw.size());
  const std::size_t row = static_cast<std::size_t>(w) * 4;
  for (int y = 0; y < h; ++y) std::memcpy(&rgba[y * row], &raw[(h - 1 - y) * row], row);
  return true;
}

bool write_png(const char* path, int w, int h, const std::uint8_t* rgba) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(sig, 1, 8, f);
  std::vector<std::uint8_t> ihdr;
  put32(ihdr, static_cast<std::uint32_t>(w));
  put32(ihdr, static_cast<std::uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  chunk(f, "IHDR", ihdr);

  // Scanlines (filter byte 0 + RGB), wrapped in zlib "stored" blocks.
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* p = rgba + (static_cast<std::size_t>(y) * w + x) * 4;
      raw.insert(raw.end(), {p[0], p[1], p[2]});
    }
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};
  for (std::size_t at = 0; at < raw.size() || at == 0;) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n & 0xFF));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    if (last) break;
  }
  std::uint32_t a = 1, b = 0;
  for (std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  return std::fclose(f) == 0;
}

}  // namespace deckhand::shell
