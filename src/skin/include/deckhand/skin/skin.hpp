#pragma once
// Skins: parser, validator, in-memory model and layout solver for the declarative .dhskin format
// (see docs/SKINS.md). No rendering, no scripting, no file access beyond load_skin(), which confines
// every path to the skin's own directory. No public function throws.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "deckhand/i18n/i18n.hpp"

namespace deckhand::skin {

// Hard limits so a hostile skin cannot exhaust memory or time.
namespace limits {
constexpr size_t kMaxFileBytes = 1u << 20;     // skin.dhs
constexpr size_t kMaxAssetBytes = 8u << 20;    // each referenced file
constexpr int kMaxDepth = 24;                  // nesting of blocks
constexpr size_t kMaxStatements = 20000;       // raw statements parsed
constexpr size_t kMaxNodes = 4096;             // layout nodes after template expansion
constexpr size_t kMaxStringLen = 4096;
constexpr size_t kMaxTokens = 512;
constexpr size_t kMaxAtlases = 16;
constexpr size_t kMaxRegions = 1024;
constexpr size_t kMaxAnimations = 64;
constexpr size_t kMaxWindows = 4;
constexpr size_t kMaxShapePoints = 256;
constexpr size_t kMaxStrings = 2048;
constexpr int kMaxTemplateDepth = 8;
constexpr float kMaxWindowDim = 16384;
constexpr float kMinWindowDim = 64;
}  // namespace limits

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
struct Point {
  float x = 0, y = 0;
};
using Color = uint32_t;  // 0xRRGGBBAA

enum class State { Normal = 0, Hover, Pressed, On, Disabled };
constexpr int kStateCount = 5;
enum class TextAlign { Left, Center, Right };

enum class Kind {
  Row, Column, Stack, Free, Grid, Panel,                                  // containers
  Waveform, Jog, Button, Knob, Fader, Meter, Label, Browser, Tree, Pads, Image, Spacer,
  Unknown                                                                 // unrecognised widget (warning)
};
const char* kind_name(Kind k);
bool is_container(Kind k);
bool is_interactive(Kind k);

struct Message {
  enum class Severity { Warning, Error } severity = Severity::Warning;
  int line = 0;
  std::string text;
};

struct Manifest {
  std::string name, author, license, min_version, description;
  int format = 1;
};

struct FontToken {
  std::string family;  // empty = application default
  std::string file;    // optional, skin-relative
  float size = 12;
  bool bold = false, italic = false;
};

struct Region {
  std::string name;
  Rect src;
  bool nine_slice = false;
  std::array<float, 4> slice{};  // left, top, right, bottom insets
};
struct Atlas {
  std::string name, src;  // src is skin-relative
  std::vector<Region> regions;
};

enum class ShapeKind { Polygon, Ellipse, RoundRect, Chamfer };
struct Shape {
  std::string name;
  ShapeKind kind = ShapeKind::Polygon;
  std::vector<Point> points;  // polygon, normalised 0..1 of the window
  float amount = 0;           // corner radius / chamfer size in logical px
};

enum class Ease { Linear, In, Out, InOut };
enum class Loop { Once, Repeat, PingPong };
struct Animation {
  std::string name;
  std::string property;  // opacity, scale, offset_x, offset_y, rotate, glow
  float from = 0, to = 1, duration_ms = 1000, delay_ms = 0;
  Ease ease = Ease::InOut;
  Loop loop = Loop::Once;
  std::string trigger = "always";  // always, hover, pressed, on, beat
};
// Value of an animation at time t_ms since its trigger started.
float animation_value(const Animation& a, float t_ms);

struct Length {
  enum class Unit { Auto, Px, Percent, Flex } unit = Unit::Auto;
  float v = 0;
};

struct StyleSpec {  // unset fields inherit
  std::optional<Color> fg, bg, border, glow;
  std::optional<float> border_w, radius, opacity, scale, font_size, glow_r;
  std::optional<std::string> font, sprite;  // font token name; "atlas:region"
  std::optional<TextAlign> align;
};

struct Style {  // fully resolved, in physical pixels where lengths are involved
  Color fg = 0xE6E6EAFF, bg = 0, border = 0, glow = 0;
  float border_w = 0, radius = 0, opacity = 1, scale = 1, font_size = 12, glow_r = 0;
  std::string font;
  std::string sprite;
  TextAlign align = TextAlign::Left;
};

enum class Align { Stretch, Start, Center, End };
enum class Justify { Start, Center, End, Between };
enum class Anchor { Fill, TL, T, TR, L, C, R, BL, B, BR };

struct Node {
  Kind kind = Kind::Spacer;
  std::string type;  // keyword as written
  int line = 0;
  std::string id, bind, bind2, text, format, show, tooltip, shape;
  Length w, h, x, y;
  float min_w = 0, min_h = 0, max_w = 0, max_h = 0;  // 0 = unbounded
  std::array<float, 4> pad{}, margin{};              // left, top, right, bottom
  float gap = 0, aspect = 0, flex = 0, rotate = 0, transition_ms = 0;
  Align align = Align::Stretch;
  Justify justify = Justify::Start;
  std::optional<Anchor> anchor;
  int col = -1, row = -1, colspan = 1, rowspan = 1, cols = 0, rows = 0;
  std::vector<std::string> anim_names;
  std::vector<int> anims;  // indices into Skin::animations
  std::map<std::string, std::string> props;  // widget-specific (orientation, bipolar, count, ...)
  StyleSpec base;
  std::array<StyleSpec, kStateCount> state;  // overrides per state; [Normal] unused
  std::vector<int> children;                 // indices into Skin::nodes
};

struct Window {
  std::string id, title, shape;  // title may be "@i18n.id"
  float width = 1280, height = 720, min_w = 0, min_h = 0;  // logical points
  int root = -1;
};

struct Skin {
  Manifest manifest;
  std::map<std::string, Color> colors;
  std::map<std::string, float> numbers;
  std::map<std::string, FontToken> fonts;
  std::vector<Atlas> atlases;
  std::vector<Shape> shapes;
  std::vector<Animation> animations;
  std::map<std::string, std::map<std::string, std::string>> strings;  // lang ("" = default) -> id -> text
  std::vector<Node> nodes;
  std::vector<Window> windows;
  std::string root_dir;  // set by load_skin for directory skins

