#include "renderer.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace deckhand::shell {
namespace {

// Same arithmetic as the Metal and OpenGL shells: vertex colour, alpha multiplied by the texel for A8 textures,
// colour times texel for RGBA8 images. The target is not sRGB, so values pass straight through.
const char kShader[] = R"(
cbuffer Frame : register(b0) { float2 viewport; float a8; float pad; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VOut vs_main(VIn i) {
  VOut o;
  o.pos = float4(i.pos.x / viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / viewport.y * 2.0, 0.0, 1.0);
  o.uv = i.uv;
  o.col = i.col;
  return o;
}
float4 ps_main(VOut i) : SV_Target {
  float4 t = tex.Sample(smp, i.uv);
  return a8 > 0.5 ? float4(i.col.rgb, i.col.a * t.r) : i.col * t;
}
)";

std::string narrow(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

using CompileFn = decltype(&D3DCompile);

CompileFn load_compiler() {
  // The compiler ships with Windows 10 and later as d3dcompiler_47.dll; it is loaded at run time so the exe has no
  // import for it (and no redistributable to carry).
  static CompileFn fn = [] {
    for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
      if (HMODULE m = LoadLibraryW(name))
        if (auto f = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")))) return f;
    }
    return static_cast<CompileFn>(nullptr);
  }();
  return fn;
}

struct FrameConstants {
  float viewport[2];
  float a8;
  float pad;
};

template <class T>
bool create_dynamic_buffer(ID3D11Device* dev, UINT bytes, UINT bind, Com<T>& out) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = bytes;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = bind;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return SUCCEEDED(dev->CreateBuffer(&d, nullptr, reinterpret_cast<ID3D11Buffer**>(out.put())));
}

}  // namespace

bool Renderer::fail(const std::string& what, HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
  error_ = what + " failed (" + buf + ")";
  return false;
}

bool Renderer::create_device() {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = E_FAIL;
  for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(sizeof levels / sizeof levels[0]), D3D11_SDK_VERSION, device_.put(), &got, ctx_.put());
    if (SUCCEEDED(hr)) {
      software_ = type == D3D_DRIVER_TYPE_WARP;
      break;
    }
  }
  if (FAILED(hr)) return fail("Direct3D 11 device", hr);
  Com<IDXGIDevice> dxgi;
  Com<IDXGIAdapter> adapter;
  if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void())) && SUCCEEDED(dxgi->GetAdapter(adapter.put()))) {
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(adapter->GetDesc(&d))) adapter_ = narrow(d.Description);
  }
  return true;
}

