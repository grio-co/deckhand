#include "text.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace deckhand::shell {
namespace {

// One Cairo context for the lifetime of the process (the shell calls from the UI thread only).
struct Fonts {
  cairo_surface_t* surface = nullptr;
  cairo_t* cr = nullptr;
  PangoContext* context = nullptr;
  ~Fonts() {
    if (context) g_object_unref(context);
    if (cr) cairo_destroy(cr);
    if (surface) cairo_surface_destroy(surface);
  }
  bool ready() {
    if (context) return true;
    surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cr = cairo_create(surface);
    context = pango_cairo_create_context(cr);
    if (!context) return false;
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(context, options);
    cairo_font_options_destroy(options);
    return true;
  }
};

}  // namespace

// Same conventions as the macOS shell: the bitmap is ceil(width)+2 by ceil(ascent)+ceil(descent)+2, the
// text is drawn one pixel in from the left with its baseline at ceil(ascent)+1 from the top, and the
// ascent is returned unrounded. Characters the first font lacks fall back through Fontconfig.
bool rasterise_text(void*, const char* utf8, float px, int weight, dh_bitmap* out) {
  static Fonts fonts;
  if (!utf8 || !out || !(px > 0) || !fonts.ready()) return false;

  PangoFontDescription* desc = pango_font_description_new();
  pango_font_description_set_family(desc, "Inter, Noto Sans, Cantarell, DejaVu Sans, Sans");
  pango_font_description_set_weight(desc, weight != 0 ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(desc, static_cast<double>(px) * PANGO_SCALE);
  PangoLayout* layout = pango_layout_new(fonts.context);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_single_paragraph_mode(layout, TRUE);
  pango_layout_set_text(layout, utf8, -1);

  PangoRectangle logical;
  pango_layout_get_extents(layout, nullptr, &logical);
  const double ascent = static_cast<double>(pango_layout_get_baseline(layout)) / PANGO_SCALE;
  const double height = static_cast<double>(logical.height) / PANGO_SCALE;
  const double width = static_cast<double>(logical.width) / PANGO_SCALE;
  const double descent = std::fmax(0.0, height - ascent);
  const int w = static_cast<int>(std::ceil(width)) + 2;
  const int h = static_cast<int>(std::ceil(ascent) + std::ceil(descent)) + 2;
  if (w <= 2 || h <= 2 || w > 4096 || h > 1024) {
    g_object_unref(layout);
    return false;
  }

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_t* cr = cairo_create(surface);
  bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  auto* memory = static_cast<std::uint8_t*>(ok ? std::malloc(static_cast<std::size_t>(w) * h) : nullptr);
  if (memory) {
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, 1.0, std::ceil(ascent) + 1.0 - ascent);
    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout(cr, layout);
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const std::uint8_t* src = cairo_image_surface_get_data(surface);
    for (int y = 0; y < h; ++y) std::memcpy(memory + static_cast<std::size_t>(y) * w, src + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(w));
    out->w = static_cast<std::uint32_t>(w);
    out->h = static_cast<std::uint32_t>(h);
    out->ascent = static_cast<float>(ascent);
    out->alpha = memory;
  }
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  g_object_unref(layout);
  return memory != nullptr;
}

}  // namespace deckhand::shell
