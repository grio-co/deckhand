// The recording indicators of the status row, the quit guard's reasons, and that the skin, the keymap and the language
// packs carry the pieces (the status-row button of the default skin, its binding, the shortcut, the strings).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "check.hpp"
#include "deckhand/app/keymap.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "skin_view.hpp"
#include "status_indicators.hpp"

using namespace deckhand;

namespace {
std::string slurp(const char* path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
}  // namespace

int main() {
  CHECK(indicator_clock(0) == "0:00" && indicator_clock(65.9) == "1:05" && indicator_clock(3723) == "1:02:03" && indicator_clock(-4) == "0:00");

  {  // the recording from an input: only Recording and Paused show; clips or dropped samples warn
    CaptureService::Status s;
    CHECK(!capture_indicator(s).active);
    s.state = CaptureService::State::Stopped;
    s.seconds = 90;
    CHECK(!capture_indicator(s).active);  // a finished side is not "recording"
    s.state = CaptureService::State::Recording;
    s.seconds = 125.4;
    CaptureIndicator i = capture_indicator(s);
    CHECK(i.active && !i.paused && !i.warn && i.clock == "2:05" && i.clips == 0);
    s.clips = 3;
    i = capture_indicator(s);
    CHECK(i.warn && i.clips == 3);
    s.clips = 0;
    s.dropped = 12;
    CHECK(capture_indicator(s).warn);
    s.state = CaptureService::State::Paused;
    i = capture_indicator(s);
    CHECK(i.active && i.paused);
  }
  {  // the recording of the mix
    RecorderStatus r;
    CHECK(!mix_indicator(r).active);
    r.open = true;  // armed, not recording
    CHECK(!mix_indicator(r).active);
    r.recording = true;
    r.seconds = 61;
    MixIndicator m = mix_indicator(r);
    CHECK(m.active && !m.warn && m.clock == "1:01");
    r.dropped_frames = 5;
    CHECK(mix_indicator(r).warn);
    r.dropped_frames = 0;
    r.error = true;
    CHECK(mix_indicator(r).warn);
  }
  {  // what quitting would lose
    CaptureService::Status c;
    RecorderStatus r;
    CHECK(!quit_risk(c, r).any());
    c.state = CaptureService::State::Paused;
    QuitRisk q = quit_risk(c, r);
    CHECK(q.capture && !q.mix && q.any());
    r.recording = true;
    q = quit_risk(c, r);
    CHECK(q.capture && q.mix);
    c.state = CaptureService::State::Idle;
    q = quit_risk(c, r);
    CHECK(!q.capture && q.mix);
  }

  {  // skin bindings: the status-row button opens the recording panel in every skin that shows it
    const ui::Target t = ui::map_binding("app.capture.open");
    CHECK(t.type == ui::Target::Type::Action && t.name == "capture_open");
    CHECK(ui::map_binding("app.capture_active").type == ui::Target::Type::Info);
    CHECK(ui::map_binding("app.rec_active").type == ui::Target::Type::Info);
    CHECK(ui::map_binding("app.rec_status").type == ui::Target::Type::Info);
    CHECK(ui::map_binding("browser.playlist.share").name == "playlist_share");
    const std::string skin = slurp("resources/skins/default.dhskin");
    CHECK(skin.find("app.capture.open") != std::string::npos && skin.find("show=\"app.capture_active\"") != std::string::npos);
    CHECK(skin.find("app.rec_status") != std::string::npos && skin.find("browser.playlist.share") != std::string::npos);
    // it sits inside the status bar template
    const auto bar = skin.find("template statusbar");
    const auto cap = skin.find("app.capture.open");
    CHECK(bar != std::string::npos && cap > bar && cap < skin.find("use statusbar"));
  }
  {  // the shortcut for the shared-playlist panel exists and is bound by default
    bool known = false;
    for (const KeyControlInfo& c : list_key_controls()) known = known || c.name == "ui.shared_playlist";
    CHECK(known);
    CHECK(slurp("resources/keymaps/default.dhkeys").find("ui.shared_playlist") != std::string::npos);
  }
  {  // strings in all five packs
    i18n::Catalog cat;
    CHECK(cat.load_builtin("resources/lang/en.dhlang"));
    std::vector<std::string> errs;
    CHECK(cat.load_directory("resources/lang", &errs) >= 5 && errs.empty());
    for (const char* tag : {"en", "de", "fr", "es", "ja"}) {
      CHECK(cat.set_language(tag));
      for (const char* k : {"capture.indicator", "capture.indicator_paused", "capture.indicator_clip", "rec.indicator", "rec.indicator_warn"})
        CHECK(cat.tr(k, {{"time", "1:23"}, {"count", 4}}).find("1:23") != std::string::npos);
      CHECK(cat.tr("capture.indicator_clip", {{"time", "1:23"}, {"count", 4}}).find("4") != std::string::npos);
      CHECK(cat.has("capture.indicator_hint") && cat.has("playlist.share"));
    }
  }
  std::puts("test_status_indicators: ok");
  return 0;
}
