#pragma once
// Free-form windows and modules (see docs/SKINS.md "Free-form windows"): window shapes and hit classification, snapping
// geometry, the default arrangement of module windows and its persistence, uniform scaling. Pure geometry, no rendering,
// no platform calls: the application and the tests use the same code. No function throws.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "deckhand/skin/skin.hpp"

namespace deckhand::skin {

// ---- shapes ---------------------------------------------------------------------------------------------
// Alpha mask of a window, either decoded from `mask=` (any resolution, mapped onto the window) or empty.
struct Mask {
  int w = 0, h = 0;
  std::vector<std::uint8_t> alpha;  // w * h
  bool empty() const { return w <= 0 || h <= 0 || alpha.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h); }
  // Alpha at normalised coordinates (0..1); 0 outside.
  int sample(float u, float v) const;
};
// Takes the alpha channel of a straight RGBA image. False (and an empty mask) when it exceeds the limits.
bool mask_from_rgba(const std::uint8_t* rgba, int w, int h, Mask& out, std::string& error);

// The window outline of the legacy shapes (polygon / ellipse / roundrect / chamfer), in points of a W x H window.
std::vector<Point> shape_outline(const Shape& sh, float W, float H, float scale = 1);
// Is (x, y) inside the region list (adds and subtractions applied in order)?
bool parts_contain(const std::vector<ShapePart>& parts, float x, float y);

// Everything needed to decide which pixels of a window are opaque, in the window's declared (base) points.
struct WindowShape {
  float base_w = 0, base_h = 0;
  int threshold = 128;
  Mask mask;                   // image mask, when the window has one
  std::vector<ShapePart> parts;  // vector regions (shape of kind Parts)
  std::vector<Point> outline;  // legacy polygon outline
  bool rectangular() const { return mask.empty() && parts.empty() && outline.size() < 3; }
  bool opaque(float x, float y) const;  // base points; false outside the window
};
// The shape of a window: the image mask (if given and valid) wins over `shape=`; neither = the whole rectangle.
WindowShape make_window_shape(const Skin& skin, const Window& w, Mask image_mask);

// ---- hit classification (what a click at a point does) -----------------------------------------------------
enum class Hit { None, Drag, Control, Resize, Solid };
// `over_control`: a widget that wants the pointer is there. Order: control, resize region, then (inside the shape) a drag
// region, or any opaque pixel when the window declares no drag regions. Opaque pixels outside the drag regions are Solid
// (they take the click but do nothing); everything outside the shape is None (clicks fall through to what is behind).
Hit classify_point(const WindowShape& shape, const Window& w, bool over_control, float x, float y);

// ---- size and scale -----------------------------------------------------------------------------------------
float clamp_user_scale(const Window& w, float s);                    // scale=fit: scale_min..scale_max
struct Size {
  float w = 0, h = 0;
};
Size window_size(const Window& w, float user_scale);                // points (base size x scale for scale=fit)
Size clamp_free_size(const Window& w, float width, float height);  // min / max / aspect for free resizing
// Uniform scale after dragging a corner (tl tr bl br) by (dx, dy) points from a window that was `start_w` wide at
// `start_scale`: the displacement along the diagonal decides.
float scale_from_corner_drag(const Window& w, float start_scale, const char* corner, float dx, float dy);

// ---- snapping and groups -------------------------------------------------------------------------------------
struct Frame {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
};
struct SnapTarget {
  Frame frame;
  std::vector<Point> points;  // absolute desktop points (the window's snap_points moved to its position)
  int id = -1;                // caller's window index
};
struct SnapResult {
  float dx = 0, dy = 0;  // add to the moving frame
  int attach = -1;       // target id the window is now docked to (edge contact or anchor), -1 = none
  bool snapped_x = false, snapped_y = false;
};
constexpr float kSnapThreshold = 12;  // points
// Snaps `moving` (and its own absolute anchor points) to the edges and anchor points of `others`. Edges snap when the
// windows touch (right to left, bottom to top: docking) or line up (left to left, ...) and their extents overlap within
// the threshold; two anchor points within the threshold in both axes snap exactly. The closest candidate per axis wins.
SnapResult snap_frame(const Frame& moving, const std::vector<Point>& moving_points, const std::vector<SnapTarget>& others,
                      float threshold = kSnapThreshold);
// Window indices that move with `root`: every window docked to it, transitively (`parent[i]` = index of the window i is
// docked to, or -1). The root itself is not listed.
std::vector<int> group_children(const std::vector<int>& parent, int root);

// ---- default arrangement and persistence -----------------------------------------------------------------------
// Top-left of a module of size (w, h) placed per `p` relative to `ref`.
Point place_relative(const Placement& p, const Frame& ref, float w, float h);

struct WinState {
  bool have_pos = false;
  float x = 0, y = 0;     // desktop points, top-left origin
  float scale = 1;        // user scale (scale=fit)
  float w = 0, h = 0;     // points, free-resizable windows (0 = default)
  bool visible = false;
  int z = 0;              // stacking order, higher = in front
  std::string parent;     // id of the window it is docked to, "" = none
};
std::string encode_win_state(const WinState& s);                  // "x=10 y=20 s=1.25 w=700 h=300 v=1 z=2 p=main"
bool decode_win_state(std::string_view text, WinState& out);       // false on anything malformed (out unchanged)
std::string win_setting_key(const std::string& skin_name, const std::string& window_id);  // "ui.win.<skin>.<id>"

// ---- curves (path sliders, arc meters) ---------------------------------------------------------------------------
// A path slider's value (0..1 along the curve) from a pointer position, and the handle's position for a value.
inline float path_value_at(const PathCurve& c, float x, float y) { return c.project(x, y); }
inline Point path_handle_at(const PathCurve& c, float value) { return c.at(value); }

}  // namespace deckhand::skin