  const Window* find_window(std::string_view id) const;
  const Shape* find_shape(std::string_view name) const;
  const Atlas* find_atlas(std::string_view name) const;
};

struct ParseOptions {
  bool allow_files = true;  // false for single-file skins: atlases/fonts with files are rejected
};
struct ParseResult {
  std::shared_ptr<const Skin> skin;  // null when there is any error
  std::vector<Message> messages;
  bool ok() const { return skin != nullptr; }
  size_t error_count() const;
  size_t warning_count() const;
  std::string describe() const;  // all messages, one per line
};

// A .dhskin is a single text file (procedural skins) or a directory holding skin.dhs plus img/ and fonts/.
ParseResult parse_skin(std::string_view text, const ParseOptions& opt = {});
ParseResult load_skin(const std::string& path);

bool is_safe_relative_path(std::string_view p);  // no "..", no absolute, no drive/backslash/NUL
bool valid_binding(std::string_view name);       // deck[1].play, mixer.ch[2].eq.high, browser.search
// "@id" -> per-skin string for the catalog's language, else catalog.tr(id); anything else is literal.
// `args` fill {placeholders} in the message (see parse_text_args for the widget's args="k=v,k=v" property).
std::string resolve_text(const Skin& skin, const i18n::Catalog& catalog, std::string_view spec, const i18n::Args& args = {});
i18n::Args parse_text_args(std::string_view s);

// ---- layout solver ----------------------------------------------------------------------------------
struct ResolvedWidget {
  int node = -1;    // index into Skin::nodes
  int parent = -1;  // index into Layout::widgets, -1 for the window root
  bool parent_is_flow = false;  // parent is a row/column/grid (siblings must not overlap)
  Kind kind = Kind::Spacer;
  std::string type, id, bind, bind2, text, format, show, tooltip, shape;
  Rect rect;  // physical pixels, window-relative
  float rotation = 0, transition_ms = 0;
  std::map<std::string, std::string> props;
  std::array<Style, kStateCount> style;  // base merged with per-state overrides
  std::vector<int> animations;           // indices into Skin::animations
  bool container = false;
  bool drawable = true;     // false for plain containers with no fill/border/sprite
  bool interactive = false;
};

struct Layout {
  std::string window;
  float width = 0, height = 0, scale = 1;  // physical pixels
  std::vector<ResolvedWidget> widgets;      // paint order (parents before children)
  std::vector<Point> shape;                 // window outline in px; empty = full rectangle
  std::vector<std::string> warnings;
};

// width/height are the window's physical pixel size; scale is the backing scale (2 on Retina). Skin
// lengths are logical points and are multiplied by `scale`.
// `hidden_nodes` (optional, indexed by Skin::nodes) marks nodes whose `show=` condition is false: inside a row,
// column or grid they take no space and no gap (they are still emitted, with an empty rectangle, so the widget
// list keeps one entry per node). Anywhere else the flag is ignored.
Layout solve_layout(const Skin& skin, size_t window_index, float width, float height, float scale,
                    const std::vector<char>* hidden_nodes = nullptr);
// Topmost interactive widget at (x, y) in window pixels, or -1; honours the window shape.
int hit_test(const Layout& layout, float x, float y);

}  // namespace deckhand::skin
