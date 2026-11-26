// skin: parse/validate shipped skins, layout invariants at several sizes, hostile input rejection.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/gfx/png.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/skin/skin.hpp"
#include "deckhand/skin/windows.hpp"

using namespace deckhand::skin;
namespace fs = std::filesystem;

static std::string wrap(const std::string& body) {
  return "dhskin 1\nmeta name=\"t\"\n" + body;
}
static const char* kMinWindow = "window w size=400x300 {\n row { spacer\n spacer } }\n";

static bool rejects(const std::string& text, const char* needle) {
  ParseResult r = parse_skin(text);
  if (r.ok()) {
    std::fprintf(stderr, "expected rejection containing '%s'\n", needle);
    return false;
  }
  if (r.describe().find(needle) == std::string::npos) {
    std::fprintf(stderr, "expected '%s' in:\n%s", needle, r.describe().c_str());
    return false;
  }
  return true;
}

// The skin's grid edit tools (nodes shown while a deck is in edit mode) are hidden, as they are when no deck is edited, or all shown.
static std::vector<char> grid_edit_mask(const Skin& sk, bool editing) {
  std::vector<char> m(sk.nodes.size(), 0);
  for (size_t i = 0; i < sk.nodes.size(); ++i)
    if (!editing && sk.nodes[i].show.find("grid.edit") != std::string::npos) m[i] = 1;
  return m;
}

static void check_layout(const Skin& sk, const char* name, float W, float H, float scale, size_t win = 0, bool editing = false) {
  const std::vector<char> hidden = grid_edit_mask(sk, editing);
  Layout L = solve_layout(sk, win, W, H, scale, &hidden);
  CHECK(!L.widgets.empty());
  CHECK(L.width == W && L.height == H);
  const float eps = 0.01f;
  size_t interactive = 0;
  for (size_t i = 0; i < L.widgets.size(); ++i) {
    const auto& w = L.widgets[i];
    if (!(w.rect.x >= -eps && w.rect.y >= -eps && w.rect.right() <= W + eps && w.rect.bottom() <= H + eps)) {
      std::fprintf(stderr, "%s %gx%g: widget %zu (%s) outside window: %g %g %g %g\n", name, W, H, i, w.type.c_str(), w.rect.x, w.rect.y, w.rect.w, w.rect.h);
      CHECK(false);
    }
    if (w.parent >= 0 && !(w.rect.w == 0 && w.rect.h == 0)) {  // (widgets of a hidden subtree collapse to nothing)
      const auto& p = L.widgets[w.parent];
      if (!(w.rect.x >= p.rect.x - eps && w.rect.y >= p.rect.y - eps && w.rect.right() <= p.rect.right() + eps && w.rect.bottom() <= p.rect.bottom() + eps))
        std::fprintf(stderr, "%s %gx%g: %s '%s' (%g %g %g %g) leaves its parent %s (%g %g %g %g)\n", name, W, H, w.type.c_str(), w.bind.c_str(), w.rect.x, w.rect.y,
                     w.rect.w, w.rect.h, p.type.c_str(), p.rect.x, p.rect.y, p.rect.w, p.rect.h);
      CHECK(w.rect.x >= p.rect.x - eps && w.rect.y >= p.rect.y - eps && w.rect.right() <= p.rect.right() + eps && w.rect.bottom() <= p.rect.bottom() + eps);
    }
    if (w.interactive) {
      ++interactive;
      if (!(w.rect.w > 1 && w.rect.h > 1) && !(w.rect.w == 0 && w.rect.h == 0 && !editing)) {  // (tools of decks not being edited take no space)
        std::fprintf(stderr, "%s %gx%g: interactive %s '%s' collapsed\n", name, W, H, w.type.c_str(), w.bind.c_str());
        CHECK(false);
      }
    }
    for (const std::string* b : {&w.bind, &w.bind2})
      if (!b->empty()) CHECK(valid_binding(*b));
    // siblings in flow containers must not overlap
    if (w.parent_is_flow)
      for (size_t j = i + 1; j < L.widgets.size(); ++j) {
        const auto& o = L.widgets[j];
        if (o.parent != w.parent) continue;
        float ix = std::min(w.rect.right(), o.rect.right()) - std::max(w.rect.x, o.rect.x);
        float iy = std::min(w.rect.bottom(), o.rect.bottom()) - std::max(w.rect.y, o.rect.y);
        if (ix > 0.05f && iy > 0.05f) {
          std::fprintf(stderr, "%s %gx%g: siblings %zu and %zu overlap (%g x %g)\n", name, W, H, i, j, ix, iy);
          CHECK(false);
        }
      }
  }
  CHECK(interactive > 20);
}

// A PNG built by hand: stored (uncompressed) deflate blocks, so the decoder's other paths are covered without a
// compressor. `rows` are raw scanlines without the filter byte; filters[y] is the filter type to apply.
static std::uint32_t test_crc(const std::vector<std::uint8_t>& d) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::uint8_t b : d) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return ~c;
}
static std::vector<std::uint8_t> make_png(int w, int h, int ctype, const std::vector<std::uint8_t>& scan,
                                          const std::vector<std::uint8_t>& plte = {}, const std::vector<std::uint8_t>& trns = {}) {
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  const auto be = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
  };
  const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& body) {
    be(out, static_cast<std::uint32_t>(body.size()));
    std::vector<std::uint8_t> t(type, type + 4);
    t.insert(t.end(), body.begin(), body.end());
    out.insert(out.end(), t.begin(), t.end());
    be(out, test_crc(t));
  };
  std::vector<std::uint8_t> ihdr;
  be(ihdr, static_cast<std::uint32_t>(w));
  be(ihdr, static_cast<std::uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, static_cast<std::uint8_t>(ctype), 0, 0, 0});
  chunk("IHDR", ihdr);
  if (!plte.empty()) chunk("PLTE", plte);
  if (!trns.empty()) chunk("tRNS", trns);
  std::vector<std::uint8_t> z = {0x78, 0x01, 0x01};  // zlib header, one final stored block
  const auto len = static_cast<std::uint16_t>(scan.size());
  z.push_back(len & 0xFF);
  z.push_back(len >> 8);
  z.push_back(~len & 0xFF);
  z.push_back((~len >> 8) & 0xFF);
  z.insert(z.end(), scan.begin(), scan.end());
  z.insert(z.end(), {0, 0, 0, 0});  // Adler-32 (not checked)
  chunk("IDAT", z);
  chunk("IEND", {});
  return out;
}

