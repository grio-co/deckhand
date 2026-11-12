// skin: parse/validate shipped skins, layout invariants at several sizes, hostile input rejection.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "check.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/skin/skin.hpp"

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

static void check_layout(const Skin& sk, const char* name, float W, float H, float scale) {
  Layout L = solve_layout(sk, 0, W, H, scale);
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
    if (w.parent >= 0) {
      const auto& p = L.widgets[w.parent];
      CHECK(w.rect.x >= p.rect.x - eps && w.rect.y >= p.rect.y - eps && w.rect.right() <= p.rect.right() + eps && w.rect.bottom() <= p.rect.bottom() + eps);
    }
    if (w.interactive) {
      ++interactive;
      if (!(w.rect.w > 1 && w.rect.h > 1)) {
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

int main() {
  // ---- shipped skins
  std::map<std::string, std::shared_ptr<const Skin>> skins;
  for (const char* n : {"default", "voidwave"}) {
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

  struct Size { float w, h, s; };
  const Size sizes[] = {{1280, 720, 1}, {1920, 1080, 1}, {3024, 1964, 2}, {1024, 600, 1}};
  for (auto& [name, sk] : skins)
    for (auto& z : sizes) check_layout(*sk, name.c_str(), z.w, z.h, z.s);

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
    ParseResult miss = load_skin((root / "missing").string());
    CHECK(!miss.ok());
    CHECK(!load_skin((root / "nonexistent").string()).ok());
    fs::remove_all(root, ec);
  }

  std::puts("test_skin OK");
  return 0;
}
