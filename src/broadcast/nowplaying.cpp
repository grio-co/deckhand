#include "deckhand/broadcast/nowplaying.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

#include "deckhand/broadcast/json.hpp"
#include "deckhand/log/log.hpp"
#include "overlay_server.hpp"
#include "sock.hpp"

namespace deckhand::broadcast {
namespace {

namespace fs = std::filesystem;

std::string identity_of(const TrackInfo& t) {
  if (t.title.empty() && t.artist.empty() && t.album.empty()) return "deck#" + std::to_string(t.deck);
  return t.title + '\x1f' + t.artist + '\x1f' + t.album;
}

TrackInfo info_of(int deck_index, const DeckState& d) {
  TrackInfo t;
  t.deck = deck_index + 1;
  t.title = d.title;
  t.artist = d.artist;
  t.album = d.album;
  t.key = d.key;
  t.bpm = d.bpm;
  t.length_seconds = d.length_seconds;
  t.artwork = d.artwork;
  return t;
}

bool same_display(const TrackInfo& a, const TrackInfo& b) {
  return a.deck == b.deck && a.title == b.title && a.artist == b.artist && a.album == b.album && a.key == b.key &&
         a.artwork == b.artwork && std::lround(a.bpm * 10) == std::lround(b.bpm * 10);
}

bool write_atomic(const fs::path& path, const std::string& content) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.flush();
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // atomic replace on POSIX
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string format_offset(double seconds) {
  const long total = seconds > 0 ? static_cast<long>(seconds) : 0;
  char buf[32];
  if (total >= 3600) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
  else std::snprintf(buf, sizeof buf, "%02ld:%02ld", total / 60, total % 60);
  return buf;
}

std::string bpm_text(double bpm) {
  if (bpm <= 0) return {};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", bpm);
  return buf;
}

}  // namespace

std::string NowPlaying::render(const std::string& tmpl, const TrackInfo& t) {
  std::string out;
  for (std::size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '{') {
      const std::size_t e = tmpl.find('}', i);
      if (e != std::string::npos) {
        const std::string name = tmpl.substr(i + 1, e - i - 1);
        const std::string* s = nullptr;
        std::string tmp;
        if (name == "title") s = &t.title;
        else if (name == "artist") s = &t.artist;
        else if (name == "album") s = &t.album;
        else if (name == "key") s = &t.key;
        else if (name == "bpm") { tmp = bpm_text(t.bpm); s = &tmp; }
        else if (name == "deck") { tmp = std::to_string(t.deck); s = &tmp; }
        if (s) {
          out += *s;
          i = e + 1;
          continue;
        }
      }
    }
    out.push_back(tmpl[i++]);
  }
  return out;
}

std::string NowPlaying::to_json(const std::optional<TrackInfo>& t, const std::string& text, bool for_http) {
  Json j = Json::object();
  j.set("on_air", Json(t.has_value()));
  if (t) {
    j.set("deck", Json(t->deck));
    j.set("title", Json(t->title));
    j.set("artist", Json(t->artist));
    j.set("album", Json(t->album));
    j.set("key", Json(t->key));
    j.set("bpm", Json(std::round(t->bpm * 10) / 10));
    j.set("length_seconds", Json(std::round(t->length_seconds * 10) / 10));
    j.set("text", Json(text));
    if (!t->artwork.empty())
      j.set("artwork", Json(for_http ? "/artwork?v=" + std::to_string(std::hash<std::string>{}(t->artwork) % 1000000007u)
                                     : t->artwork));
  }
  return j.dump();
}

NowPlaying::NowPlaying(NowPlayingConfig cfg) { configure(cfg); }
NowPlaying::~NowPlaying() = default;

void NowPlaying::configure(const NowPlayingConfig& cfg) {
  std::lock_guard<std::mutex> lk(m_);
  cfg_ = cfg;
  server_.reset();
  if (cfg_.http.enabled) {
    auto s = std::make_unique<OverlayServer>();
    if (s->start(cfg_.http.port)) server_ = std::move(s);
  }
  write_outputs_locked(true);
}

void NowPlaying::on_track_change(std::function<void(const TrackInfo&)> cb) {
  std::lock_guard<std::mutex> lk(m_);
  on_change_ = std::move(cb);
}

std::optional<TrackInfo> NowPlaying::current() const {
  std::lock_guard<std::mutex> lk(m_);
  return current_;
}

std::vector<TracklistEntry> NowPlaying::tracklist() const {
  std::lock_guard<std::mutex> lk(m_);
  return tracklist_;
}