int main() {
  // ---- PNG reader (src/gfx/png.cpp)
  {
    using deckhand::gfx::Image;
    using deckhand::gfx::decode_png;
    Image im;
    std::string err;
    // RGBA 2x2: row 0 unfiltered; row 1 uses the Sub filter (each byte adds the pixel to its left)
    auto rgba = make_png(2, 2, 6, {0, 10, 20, 30, 40, 50, 60, 70, 80, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    CHECK(decode_png(rgba.data(), rgba.size(), im, err));
    CHECK(im.width == 2 && im.height == 2 && im.rgba.size() == 16);
    CHECK(im.rgba[0] == 10 && im.rgba[3] == 40 && im.rgba[4] == 50 && im.rgba[7] == 80);
    CHECK(im.rgba[8] == 1 && im.rgba[12] == 2);  // second pixel = its byte (1) + the first pixel's (1)
    // RGB (opaque) and palette with tRNS
    auto rgb = make_png(1, 1, 2, {0, 1, 2, 3});
    CHECK(decode_png(rgb.data(), rgb.size(), im, err) && im.rgba[2] == 3 && im.rgba[3] == 255);
    auto pal = make_png(2, 1, 3, {0, 0, 1}, {255, 0, 0, 0, 255, 0}, {128});
    CHECK(decode_png(pal.data(), pal.size(), im, err));
    CHECK(im.rgba[0] == 255 && im.rgba[3] == 128 && im.rgba[5] == 255 && im.rgba[7] == 255);
    // damaged input is refused, never read out of bounds
    auto bad = rgba;
    bad[20] ^= 0xFF;  // IHDR body: CRC mismatch
    CHECK(!decode_png(bad.data(), bad.size(), im, err));
    CHECK(!decode_png(rgba.data(), rgba.size() / 2, im, err));
    CHECK(!decode_png(rgba.data(), 5, im, err));
    // the shipped atlas uses dynamic Huffman blocks (zlib level 9): it must decode to its declared size
    CHECK(deckhand::gfx::load_png_file("resources/skins/voidwave.dhskin/img/atlas.png", im, err));
    CHECK(im.width == 256 && im.height == 256);
    int opaque = 0;
    for (std::size_t i = 3; i < im.rgba.size(); i += 4) opaque += im.rgba[i] > 128;
    CHECK(opaque > 500 && opaque < 40000);
  }

  // ---- hidden widgets give back their space in flow containers
  {
    ParseResult r = parse_skin(wrap("window w size=400x100 {\n row gap=10 {\n label id=a w=100\n label id=b w=100 show=\"app.x\"\n label id=c w=100\n }\n}\n"));
    CHECK(r.ok());
    const Skin& sk = *r.skin;
    int b_node = -1;
    for (std::size_t i = 0; i < sk.nodes.size(); ++i)
      if (sk.nodes[i].id == "b") b_node = static_cast<int>(i);
    CHECK(b_node >= 0);
    const auto find = [](const Layout& l, const char* id) -> const ResolvedWidget* {
      for (const ResolvedWidget& w : l.widgets)
        if (w.id == id) return &w;
      return nullptr;
    };
    Layout shown = solve_layout(sk, 0, 400, 100, 1);
    CHECK(find(shown, "c")->rect.x == 220);  // 100 + 10 + 100 + 10
    std::vector<char> hidden(sk.nodes.size(), 0);
    hidden[static_cast<std::size_t>(b_node)] = 1;
    Layout gone = solve_layout(sk, 0, 400, 100, 1, &hidden);
    CHECK(find(gone, "c")->rect.x == 110);  // b and its gap are gone
    CHECK(find(gone, "b") && find(gone, "b")->rect.w == 0);
    CHECK(gone.widgets.size() == shown.widgets.size());  // same widgets in the same order
    for (std::size_t i = 0; i < gone.widgets.size(); ++i) CHECK(gone.widgets[i].node == shown.widgets[i].node);
  }

  // ---- shipped skins
  std::map<std::string, std::shared_ptr<const Skin>> skins;
  for (const char* n : {"default", "voidwave", "orbit"}) {
    ParseResult r = load_skin(std::string("resources/skins/") + n + ".dhskin");
    if (!r.ok() || r.warning_count()) std::fprintf(stderr, "%s", r.describe().c_str());
    CHECK(r.ok());
    CHECK(r.error_count() == 0 && r.warning_count() == 0);
    CHECK(!r.skin->manifest.name.empty() && !r.skin->windows.empty());
    skins[n] = r.skin;
  }
  CHECK(skins["voidwave"]->find_window("main") && !skins["voidwave"]->find_window("main")->shape.empty());
  CHECK(skins["default"]->find_window("main")->shape.empty());
  CHECK(!skins["voidwave"]->animations.empty());
  CHECK(skins["voidwave"]->find_atlas("art") && !skins["voidwave"]->root_dir.empty());  // a directory skin with an image

  struct Size { float w, h, s; };
  const Size sizes[] = {{1280, 720, 1}, {1920, 1080, 1}, {3024, 1964, 2}, {1024, 600, 1}};
  for (auto& [name, sk] : skins)
    for (auto& z : sizes) check_layout(*sk, name.c_str(), z.w, z.h, z.s);

  // the four-deck window of the default skin lays out cleanly too, and "main" stays first
  {
    const Skin& d = *skins["default"];
    CHECK(d.windows.size() == 3 && d.windows[0].id == "main" && d.windows[1].id == "four" && d.windows[2].id == "browser");
    CHECK(d.windows[1].variant && d.windows[2].role == "browser" && !d.windows[2].visible);
    {
      const Layout bl = solve_layout(d, 2, 980, 420, 1);
      bool tree = false, table = false;
      for (auto& w : bl.widgets) { tree |= w.kind == Kind::Tree; table |= w.kind == Kind::Browser; }
      CHECK(tree && table);  // the detached library hosts the playlist tree and the track table
    }
    for (auto& z : sizes) check_layout(d, "default/four", z.w, z.h, z.s, 1);
    // with the grid edit tools of every deck shown, the windows still lay out (no overlaps, nothing outside) at their own size
    check_layout(d, "default/main editing", 1280, 800, 1, 0, true);
    check_layout(d, "default/four editing", 1440, 900, 1, 1, true);
    // the bindings the application maps (src/app/skin_view.cpp map_binding) are all present
    std::map<std::string, int> main_binds, four_binds;
    for (auto& w : solve_layout(d, 0, 1280, 800, 1).widgets) {
      if (!w.bind.empty()) ++main_binds[w.bind];
      if (!w.show.empty()) ++main_binds[w.show[0] == '!' ? w.show.substr(1) : w.show];
    }
    for (auto& w : solve_layout(d, 1, 1440, 900, 1).widgets) if (!w.bind.empty()) ++four_binds[w.bind];
    for (const char* b : {"deck[1].key_lock", "deck[1].quantize", "deck[1].slip", "deck[2].reverse", "deck[1].tempo", "deck[1].tempo_range",
                          "deck[1].key_shift", "deck[1].key_shift.minus", "deck[1].key_shift.plus", "deck[2].key_shift.reset",
                          "deck[1].fx[1].select", "deck[1].fx[2].on", "deck[2].fx[3].wet", "deck[1].time", "deck[1].letter",
                          "mixer.master.volume", "mixer.headphone.volume", "mixer.headphone.mix", "mixer.sampler.level",
                          "app.rec", "app.export", "app.import", "app.language", "app.skin", "app.layout", "app.settings", "app.sampler",
                          "app.learn", "app.status", "app.controller", "app.audio", "app.has_import", "app.multi_language", "app.multi_skin",
                          "app.has_four", "browser.search", "browser.count", "browser.playlist.add_list", "browser.playlist.add_folder",
                          "browser.playlist.rename", "browser.playlist.delete", "browser.add_to", "browser.remove", "browser.can_remove",
                          "browser.edit", "deck[2].load", "deck[1].grid.edit", "deck[2].grid.edit", "deck[1].metronome", "deck[2].grid.shift_m1",
                          "deck[1].grid.first_beat", "deck[1].grid.downbeat", "deck[1].grid.bpm", "deck[2].grid.tap", "deck[2].grid.lock",
                          "deck[1].grid.undo", "deck[2].cue.memory_set", "deck[1].cue.loop_save", "deck[2].cue.loop_recall",
                          "mixer.metronome.level"})
      if (!main_binds.count(b)) { std::fprintf(stderr, "default main window lacks binding %s\n", b); CHECK(false); }
    for (const char* b : {"deck[3].play", "deck[4].pads", "mixer.ch[3].fader", "mixer.ch[4].eq.low", "deck[4].waveform", "mixer.ch[4].cue"})
      if (!four_binds.count(b)) { std::fprintf(stderr, "default four window lacks binding %s\n", b); CHECK(false); }
  }

  // default skin specifics
  {
    Layout L = solve_layout(*skins["default"], 0, 1280, 720, 1);
    std::map<std::string, int> binds;
    for (auto& w : L.widgets) if (!w.bind.empty()) ++binds[w.bind];
    for (const char* b : {"deck[1].play", "deck[2].play", "deck[1].bpm", "deck[2].bpm", "mixer.crossfader", "mixer.ch[1].eq.high",
                          "mixer.ch[2].fader", "deck[1].waveform", "browser.tracks", "browser.tree", "deck[1].pads", "mixer.ch[2].cue"})
      CHECK(binds.count(b) == 1);
    // two decks left and right of the mixer
    const ResolvedWidget *p1 = nullptr, *p2 = nullptr, *xf = nullptr, *br = nullptr;
    for (auto& w : L.widgets) {
      if (w.bind == "deck[1].play") p1 = &w;
      if (w.bind == "deck[2].play") p2 = &w;
      if (w.bind == "mixer.crossfader") xf = &w;
      if (w.bind == "browser.tracks") br = &w;
    }
    CHECK(p1 && p2 && xf && br);
    CHECK(p1->rect.x < xf->rect.x && xf->rect.x < p2->rect.x);
    CHECK(br->rect.y > xf->rect.bottom());
    // scale 2 at double the pixels gives the same geometry doubled
    Layout L2 = solve_layout(*skins["default"], 0, 2560, 1440, 2);
    CHECK(L2.widgets.size() == L.widgets.size());
    for (size_t i = 0; i < L.widgets.size(); ++i) {
      CHECK(std::fabs(L2.widgets[i].rect.x - 2 * L.widgets[i].rect.x) < 0.05f);
      CHECK(std::fabs(L2.widgets[i].rect.w - 2 * L.widgets[i].rect.w) < 0.05f);
    }
    // states merged: "on" play button has a filled background, normal does not
    for (auto& w : L.widgets)
      if (w.bind == "deck[1].play") {
        CHECK(w.style[static_cast<int>(State::On)].bg != w.style[static_cast<int>(State::Normal)].bg);
        CHECK(w.text == "@deck.play" && w.props.at("text_on") == "@deck.pause");
      }
    CHECK(hit_test(L, p1->rect.x + 2, p1->rect.y + 2) >= 0);
    CHECK(hit_test(L, -5, 5) == -1);
  }
  // voidwave window shape clips hit testing
  {
    Layout L = solve_layout(*skins["voidwave"], 0, 1100, 620, 1);
    CHECK(L.shape.size() == 6);
    CHECK(hit_test(L, 1, 1) == -1);
    CHECK(hit_test(L, 1099, 619) == -1);
    CHECK(L.shape[2].x == 1100 && L.shape[2].y == 310);
  }

  // ---- text via i18n
  {
    deckhand::i18n::Catalog cat;
    CHECK(cat.load_builtin("resources/lang/en.dhlang"));
    cat.load_directory("resources/lang");
    const Skin& sk = *skins["default"];
    CHECK(resolve_text(sk, cat, "@deck.sync") == "SYNC");
    CHECK(resolve_text(sk, cat, "@skin.credit") == "Default skin by the DeckHand contributors");
    CHECK(resolve_text(sk, cat, "literal") == "literal");
    CHECK(resolve_text(sk, cat, "@browser.load", parse_text_args("deck=A")) == "Load A");
    CHECK(cat.set_language("de"));
    CHECK(resolve_text(sk, cat, "@browser.load", parse_text_args("deck=B")) == "B laden");
    // every @id used by either skin exists in English
    for (auto& [n, s] : skins)
      for (const auto& node : s->nodes)
        for (const std::string* t : {&node.text, &node.tooltip, (node.props.count("text_on") ? &node.props.at("text_on") : &node.text)})
          if (!t->empty() && (*t)[0] == '@' && !(s->strings.count("") && s->strings.at("").count(t->substr(1)))) CHECK(cat.has(t->substr(1)));
  }

  // ---- animation maths
  {
    Animation a;
    a.from = 0; a.to = 10; a.duration_ms = 100; a.ease = Ease::Linear; a.loop = Loop::PingPong;
    CHECK(animation_value(a, 0) == 0 && animation_value(a, 50) == 5 && animation_value(a, 100) == 10);
    CHECK(std::fabs(animation_value(a, 150) - 5) < 1e-4f && std::fabs(animation_value(a, 200)) < 1e-3f);
    a.loop = Loop::Once;
    CHECK(animation_value(a, 1000) == 10);
  }

  // ---- helpers
  CHECK(valid_binding("deck[1].play") && valid_binding("mixer.ch[2].eq.high") && valid_binding("browser.search"));
  CHECK(!valid_binding("") && !valid_binding("Deck.play") && !valid_binding("deck[].play") && !valid_binding("deck[1]x") &&
        !valid_binding("a..b") && !valid_binding(".a") && !valid_binding("a.") && !valid_binding("a b") && !valid_binding("deck[1234].x"));
  CHECK(is_safe_relative_path("img/a.png"));
  CHECK(!is_safe_relative_path("../a.png") && !is_safe_relative_path("img/../../a") && !is_safe_relative_path("/etc/passwd") &&
        !is_safe_relative_path("a\\b") && !is_safe_relative_path("C:/x") && !is_safe_relative_path("") && !is_safe_relative_path("a//b") &&
        !is_safe_relative_path("~/x"));

  // ---- hostile / malformed input
  CHECK(rejects("meta name=\"x\"\n", "must start with 'dhskin 1'"));
  CHECK(rejects("dhskin 2\n", "unsupported skin format"));
  CHECK(rejects(wrap("window w size=400x300 {\n row {\n"), "missing '}'"));
  CHECK(rejects(wrap("}\n"), "unmatched '}'"));
  CHECK(rejects(wrap("window w size=400x300 {\n label text=\"abc\n}\n"), "unterminated string"));
  CHECK(rejects(wrap("atlas a src=\"../../etc/passwd\" {\n}\n") + kMinWindow, "unsafe"));
  CHECK(rejects(wrap("atlas a src=\"/etc/passwd\" {\n}\n") + kMinWindow, "unsafe"));
  CHECK(rejects(wrap("atlas a src=\"img\\\\x.png\" {\n}\n") + kMinWindow, "unsafe"));
  CHECK(rejects(wrap("tokens {\n font f file=\"../x.ttf\"\n}\n") + kMinWindow, "unsafe font path"));
  {
    ParseOptions o; o.allow_files = false;
    ParseResult r = parse_skin(wrap("atlas a src=\"img/a.png\" {\n}\n") + kMinWindow, o);
    CHECK(!r.ok() && r.describe().find("single-file") != std::string::npos);
  }
  CHECK(rejects(wrap("window w size=99999x300 {\n row {\n }\n}\n"), "bad window size"));
  CHECK(rejects(wrap("window w size=400x300 {\n row {\n }\n}\nwindow w size=400x300 {\n row {\n }\n}\n"), "duplicate window"));
  CHECK(rejects(wrap(""), "no window"));
  CHECK(rejects("dhskin 1\n" + std::string(kMinWindow), "manifest lacks a name"));
  CHECK(rejects(wrap("window w size=400x300 {\n label bind=\"Deck.Play\"\n}\n"), "malformed bind"));
  CHECK(rejects(wrap("window w size=400x300 {\n label color=nosuchtoken\n}\n"), "bad value"));
  CHECK(rejects(wrap("window w size=400x300 {\n label font=nofont\n}\n"), "unknown font token"));
  CHECK(rejects(wrap("window w size=400x300 {\n label anim=ghost\n}\n"), "unknown animation"));
  CHECK(rejects(wrap("window w size=400x300 {\n label w=abc\n}\n"), "bad value"));
  CHECK(rejects(wrap("window w size=400x300 shape=none {\n spacer\n}\n"), "unknown shape"));
  CHECK(rejects(wrap("window w size=400x300 {\n spacer\n spacer\n}\n"), "exactly one root"));
  CHECK(rejects(wrap("animation a property=teleport duration=10\n") + kMinWindow, "unknown animation property"));
  CHECK(rejects(wrap("animation a property=opacity duration=99999999\n") + kMinWindow, "bad duration"));
  CHECK(rejects(wrap("shape s {\n polygon 0,0 1,1\n}\n") + kMinWindow, "polygon needs"));
  // oversize
  {
    std::string big = wrap("") + std::string(limits::kMaxFileBytes + 10, ' ');
    CHECK(rejects(big, "larger than"));
  }
  // depth bomb
  {
    std::string t = wrap("window w size=400x300 {\n");
    for (int i = 0; i < 40; ++i) t += "row {\n";
    for (int i = 0; i < 40; ++i) t += "}\n";
    t += "}\n";
    CHECK(rejects(t, "nested too deeply"));
  }
  // node-count bomb (flat)
  {
    std::string t = wrap("window w size=400x300 {\n row {\n");
    for (size_t i = 0; i < limits::kMaxNodes + 5; ++i) t += "spacer\n";
    t += "}\n}\n";
    CHECK(rejects(t, "too many layout nodes"));
  }
  // template cycles, unknown templates, expansion bomb, bad params
  CHECK(rejects(wrap("template a {\n use b\n}\ntemplate b {\n use a\n}\nwindow w size=400x300 {\n row {\n use a\n }\n}\n"), "template cycle"));
  CHECK(rejects(wrap("template a {\n use a\n}\nwindow w size=400x300 {\n row {\n use a\n }\n}\n"), "template cycle"));
  CHECK(rejects(wrap("window w size=400x300 {\n row {\n use nothing\n }\n}\n"), "unknown template"));
  CHECK(rejects(wrap("template a n {\n spacer\n}\nwindow w size=400x300 {\n row {\n use a\n }\n}\n"), "needs parameter"));
  CHECK(rejects(wrap("template a n {\n spacer\n}\nwindow w size=400x300 {\n row {\n use a n=1 m=2\n }\n}\n"), "no parameter"));
  CHECK(rejects(wrap("template a {\n label text=\"${zz}\"\n}\nwindow w size=400x300 {\n row {\n use a\n }\n}\n"), "unresolved template parameter"));
  {
    std::string t = wrap("template l5 {\n spacer\n}\n");
    std::string prev = "l5";
    for (int i = 4; i >= 0; --i) {
      std::string nm = "l" + std::to_string(i);
      t += "template " + nm + " {\n";
      for (int k = 0; k < 12; ++k) t += " use " + prev + "\n";
      t += "}\n";
      prev = nm;
    }
    t += "window w size=400x300 {\n row {\n use l0\n }\n}\n";
    ParseResult r = parse_skin(t);
    CHECK(!r.ok());  // 12^5 expansion refused (too large or too many nodes)
  }
  // template depth limit (long non-cyclic chain)
  {
    std::string t = wrap("template t0 {\n spacer\n}\n");
    for (int i = 1; i < 12; ++i) t += "template t" + std::to_string(i) + " {\n use t" + std::to_string(i - 1) + "\n}\n";
    t += "window w size=400x300 {\n row {\n use t11\n }\n}\n";
    CHECK(rejects(t, "nested too deeply"));
  }

  // ---- forward compatibility: unknown widgets/properties are warnings, not errors
  {
    ParseResult r = parse_skin(wrap("hologram x=1\nwindow w size=400x300 {\n row {\n  quantum_dial bind=\"deck[1].x\" {\n   spacer\n  }\n  label sparkle=high text=\"hi\"\n  spacer\n }\n}\n"));
    CHECK(r.ok());
    CHECK(r.error_count() == 0 && r.warning_count() == 3);
    Layout L = solve_layout(*r.skin, 0, 400, 300, 1);
    CHECK(L.widgets.size() == 4);  // row, unknown, label, spacer
    bool saw_unknown = false;
    for (auto& w : L.widgets) saw_unknown |= w.kind == Kind::Unknown;
    CHECK(saw_unknown);
  }

  // ---- solver behaviour on a small hand-made skin
  {
    ParseResult r = parse_skin(wrap(
        "window w size=400x300 {\n"
        " column pad=10 gap=10 {\n"
        "  row h=50 gap=10 justify=between {\n   spacer w=40\n   spacer w=40\n   spacer w=40\n  }\n"
        "  row flex=1 gap=0 {\n   spacer w=25%\n   spacer flex=1\n   spacer flex=3 max_w=100\n  }\n"
        "  row h=40 {\n   spacer w=900\n   spacer w=900\n  }\n"
        "  free h=60 {\n   spacer w=20 h=20 anchor=br x=5 y=5\n   spacer w=50% h=50% anchor=c\n  }\n"
        "  grid cols=2 rows=2 gap=4 h=60 {\n   spacer\n   spacer\n   spacer colspan=2\n  }\n"
        " }\n}\n"));
    CHECK(r.ok());
    Layout L = solve_layout(*r.skin, 0, 400, 300, 1);
    auto at = [&](size_t i) { return L.widgets[i].rect; };
    // index: 0 column, 1 row, 2-4 spacers, 5 row, 6-8, 9 row, 10-11, 12 free, 13-14, 15 grid, 16-18
    CHECK(L.widgets.size() == 19);
    CHECK(at(1).h == 50 && at(1).x == 10);
    CHECK(at(2).x == 10 && at(4).right() == 390);          // justify=between spans the row
    CHECK(std::fabs(at(3).x - 180) < 0.01f);                // centred middle item
    CHECK(std::fabs(at(6).w - 95) < 0.01f);                 // 25% of 380
    CHECK(at(8).w == 100 && std::fabs(at(7).w - (380 - 95 - 100)) < 0.01f);  // max_w frozen, remainder to the other flex
    CHECK(at(10).w + at(11).w <= 380.01f && at(10).w == at(11).w);  // oversized fixed items shrink to fit
    CHECK(at(13).right() == at(12).right() - 5 && at(13).bottom() == at(12).bottom() - 5);  // anchor=br with offsets
    CHECK(std::fabs(at(14).x + at(14).w / 2 - (at(12).x + at(12).w / 2)) < 0.01f);
    CHECK(at(16).y == at(17).y && at(18).y > at(16).y && std::fabs(at(18).w - at(15).w) < 0.01f);  // colspan row
    CHECK(at(0).bottom() <= 300 + 0.01f);
  }
  // aspect + percent + stack
  {
    ParseResult r = parse_skin(wrap("window w size=400x300 {\n row {\n  panel aspect=1 {\n   spacer\n  }\n }\n}\n"));
    CHECK(r.ok());
    Layout L = solve_layout(*r.skin, 0, 400, 300, 1);
    CHECK(L.widgets[1].rect.w == 300 && L.widgets[1].rect.h == 300 && L.widgets[1].rect.x == 50);
    CHECK(solve_layout(*r.skin, 5, 100, 100, 1).widgets.empty());  // bad window index is safe
  }


  // ================= free-form windows, modules, snapping, curves =================
  // ---- the shipped Orbit skin declares them
  {
    const Skin& o = *skins["orbit"];
    CHECK(o.windows.size() == 3);
    const Window* m = o.find_window("main");
    const Window* b = o.find_window("browser");
    const Window* p = o.find_window("sampler");
    CHECK(m && b && p && m->role == "main" && b->role == "browser" && p->role == "sampler");
    CHECK(m->chrome == Chrome::None && m->transparent && m->scale_fit && m->snap && m->visible);
    CHECK(m->mask == "img/main.png" && m->mask_threshold == 128 && m->scale_min == 0.5f && m->scale_max == 2.0f && m->aspect_lock);
    CHECK(!m->resize.empty() && m->snap_points.size() == 2);
    CHECK(b->place.set && b->place.ref == "main" && b->place.side == Placement::Side::Below && b->place.dx == 120 && b->place.dy == -12);
    CHECK(b->visible && p->floating && p->drag_mode == DragMode::Regions && p->drag.size() == 2 && p->drag[1].sub);
    const Shape* cap = o.find_shape("capsule_shape");
    CHECK(cap && cap->kind == ShapeKind::Parts && cap->parts.size() == 1);
    // curved controls and window actions
    int paths = 0, arcs = 0, actions = 0;
    for (const Node& n : o.nodes) {
      paths += n.kind == Kind::PathFader;
      arcs += n.kind == Kind::Arc;
      actions += n.props.count("action") ? 1 : 0;
    }
    CHECK(paths == 2 && arcs == 2 && actions >= 4);
    // everything the module windows declare stays inside them
    check_layout(o, "orbit/main", o.windows[0].width, o.windows[0].height, 1, 0);
    for (size_t w = 1; w < o.windows.size(); ++w) {  // the modules lay out inside themselves too, at 1x and at a scaled-up size
      for (const float k : {1.0f, 1.5f}) {
        const Layout L = solve_layout(o, w, o.windows[w].width * k, o.windows[w].height * k, 1);
        CHECK(!L.widgets.empty());
        for (const auto& rw : L.widgets) CHECK(rw.rect.x >= -0.01f && rw.rect.y >= -0.01f && rw.rect.right() <= L.width + 0.01f && rw.rect.bottom() <= L.height + 0.01f);
      }
    }
  }

  // ---- shape hit tests: image mask (with interior holes), vector regions, drag / control / resize classification
  {
    const Skin& o = *skins["orbit"];
    const Window& m = *o.find_window("main");
    // the real mask: alpha of img/main.png
    deckhand::gfx::Image img;
    std::string why;
    CHECK(deckhand::gfx::load_png_file("resources/skins/orbit.dhskin/img/main.png", img, why));
    Mask mask;
    CHECK(mask_from_rgba(img.rgba.data(), img.width, img.height, mask, why) && !mask.empty());
    WindowShape shape = make_window_shape(o, m, mask);
    CHECK(!shape.rectangular());
    CHECK(shape.opaque(255, 280));    // deck A centre
    CHECK(shape.opaque(745, 280));    // deck B centre
    CHECK(shape.opaque(500, 280));    // the waist (mixer)
    CHECK(!shape.opaque(5, 5));       // corner: transparent
    CHECK(!shape.opaque(500, 60));    // notch between the discs, above the waist
    CHECK(!shape.opaque(500, 165));   // porthole: a hole inside the shape
    CHECK(!shape.opaque(500, 395));
    CHECK(shape.opaque(500, 140));    // rim above the porthole is solid again
    CHECK(!shape.opaque(-1, 10) && !shape.opaque(10, 600) && !shape.opaque(1000, 280));
    // classification: control > resize grip > drag; transparent = falls through, even over nothing
    CHECK(classify_point(shape, m, false, 255, 90) == Hit::Drag);       // bare art: drags the window
    CHECK(classify_point(shape, m, true, 255, 280) == Hit::Control);
    CHECK(classify_point(shape, m, false, 880, 455) == Hit::Resize);    // the grip
    CHECK(classify_point(shape, m, false, 5, 5) == Hit::None);          // desktop shows through, click falls through
    CHECK(classify_point(shape, m, false, 500, 165) == Hit::None);      // through the porthole too
    CHECK(classify_point(shape, m, true, 5, 5) == Hit::Control);        // a control overhanging the art still works
    // vector regions with a drag list: ring drags, the middle swallows the click
    const Window& pod = *o.find_window("sampler");
    WindowShape ps = make_window_shape(o, pod, Mask{});
    CHECK(!ps.rectangular() && ps.opaque(150, 150) && !ps.opaque(2, 2) && !ps.opaque(299, 299));
    CHECK(classify_point(ps, pod, false, 150, 20) == Hit::Drag);
    CHECK(classify_point(ps, pod, false, 150, 150) == Hit::Solid);
    CHECK(classify_point(ps, pod, false, 2, 2) == Hit::None);
    const Window& cb = *o.find_window("browser");
    WindowShape cs = make_window_shape(o, cb, Mask{});
    CHECK(cs.opaque(380, 150) && cs.opaque(10, 150) && !cs.opaque(2, 2) && !cs.opaque(380, 305));  // stadium: round ends
    // a threshold decides what counts as opaque in an image mask
    Mask soft;
    soft.w = 2; soft.h = 1; soft.alpha = {40, 200};
    Window mw = m; mw.width = 2; mw.height = 1; mw.mask_threshold = 128;
    WindowShape ss = make_window_shape(o, mw, soft);
    CHECK(!ss.opaque(0.5f, 0.5f) && ss.opaque(1.5f, 0.5f));
    // vector region lists: add, subtract, order, polygon and path
    std::vector<ShapePart> parts(3);
    parts[0].type = ShapePart::Type::Rect; parts[0].box = {0, 0, 100, 100};
    parts[1].type = ShapePart::Type::Ellipse; parts[1].box = {40, 40, 20, 20}; parts[1].sub = true;
    parts[2].type = ShapePart::Type::Polygon; parts[2].points = {{200, 0}, {220, 0}, {210, 20}};
    CHECK(parts_contain(parts, 10, 10) && !parts_contain(parts, 50, 50) && parts_contain(parts, 210, 5) && !parts_contain(parts, 150, 50));
    parts[0].radius = 30;
    CHECK(!parts_contain(parts, 1, 1) && parts_contain(parts, 30, 2));  // rounded corner
  }

  // ---- window / region syntax: valid forms and what is rejected
  {
    const std::string win = "window w size=400x300 {\n row { spacer\n spacer } }\n";
    ParseResult ok = parse_skin(wrap("shape s {\n rect x=0 y=0 w=100 h=100 radius=10\n ellipse cx=50 cy=50 rx=20 ry=20 op=sub\n path d=\"M 0 0 L 90 0 Q 100 50 90 90 C 60 100 20 100 0 90 Z\"\n}\n"
                                     "window a size=400x300 role=main chrome=none background=transparent shape=s snap=true scale=fit scale_min=50 scale_max=300 scale_default=120 max=800x600 shadow=none floating=true {\n"
                                     " drag {\n rect x=0 y=0 w=400 h=40\n }\n resize handle=br size=20\n snap_point 10,20\n row { spacer\n spacer } }\n"
                                     "window b role=other size=200x100 place=\"a below 5 -6\" visible=true { row { spacer } }\n"));
    if (!ok.ok()) std::fprintf(stderr, "%s", ok.describe().c_str());
    CHECK(ok.ok());
    const Window& a = *ok.skin->find_window("a");
    CHECK(a.chrome == Chrome::None && a.transparent && a.floating && !a.shadow && a.scale_fit && a.scale_default == 1.2f && a.max_w == 800);
    CHECK(a.drag_mode == DragMode::Regions && a.resize_handle == "br" && a.resize_size == 20 && a.snap_points.size() == 1);
    CHECK(ok.skin->find_shape("s")->kind == ShapeKind::Parts && ok.skin->find_shape("s")->parts.size() == 3 && ok.skin->find_shape("s")->parts[2].points.size() > 10);
    const Window& b = *ok.skin->find_window("b");
    CHECK(b.place.set && b.place.ref == "a" && b.place.side == Placement::Side::Below && b.place.dy == -6 && b.visible);
    // old skins are unchanged: classic windows are rectangular, opaque, system chrome, not scaled
    ParseResult plain = parse_skin(wrap(win));
    CHECK(plain.ok());
    const Window& pw = plain.skin->windows[0];
    CHECK(pw.chrome == Chrome::System && !pw.transparent && !pw.scale_fit && pw.drag_mode == DragMode::Auto && pw.role == "w" && make_window_shape(*plain.skin, pw, Mask{}).rectangular());
    // rejected: bad values, hostile counts, paths outside the skin
    CHECK(rejects(wrap("window w size=400x300 chrome=glass { row { spacer } }\n"), "bad chrome"));
    CHECK(rejects(wrap("window w size=400x300 background=glass { row { spacer } }\n"), "bad background"));
    CHECK(rejects(wrap("window w size=400x300 role=Bad-Role { row { spacer } }\n"), "bad role"));
    CHECK(rejects(wrap("window w size=400x300 mask=\"../x.png\" { row { spacer } }\n"), "unsafe mask path"));
    CHECK(rejects(wrap("window w size=400x300 mask=\"/etc/passwd\" { row { spacer } }\n"), "unsafe mask path"));
    {
      ParseOptions single;
      single.allow_files = false;
      ParseResult r = parse_skin(wrap("window w size=400x300 mask=\"img/x.png\" { row { spacer } }\n"), single);
      CHECK(!r.ok() && r.describe().find("single-file skins cannot reference files") != std::string::npos);
    }
    CHECK(rejects(wrap("window w size=400x300 mask_threshold=0 { row { spacer } }\n"), "mask_threshold"));
    CHECK(rejects(wrap("window w size=400x300 scale_min=200 scale_max=100 { row { spacer } }\n"), "inconsistent scale"));
    CHECK(rejects(wrap("window w size=400x300 place=\"main sideways\" { row { spacer } }\n"), "bad place"));
    CHECK(rejects(wrap("window a size=400x300 { row { spacer } }\nwindow b role=a size=400x300 { row { spacer } }\n"), "share the role"));
    CHECK(rejects(wrap("window w size=400x300 { drag { spacer } row { spacer } }\n"), "unknown region primitive"));
    CHECK(rejects(wrap("window w size=400x300 { resize { rect x=0 y=0 w=1 h=1 } snap_point x row { spacer } }\n"), "snap_point needs"));
    CHECK(rejects(wrap("window w size=400x300 { resize\n row { spacer } }\n"), "resize needs"));
    CHECK(rejects(wrap("window w size=400x300 { resize handle=middle\n row { spacer } }\n"), "bad resize handle"));
    CHECK(rejects(wrap("shape s {\n path d=\"M 0 0 L 10 10 relative\"\n ellipse cx=1 cy=1 rx=1 ry=1\n}\n" + win), "path"));
    CHECK(rejects(wrap("shape s {\n rect x=0 y=0 w=-5 h=10\n ellipse cx=1 cy=1 rx=1 ry=1\n}\n" + win), "bad value"));
    CHECK(rejects(wrap("window w size=400x300 { row { button action=\"window.explode:x\" } }\n"), "bad action"));
    CHECK(rejects(wrap("window w size=400x300 { row { button action=\"window.toggle:Bad Role\" } }\n"), "bad action"));
    CHECK(rejects(wrap("window w size=400x300 { row { path d=\"nonsense\" } }\n"), "path"));
    CHECK(rejects(wrap("window w size=400x300 { row { path } }\n"), "needs d="));
    CHECK(parse_skin(wrap("window w size=400x300 { row { button action=\"window.toggle:browser\"\n button action=\"window.reset\" } }\n")).ok());
    {  // absurd counts: too many windows, too many region primitives, too many snap points
      std::string many = wrap("");
      for (int i = 0; i < 13; ++i) many += "window w" + std::to_string(i) + " size=100x100 { row { spacer } }\n";
      CHECK(rejects(many, "too many windows"));
      std::string shape = "shape s {\n";
      for (int i = 0; i < 70; ++i) shape += " rect x=0 y=0 w=1 h=1\n";
      CHECK(rejects(wrap(shape + "}\n" + win), "too many region primitives"));
      std::string snaps = "window w size=400x300 {\n";
      for (int i = 0; i < 40; ++i) snaps += " snap_point 1,1\n";
      CHECK(rejects(wrap(snaps + " row { spacer } }\n"), "too many snap points"));
      std::string longpath = "M 0 0";
      for (int i = 0; i < 2000; ++i) longpath += " L 1 1";
      CHECK(rejects(wrap("window w size=400x300 { row { path d=\"" + longpath + "\" } }\n"), "too long"));
      std::string curvy = "M 0 0";
      for (int i = 0; i < 300; ++i) curvy += " Q 1 1 2 2";  // under the string limit, over the point limit once flattened
      CHECK(curvy.size() < 4000 && rejects(wrap("window w size=400x300 { row { path d=\"" + curvy + "\" } }\n"), "path"));
    }
    // a hostile mask: oversized images and empty data are refused, never allocated
    Mask big;
    std::string err;
    std::vector<std::uint8_t> px(16, 255);
    CHECK(!mask_from_rgba(px.data(), 100000, 100000, big, err) && big.empty());
    CHECK(!mask_from_rgba(px.data(), 5000, 5000, big, err));
    CHECK(!mask_from_rgba(nullptr, 2, 2, big, err) && !mask_from_rgba(px.data(), 0, 2, big, err));
    CHECK(mask_from_rgba(px.data(), 2, 2, big, err) && big.sample(0.1f, 0.1f) == 255 && big.sample(1.5f, 0.1f) == 0 && big.sample(-1, 0) == 0);
  }

  // ---- snapping geometry and groups
  {
    using namespace deckhand::skin;
    std::vector<SnapTarget> others;
    SnapTarget main_t;
    main_t.frame = {100, 100, 400, 300};
    main_t.id = 0;
    main_t.points = {{300, 400}};
    others.push_back(main_t);
    // right edge near the main window's left edge: docks to it
    SnapResult r = snap_frame({-295, 120, 400, 150}, {}, others);
    CHECK(r.snapped_x);
    CHECK(std::fabs(r.dx - (100 - 105)) < 1e-4f && r.attach == 0);
    // top edge near the main window's bottom edge: docks below, x aligned by the left-left candidate
    r = snap_frame({105, 407, 200, 100}, {}, others);
    CHECK(r.snapped_y && std::fabs(r.dy - (-7)) < 1e-4f && r.attach == 0 && r.snapped_x && std::fabs(r.dx - (-5)) < 1e-4f);
    // far away: nothing
    r = snap_frame({800, 800, 100, 100}, {}, others);
    CHECK(!r.snapped_x && !r.snapped_y && r.attach == -1 && r.dx == 0 && r.dy == 0);
    // lining up edges beside a window without touching it does not dock (no attach), but still aligns
    r = snap_frame({120, 96, 100, 100}, {}, others);  // on top of main, 4 points above its top edge: tops line up
    CHECK(r.snapped_y && std::fabs(r.dy - 4) < 1e-4f && r.attach == -1 && !r.snapped_x);
    r = snap_frame({600, 104, 100, 100}, {}, others);  // 100 points beside it: too far apart to care
    CHECK(!r.snapped_x && !r.snapped_y);
    // anchor points: both axes at once, exact, and they dock
    r = snap_frame({290, 450, 80, 80}, {{298, 452}}, others);  // the anchor is 52 points below the target point: too far
    CHECK(!(r.snapped_x && r.snapped_y && r.attach == 0 && std::fabs(298 + r.dx - 300) < 1e-4f && std::fabs(452 + r.dy - 400) < 1e-4f));
    // a point 4 px off in each axis snaps exactly
    r = snap_frame({-1000, -1000, 100, 100}, {{296, 404}}, others);
    CHECK(r.snapped_x && r.snapped_y && std::fabs(296 + r.dx - 300) < 1e-4f && std::fabs(404 + r.dy - 400) < 1e-4f && r.attach == 0);
    // beyond the threshold: no snapping
    r = snap_frame({-1000, -1000, 100, 100}, {{300 - kSnapThreshold - 3, 400 + kSnapThreshold + 3}}, others);
    CHECK(!r.snapped_x && !r.snapped_y);
    // the closest candidate wins
    others.push_back({{100, 420, 400, 80}, {}, 1});
    r = snap_frame({105, 396, 200, 20}, {}, others);  // bottom at 416: 4 from window 1's top (420), top 396: 4 from window 0's bottom (400)
    CHECK(r.snapped_y && std::fabs(std::fabs(r.dy) - 4) < 1e-4f);
    // groups: parent[i] = window i is docked to parent[i]
    CHECK(group_children({-1, 0, 1, -1, 0}, 0) == std::vector<int>({1, 2, 4}));
    CHECK(group_children({-1, 0, 1, -1, 0}, 1) == std::vector<int>({2}));
    CHECK(group_children({-1, 0, 1, -1, 0}, 3).empty());
    CHECK(group_children({1, 0}, 0).size() == 1);  // a cycle terminates
    CHECK(group_children({0}, 5).empty() && group_children({}, 0).empty());
    // default placement
    Placement below;
    below.side = Placement::Side::Below; below.dx = 10; below.dy = 20;
    const Point pb = place_relative(below, {100, 100, 400, 300}, 200, 100);
    CHECK(pb.x == 110 && pb.y == 420);
    Placement left;
    left.side = Placement::Side::Left; left.dx = -8;
    const Point pl = place_relative(left, {100, 100, 400, 300}, 200, 100);
    CHECK(pl.x == -108 && pl.y == 100);
    Placement above;
    above.side = Placement::Side::Above;
    CHECK(place_relative(above, {100, 100, 400, 300}, 200, 100).y == 0);
  }

  // ---- uniform scaling and free resizing
  {
    const Skin& o = *skins["orbit"];
    const Window& m = *o.find_window("main");
    CHECK(clamp_user_scale(m, 0.1f) == 0.5f && clamp_user_scale(m, 9) == 2.0f && clamp_user_scale(m, 1.25f) == 1.25f);
    CHECK(window_size(m, 1.5f).w == 1500 && window_size(m, 1.5f).h == 840);
    // dragging the bottom-right grip outwards grows uniformly; inwards shrinks; clamped at the limits
    const float grown = scale_from_corner_drag(m, 1.0f, "br", 100, 56);
    CHECK(grown > 1.0f && grown < 1.2f);
    CHECK(scale_from_corner_drag(m, 1.0f, "br", -100, -56) < 1.0f);
    CHECK(scale_from_corner_drag(m, 1.0f, "tl", -100, -56) > 1.0f);  // the top-left corner moves the other way
    CHECK(scale_from_corner_drag(m, 1.0f, "br", 100000, 100000) == 2.0f && scale_from_corner_drag(m, 1.0f, "br", -100000, -100000) == 0.5f);
    CHECK(window_size(m, 1.0f).w * 0.56f == window_size(m, 1.0f).h);  // aspect preserved
    Window free_w;
    free_w.width = 400; free_w.height = 200; free_w.min_w = 200; free_w.min_h = 100; free_w.max_w = 800; free_w.max_h = 400;
    CHECK(clamp_free_size(free_w, 100, 50).w == 200 && clamp_free_size(free_w, 9000, 9000).h == 400 && clamp_free_size(free_w, 300, 150).w == 300);
    free_w.aspect_lock = true;
    const deckhand::skin::Size ar = clamp_free_size(free_w, 600, 600);
    CHECK(std::fabs(ar.w / ar.h - 2.0f) < 1e-4f);
    CHECK(window_size(*skins["default"]->find_window("main"), 3.0f).w == 1280);  // classic windows ignore the factor
  }

  // ---- arrangement persistence
  {
    WinState s;
    s.have_pos = true; s.x = -1440.5f; s.y = 62; s.scale = 1.375f; s.w = 700; s.h = 300; s.visible = true; s.z = 4; s.parent = "main";
    const std::string text = encode_win_state(s);
    WinState back;
    CHECK(decode_win_state(text, back));
    CHECK(back.have_pos && back.x == -1440.5f && back.y == 62 && std::fabs(back.scale - 1.375f) < 1e-3f && back.w == 700 && back.h == 300 && back.visible && back.z == 4 && back.parent == "main");
    WinState nopos;
    nopos.visible = false;
    CHECK(decode_win_state(encode_win_state(nopos), back) && !back.have_pos && !back.visible && back.parent.empty());
    for (const char* bad : {"", "x=1", "garbage", "x=abc y=2", "x=1e99 y=2", "x=1 y=2 s=0 v=1", "x=1 y=2 s=99", "x=1 y=2 p=../../etc", "x=1 y=2 w=-5", "x=1 y=2 q=3", "= =", "x=1 y=2 s=nan"}) {
      WinState keep = back;
      CHECK(!decode_win_state(bad, keep));
      CHECK(keep.x == back.x && keep.parent == back.parent);  // untouched on failure
    }
    CHECK(!decode_win_state(std::string(500, 'x'), back));
    CHECK(win_setting_key("orbit", "browser") == "ui.win.orbit.browser");
    CHECK(win_setting_key("../evil skin", "a b") == "ui.win.___evil_skin.a_b");
  }

  // ---- path sliders: value <-> position along an arbitrary curve
  {
    PathCurve line;
    std::string err;
    CHECK(parse_path("M 0 0 L 100 0", line, err) && std::fabs(line.length() - 100) < 1e-3f);
    CHECK(std::fabs(line.at(0.5f).x - 50) < 1e-3f && line.at(0.5f).y == 0 && line.at(-3).x == 0 && line.at(7).x == 100);
    CHECK(std::fabs(line.project(25, 40) - 0.25f) < 1e-4f && line.project(-50, 0) == 0 && line.project(500, 9) == 1);
    PathCurve curve;
    CHECK(parse_path("M 55 8 Q 8 140 55 272", curve, err));
    CHECK(curve.pts.size() > 10 && curve.length() > 264);  // longer than the chord because it bulges
    for (float v = 0; v <= 1.0001f; v += 0.1f) {
      const Point p = path_handle_at(curve, v);
      CHECK(std::fabs(path_value_at(curve, p.x, p.y) - v) < 0.01f);  // position -> value -> position round trips
      CHECK(std::fabs(path_value_at(curve, p.x - 3, p.y + 2) - v) < 0.05f);  // near the track also works
    }
    // a cubic jaw-line with a hairpin keeps the nearest-point mapping monotone on each leg
    PathCurve jaw;
    CHECK(parse_path("M 0 0 C 40 80 160 80 200 0 L 200 40 H 0 V 60 Z", jaw, err) && jaw.length() > 300);
    CHECK(jaw.project(jaw.at(0.3f).x, jaw.at(0.3f).y) > 0.2f && jaw.project(jaw.at(0.3f).x, jaw.at(0.3f).y) < 0.4f);
    for (const char* bad : {"", "L 1 1", "M 1", "M 0 0 L", "M 0 0 l 1 1", "M 0 0 X 1 1", "M 0 0", "M 1e99 0 L 1 1", "M 0 0 L nan 1"}) {
      PathCurve c;
      CHECK(!parse_path(bad, c, err));
    }
    CHECK(PathCurve{}.project(1, 1) == 0 && PathCurve{}.at(0.5f).x == 0);
  }

  // ---- directory skins: confinement of asset paths
  {
    fs::path root = fs::temp_directory_path() / "dh_skin_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ok" / "img", ec);
    fs::create_directories(root / "evil" / "img", ec);
    fs::create_directories(root / "missing", ec);
    const std::string skin = wrap("atlas a src=\"img/a.png\" {\n region b 0 0 8 8 slice=2\n}\n") + std::string(kMinWindow);
    for (const char* d : {"ok", "evil", "missing"}) std::ofstream(root / d / "skin.dhs") << skin;
    std::ofstream(root / "ok" / "img" / "a.png") << "png";
    std::ofstream(root / "secret.png") << "secret";
    fs::create_symlink(root / "secret.png", root / "evil" / "img" / "a.png", ec);
    ParseResult ok = load_skin((root / "ok").string());
    CHECK(ok.ok());
    CHECK(ok.skin->find_atlas("a") && ok.skin->find_atlas("a")->regions[0].nine_slice);
    ParseResult evil = load_skin((root / "evil").string());
    if (!ec) CHECK(!evil.ok() && evil.describe().find("outside the skin directory") != std::string::npos);
    {  // a window mask is an asset too: it must stay inside the skin directory
      fs::create_directories(root / "maskevil" / "img", ec);
      std::ofstream(root / "maskevil" / "skin.dhs") << wrap("window w size=400x300 chrome=none mask=\"img/m.png\" {\n row { spacer\n spacer } }\n");
      fs::create_symlink(root / "secret.png", root / "maskevil" / "img" / "m.png", ec);
      ParseResult me = load_skin((root / "maskevil").string());
      if (!ec) CHECK(!me.ok() && me.describe().find("outside the skin directory") != std::string::npos);
      fs::create_directories(root / "maskmissing", ec);
      std::ofstream(root / "maskmissing" / "skin.dhs") << wrap("window w size=400x300 chrome=none mask=\"img/none.png\" {\n row { spacer\n spacer } }\n");
      CHECK(!load_skin((root / "maskmissing").string()).ok());
    }
    ParseResult miss = load_skin((root / "missing").string());
    CHECK(!miss.ok());
    CHECK(!load_skin((root / "nonexistent").string()).ok());
    fs::remove_all(root, ec);
  }

  std::puts("test_skin OK");
  return 0;
}
