// Renders the core's draw list with Direct3D 11: one shader pair (vertex colour x texel, A8 textures supply only
// alpha), a scissor rectangle per command, a flip-model swap chain, or an offscreen target for --screenshot. The
// device is created on the GPU and falls back to the WARP software rasteriser when there is none.
#pragma once
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "com.hpp"
#include "deckhand/app/capi.h"

namespace deckhand::shell {

class Renderer {
 public:
  Renderer() = default;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // `hwnd` null: render to an offscreen target of width x height (read it back with read_pixels).
  bool init(HWND hwnd, int width, int height);
  void resize(int width, int height);  // swap-chain mode
  void render(const dh_frame& frame, int width_px, int height_px);
  // Presents the last frame; false when the window is occluded (the caller should not spin).
  bool present(bool vsync);
  // RGBA, top row first, of the last rendered frame.
  bool read_pixels(std::vector<std::uint8_t>& rgba, int& width, int& height);
  const std::string& adapter_name() const { return adapter_; }
  const std::string& error() const { return error_; }
  bool software() const { return software_; }

 private:
  struct Texture {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> view;
    std::uint32_t w = 0, h = 0;
    bool a8 = false;
  };
  bool create_device();
  bool create_shaders();
  bool create_targets(int width, int height);
  void apply(const dh_tex_upload& u);
  bool fail(const std::string& what, HRESULT hr);

  Com<ID3D11Device> device_;
  Com<ID3D11DeviceContext> ctx_;
  Com<IDXGISwapChain1> swap_;
  Com<ID3D11Texture2D> target_;  // offscreen render target (offscreen mode)
  Com<ID3D11RenderTargetView> rtv_;
  Com<ID3D11VertexShader> vs_;
  Com<ID3D11PixelShader> ps_;
  Com<ID3D11InputLayout> layout_;
  Com<ID3D11Buffer> vbo_, ibo_, cbuf_;
  Com<ID3D11BlendState> blend_;
  Com<ID3D11RasterizerState> raster_;
  Com<ID3D11SamplerState> sampler_;
  Com<ID3D11DepthStencilState> depth_;
  UINT vbo_cap_ = 0, ibo_cap_ = 0;  // bytes
  HWND hwnd_ = nullptr;
  int width_ = 0, height_ = 0;
  bool software_ = false;
  std::string adapter_, error_;
  std::unordered_map<std::uint32_t, Texture> textures_;
};

}  // namespace deckhand::shell