std::string NowPlaying::tracklist_text() const {
  std::lock_guard<std::mutex> lk(m_);
  std::string out;
  for (const auto& e : tracklist_) out += format_offset(e.offset_seconds) + " " + e.label + "\n";
  return out;
}

std::uint16_t NowPlaying::http_port() const {
  std::lock_guard<std::mutex> lk(m_);
  return server_ ? server_->port() : 0;
}

void NowPlaying::reset() {
  std::lock_guard<std::mutex> lk(m_);
  current_.reset();
  current_identity_.clear();
  cand_.reset();
  tracklist_.clear();
  write_outputs_locked(true);
}

void NowPlaying::publish_http_locked() {
  if (!server_) return;
  const std::string text = current_ ? render(cfg_.text_template, *current_) : std::string();
  server_->publish(to_json(current_, text, true), current_ ? current_->artwork : std::string());
}

void NowPlaying::write_outputs_locked(bool tracklist_changed) {
  const std::string text = current_ ? render(cfg_.text_template, *current_) : std::string();
  if (cfg_.files.enabled && !cfg_.files.folder.empty()) {
    std::error_code ec;
    fs::create_directories(cfg_.files.folder, ec);
    const fs::path dir = cfg_.files.folder;
    bool ok = write_atomic(dir / "title.txt", current_ ? current_->title : std::string());
    ok &= write_atomic(dir / "artist.txt", current_ ? current_->artist : std::string());
    ok &= write_atomic(dir / "now-playing.txt", text);
    ok &= write_atomic(dir / "now-playing.json", to_json(current_, text, false) + "\n");
    if (tracklist_changed) {
      std::string tl;
      for (const auto& e : tracklist_) tl += format_offset(e.offset_seconds) + " " + e.label + "\n";
      ok &= write_atomic(dir / "tracklist.txt", tl);
    }
    if (!ok) DH_LOG(Warn, "now playing: could not write to %s", cfg_.files.folder.c_str());
  }
  publish_http_locked();
}

void NowPlaying::update(const NowPlayingState& st, double now) {
  if (now < 0) now = net::now_seconds();
  std::function<void(const TrackInfo&)> cb;
  std::optional<TrackInfo> fired;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto sounding = [&](const DeckState& d) { return d.loaded && d.playing && d.audible >= cfg_.min_audible; };
    int best = -1;
    for (std::size_t i = 0; i < st.decks.size(); ++i) {
      if (!sounding(st.decks[i])) continue;
      if (best < 0 || st.decks[i].audible > st.decks[static_cast<std::size_t>(best)].audible) best = static_cast<int>(i);
    }
    if (best < 0) {
      cand_.reset();  // nothing audible: keep the last track on air
    } else {
      const DeckState& d = st.decks[static_cast<std::size_t>(best)];
      const TrackInfo ti = info_of(best, d);
      const std::string id = identity_of(ti);
      if (current_ && id == current_identity_) {
        cand_.reset();
        if (!same_display(*current_, ti) && now - last_refresh_ >= 1.0) {
          current_ = ti;
          last_refresh_ = now;
          write_outputs_locked(false);
        }
      } else {
        bool allowed = true;
        if (current_) {
          // While the current track still sounds, a challenger must be clearly louder.
          for (std::size_t i = 0; i < st.decks.size(); ++i) {
            if (!sounding(st.decks[i]) || identity_of(info_of(static_cast<int>(i), st.decks[i])) != current_identity_)
              continue;
            if (d.audible < st.decks[i].audible + cfg_.switch_margin) allowed = false;
          }
        }
        if (!allowed) {
          cand_.reset();
        } else {
          if (!cand_ || cand_->deck != ti.deck || cand_->identity != id) cand_ = Candidate{ti.deck, id, now};
          if (now - cand_->since >= cfg_.hold_seconds) {
            TracklistEntry e;
            if (tracklist_.empty()) first_time_ = cand_->since;
            e.offset_seconds = cand_->since - first_time_;
            e.track = ti;
            e.label = render(cfg_.tracklist_template, ti);
            tracklist_.push_back(std::move(e));
            current_ = ti;
            current_identity_ = id;
            cand_.reset();
            last_refresh_ = now;
            write_outputs_locked(true);
            cb = on_change_;
            fired = ti;
          }
        }
      }
    }
  }
  if (cb && fired) {
    try {
      cb(*fired);
    } catch (...) {
    }
  }
}

}  // namespace deckhand::broadcast
