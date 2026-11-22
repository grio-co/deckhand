#include "text.hpp"

#include <windows.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "com.hpp"

namespace deckhand::shell {
namespace {

std::wstring widen(const char* s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

// Receives each shaped glyph run of a layout and adds its coverage to an A8 buffer.
class Collector final : public IDWriteTextRenderer {
 public:
  Collector(IDWriteFactory* factory, std::uint8_t* alpha, int w, int h) : factory_(factory), alpha_(alpha), w_(w), h_(h) {}

  // IUnknown (stack object: reference counting is a formality)
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  // IDWritePixelSnapping
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
    *disabled = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
    *m = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
    *ppd = 1.0f;
    return S_OK;
  }

  // IDWriteTextRenderer
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
    Com<IDWriteGlyphRunAnalysis> analysis;
    const DWRITE_MEASURING_MODE m = mode == DWRITE_MEASURING_MODE_NATURAL ? DWRITE_MEASURING_MODE_NATURAL : mode;
    if (FAILED(factory_->CreateGlyphRunAnalysis(run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, m, x, y,
                                                analysis.put())))
      return S_OK;  // a run that cannot be rasterised leaves a gap, not an error
    RECT r{};
    if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &r)) || r.right <= r.left || r.bottom <= r.top)
      return S_OK;  // whitespace
    const int rw = r.right - r.left, rh = r.bottom - r.top;
    std::vector<std::uint8_t> tex(static_cast<std::size_t>(rw) * rh * 3);
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &r, tex.data(), static_cast<UINT32>(tex.size()))))
      return S_OK;
    for (int j = 0; j < rh; ++j) {
      const int dy = r.top + j;
      if (dy < 0 || dy >= h_) continue;
      for (int i = 0; i < rw; ++i) {
        const int dx = r.left + i;
        if (dx < 0 || dx >= w_) continue;
        const std::uint8_t* t = &tex[(static_cast<std::size_t>(j) * rw + i) * 3];
        const int gray = (t[0] + t[1] + t[2] + 1) / 3;  // grayscale coverage from the three subpixel samples
        std::uint8_t& dst = alpha_[static_cast<std::size_t>(dy) * w_ + dx];
        dst = static_cast<std::uint8_t>(std::min(255, dst + gray));
      }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

 private:
  IDWriteFactory* factory_;
  std::uint8_t* alpha_;
  int w_, h_;
};

struct Factory {
  Com<IDWriteFactory> f;
  std::wstring family;  // the first installed family of the preference list
  bool ready() {
    if (f) return true;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.put_void())))) return false;
    // Segoe UI is the system UI font on every Windows 10/11; the rest are for editions and environments (Wine) without it.
    Com<IDWriteFontCollection> fonts;
    f->GetSystemFontCollection(fonts.put(), FALSE);
    family = L"Segoe UI";
    if (fonts) {
      for (const wchar_t* name : {L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans", L"Noto Sans", L"Liberation Sans", L"Verdana"}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) {
          family = name;
          break;
        }
      }
    }
    return true;
  }
};

}  // namespace

// Same conventions as the macOS and Linux shells: the bitmap is ceil(width)+2 by ceil(ascent)+ceil(descent)+2, the
// text is drawn one pixel in from the left with its baseline at ceil(ascent)+1 from the top, and the ascent is
// returned unrounded. The family is the system UI font; characters it lacks fall back through DirectWrite's system
// font fallback (CJK, Arabic, Hebrew, Indic, emoji).
bool rasterise_text(void*, const char* utf8, float px, int weight, dh_bitmap* out) {
  static Factory factory;
  if (!utf8 || !out || !(px > 0) || !factory.ready()) return false;
  const std::wstring text = widen(utf8);
  if (text.empty()) return false;

  Com<IDWriteTextFormat> format;
  if (FAILED(factory.f->CreateTextFormat(factory.family.c_str(), nullptr, weight != 0 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", format.put())))
    return false;
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  Com<IDWriteTextLayout> layout;
  if (FAILED(factory.f->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), 16384.0f, 4096.0f, layout.put())))
    return false;

  DWRITE_TEXT_METRICS tm{};
  if (FAILED(layout->GetMetrics(&tm))) return false;
  UINT32 line_count = 0;
  layout->GetLineMetrics(nullptr, 0, &line_count);
  std::vector<DWRITE_LINE_METRICS> lines(std::max<UINT32>(1, line_count));
  if (FAILED(layout->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &line_count)) || line_count == 0) return false;
  const double ascent = lines[0].baseline;
  const double height = lines[0].height;
  const double descent = std::fmax(0.0, height - ascent);
  const int w = static_cast<int>(std::ceil(tm.widthIncludingTrailingWhitespace)) + 2;
  const int h = static_cast<int>(std::ceil(ascent) + std::ceil(descent)) + 2;
  if (w <= 2 || h <= 2 || w > 4096 || h > 1024) return false;

  auto* memory = static_cast<std::uint8_t*>(std::calloc(static_cast<std::size_t>(w) * h, 1));
  if (!memory) return false;
  Collector collector(factory.f.get(), memory, w, h);
  if (FAILED(layout->Draw(nullptr, &collector, 1.0f, static_cast<FLOAT>(std::ceil(ascent) + 1.0 - ascent)))) {
    std::free(memory);
    return false;
  }
  out->w = static_cast<std::uint32_t>(w);
  out->h = static_cast<std::uint32_t>(h);
  out->ascent = static_cast<float>(ascent);
  out->alpha = memory;
  return true;
}

}  // namespace deckhand::shell