bool Renderer::create_shaders() {
  const CompileFn compile = load_compiler();
  if (!compile) {
    error_ = "d3dcompiler_47.dll is not available";
    return false;
  }
  auto build = [&](const char* entry, const char* target, Com<ID3DBlob>& code) {
    Com<ID3DBlob> errors;
    const HRESULT hr = compile(kShader, sizeof kShader - 1, "deckhand", nullptr, nullptr, entry, target, 0, 0, code.put(), errors.put());
    if (FAILED(hr)) {
      error_ = std::string("shader compile failed: ") +
               (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
      return false;
    }
    return true;
  };
  Com<ID3DBlob> vcode, pcode;
  if (!build("vs_main", "vs_4_0", vcode) || !build("ps_main", "ps_4_0", pcode)) return false;
  HRESULT hr = device_->CreateVertexShader(vcode->GetBufferPointer(), vcode->GetBufferSize(), nullptr, vs_.put());
  if (FAILED(hr)) return fail("CreateVertexShader", hr);
  hr = device_->CreatePixelShader(pcode->GetBufferPointer(), pcode->GetBufferSize(), nullptr, ps_.put());
  if (FAILED(hr)) return fail("CreatePixelShader", hr);
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(dh_vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(dh_vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(dh_vertex, rgba), D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr = device_->CreateInputLayout(elements, 3, vcode->GetBufferPointer(), vcode->GetBufferSize(), layout_.put());
  if (FAILED(hr)) return fail("CreateInputLayout", hr);

  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(FrameConstants);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  hr = device_->CreateBuffer(&cb, nullptr, cbuf_.put());
  if (FAILED(hr)) return fail("CreateBuffer", hr);

  D3D11_BLEND_DESC bd{};
  auto& rt = bd.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device_->CreateBlendState(&bd, blend_.put());
  if (FAILED(hr)) return fail("CreateBlendState", hr);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.ScissorEnable = TRUE;
  rd.DepthClipEnable = TRUE;
  hr = device_->CreateRasterizerState(&rd, raster_.put());
  if (FAILED(hr)) return fail("CreateRasterizerState", hr);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  hr = device_->CreateSamplerState(&sd, sampler_.put());
  if (FAILED(hr)) return fail("CreateSamplerState", hr);

  D3D11_DEPTH_STENCIL_DESC dd{};  // depth and stencil off
  hr = device_->CreateDepthStencilState(&dd, depth_.put());
  if (FAILED(hr)) return fail("CreateDepthStencilState", hr);
  return true;
}

bool Renderer::create_targets(int width, int height) {
  rtv_.reset();
  target_.reset();
  width_ = width;
  height_ = height;
  if (hwnd_) {
    Com<ID3D11Texture2D> back;
    HRESULT hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
    if (FAILED(hr)) return fail("swap chain buffer", hr);
    hr = device_->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
    if (FAILED(hr)) return fail("CreateRenderTargetView", hr);
    return true;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(width);
  td.Height = static_cast<UINT>(height);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  HRESULT hr = device_->CreateTexture2D(&td, nullptr, target_.put());
  if (FAILED(hr)) return fail("offscreen target", hr);
  hr = device_->CreateRenderTargetView(target_.get(), nullptr, rtv_.put());
  if (FAILED(hr)) return fail("CreateRenderTargetView", hr);
  return true;
}

bool Renderer::init(HWND hwnd, int width, int height) {
  hwnd_ = hwnd;
  if (width < 1 || height < 1) return fail("render size", E_INVALIDARG);
  if (!create_device() || !create_shaders()) return false;
  if (hwnd) {
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory2> factory;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void());
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(adapter.put());
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
    if (FAILED(hr)) return fail("DXGI factory", hr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(width);
    sd.Height = static_cast<UINT>(height);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // flip model: no copy, lowest latency
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(device_.get(), hwnd, &sd, nullptr, nullptr, swap_.put());
    if (FAILED(hr)) {  // some drivers lack flip-model or DXGI_SCALING_NONE: try the older discard model
      sd.Scaling = DXGI_SCALING_STRETCH;
      sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      sd.BufferCount = 1;
      hr = factory->CreateSwapChainForHwnd(device_.get(), hwnd, &sd, nullptr, nullptr, swap_.put());
    }
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
  }
  return create_targets(width, height);
}

void Renderer::resize(int width, int height) {
  if (!hwnd_ || width < 1 || height < 1 || (width == width_ && height == height_)) return;
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  rtv_.reset();
  const HRESULT hr = swap_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr)) {
    fail("ResizeBuffers", hr);
    return;
  }
  create_targets(width, height);
}

void Renderer::apply(const dh_tex_upload& u) {
  if (!u.pixels || u.w == 0 || u.h == 0) return;
  const bool a8 = u.fmt == DH_TEX_A8;
  Texture& t = textures_[u.id];
  if (!t.tex || t.w != u.tex_w || t.h != u.tex_h || t.a8 != a8) {
    t = Texture{};
    t.w = u.tex_w;
    t.h = u.tex_h;
    t.a8 = a8;
    const UINT bpp = a8 ? 1 : 4;
    std::vector<std::uint8_t> zero(static_cast<std::size_t>(u.tex_w) * u.tex_h * bpp, 0);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = u.tex_w;
    td.Height = u.tex_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = a8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{zero.data(), u.tex_w * bpp, 0};
    if (FAILED(device_->CreateTexture2D(&td, &init, t.tex.put())) ||
        FAILED(device_->CreateShaderResourceView(t.tex.get(), nullptr, t.view.put()))) {
      textures_.erase(u.id);
      return;
    }
  }
  D3D11_BOX box{u.x, u.y, 0, u.x + u.w, u.y + u.h, 1};
  ctx_->UpdateSubresource(t.tex.get(), 0, &box, u.pixels, u.stride, 0);
}

void Renderer::render(const dh_frame& f, int width, int height) {
  if (!rtv_) return;
  for (std::uint32_t i = 0; i < f.upload_count; ++i) apply(f.uploads[i]);
  for (std::uint32_t i = 0; i < f.release_count; ++i) textures_.erase(f.releases[i]);

  ID3D11RenderTargetView* rtv = rtv_.get();
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->ClearRenderTargetView(rtv, f.clear_rgba);
  if (f.vtx_count == 0 || f.idx_count == 0 || width <= 0 || height <= 0) return;

  const UINT vbytes = f.vtx_count * static_cast<UINT>(sizeof(dh_vertex)), ibytes = f.idx_count * 4u;
  if (!vbo_ || vbytes > vbo_cap_) {
    vbo_cap_ = std::max(vbytes, vbo_cap_ * 2);
    if (!create_dynamic_buffer(device_.get(), vbo_cap_, D3D11_BIND_VERTEX_BUFFER, vbo_)) return;
  }
  if (!ibo_ || ibytes > ibo_cap_) {
    ibo_cap_ = std::max(ibytes, ibo_cap_ * 2);
    if (!create_dynamic_buffer(device_.get(), ibo_cap_, D3D11_BIND_INDEX_BUFFER, ibo_)) return;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(vbo_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, f.vtx, vbytes);
    ctx_->Unmap(vbo_.get(), 0);
  }
  if (SUCCEEDED(ctx_->Map(ibo_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, f.idx, ibytes);
    ctx_->Unmap(ibo_.get(), 0);
  }

  FrameConstants fc{{static_cast<float>(width), static_cast<float>(height)}, 0, 0};
  ctx_->IASetInputLayout(layout_.get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11Buffer* vb = vbo_.get();
  const UINT stride = sizeof(dh_vertex), offset = 0;
  ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx_->IASetIndexBuffer(ibo_.get(), DXGI_FORMAT_R32_UINT, 0);
  ctx_->VSSetShader(vs_.get(), nullptr, 0);
  ctx_->PSSetShader(ps_.get(), nullptr, 0);
  ID3D11Buffer* cb = cbuf_.get();
  ctx_->VSSetConstantBuffers(0, 1, &cb);
  ctx_->PSSetConstantBuffers(0, 1, &cb);
  ID3D11SamplerState* sm = sampler_.get();
  ctx_->PSSetSamplers(0, 1, &sm);
  const float blend_factor[4] = {0, 0, 0, 0};
  ctx_->OMSetBlendState(blend_.get(), blend_factor, 0xFFFFFFFFu);
  ctx_->OMSetDepthStencilState(depth_.get(), 0);
  ctx_->RSSetState(raster_.get());

  int bound_a8 = -1;
  for (std::uint32_t i = 0; i < f.cmd_count; ++i) {
    const dh_draw_cmd& c = f.cmds[i];
    const auto it = textures_.find(c.texture_id);
    if (it == textures_.end() || !it->second.view || c.idx_count == 0) continue;
    const int x0 = std::max(0, static_cast<int>(c.clip[0])), y0 = std::max(0, static_cast<int>(c.clip[1]));
    const int x1 = std::min(width, static_cast<int>(c.clip[2])), y1 = std::min(height, static_cast<int>(c.clip[3]));
    if (x1 <= x0 || y1 <= y0) continue;
    const D3D11_RECT scissor{x0, y0, x1, y1};
    ctx_->RSSetScissorRects(1, &scissor);
    if (bound_a8 != (it->second.a8 ? 1 : 0)) {
      bound_a8 = it->second.a8 ? 1 : 0;
      fc.a8 = it->second.a8 ? 1.0f : 0.0f;
      ctx_->UpdateSubresource(cbuf_.get(), 0, nullptr, &fc, 0, 0);
    }
    ID3D11ShaderResourceView* srv = it->second.view.get();
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->DrawIndexed(c.idx_count, c.idx_offset, static_cast<INT>(c.vtx_offset));
  }
}

bool Renderer::present(bool vsync) {
  if (!swap_) return true;
  const HRESULT hr = swap_->Present(vsync ? 1 : 0, 0);
  return hr != DXGI_STATUS_OCCLUDED;
}

bool Renderer::read_pixels(std::vector<std::uint8_t>& rgba, int& width, int& height) {
  Com<ID3D11Texture2D> back;
  ID3D11Texture2D* raw = target_.get();
  if (hwnd_) {
    if (!swap_ || FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) return false;
    raw = back.get();
  }
  if (!raw) return false;
  D3D11_TEXTURE2D_DESC td{};
  raw->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  Com<ID3D11Texture2D> staging;
  bool ok = SUCCEEDED(device_->CreateTexture2D(&td, nullptr, staging.put()));
  if (ok) {
    ctx_->CopyResource(staging.get(), raw);
    D3D11_MAPPED_SUBRESOURCE m{};
    ok = SUCCEEDED(ctx_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
      width = static_cast<int>(td.Width);
      height = static_cast<int>(td.Height);
      rgba.resize(static_cast<std::size_t>(width) * height * 4);
      for (int y = 0; y < height; ++y) {
        const std::uint8_t* s = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        std::uint8_t* d = &rgba[static_cast<std::size_t>(y) * width * 4];
        for (int x = 0; x < width; ++x) {  // BGRA -> RGBA
          d[4 * x] = s[4 * x + 2];
          d[4 * x + 1] = s[4 * x + 1];
          d[4 * x + 2] = s[4 * x];
          d[4 * x + 3] = s[4 * x + 3];
        }
      }
      ctx_->Unmap(staging.get(), 0);
    }
  }
  return ok;
}

}  // namespace deckhand::shell
