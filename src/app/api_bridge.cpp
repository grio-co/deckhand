#include "deckhand/app/api_bridge.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "deckhand/api/server.hpp"
#include "deckhand/app/controls.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {

using api::Json;
using api::Request;
using api::Response;

namespace {

std::int64_t unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json track_json(const TrackRow& t) {
  Json j = Json::object();
  j.set("id", t.id);
  j.set("title", t.title);
  j.set("artist", t.artist);
  j.set("album", t.album);
  j.set("genre", t.genre);
  j.set("key", t.key);
  j.set("bpm", t.bpm);
  j.set("duration_ms", t.duration_ms);
  j.set("rating", t.rating);
  j.set("format", t.format);
  j.set("path", t.path);
  return j;
}

Json seconds_or_null(double frames, double rate) { return frames < 0 || rate <= 0 ? Json() : Json(frames / rate); }

bool int_param(const Request& r, const char* name, std::int64_t& out) {
  const auto it = r.params.find(name);
  if (it == r.params.end() || it->second.empty() || it->second.size() > 18) return false;
  std::int64_t v = 0;
  for (const char ch : it->second) {
    if (ch < '0' || ch > '9') return false;
    v = v * 10 + (ch - '0');
  }
  out = v;
  return true;
}

SortKey sort_key(const std::string& s) {
  if (s == "title") return SortKey::Title;
  if (s == "artist") return SortKey::Artist;
  if (s == "album") return SortKey::Album;
  if (s == "genre") return SortKey::Genre;
  if (s == "bpm") return SortKey::Bpm;
  if (s == "key") return SortKey::Key;
  if (s == "duration") return SortKey::Duration;
  if (s == "rating") return SortKey::Rating;
  if (s == "added") return SortKey::DateAdded;
  return SortKey::Default;
}

// A control change waiting for its moment.
struct Scheduled {
  std::uint64_t id = 0;
  std::string control;
  bool has_value = false;
  double value = 0;
  std::int64_t at_unix_ms = 0;  // > 0: wall-clock trigger
  int when_deck = -1;           // >= 0: trigger when this deck's playhead reaches when_seconds
  double when_seconds = 0;
};

}  // namespace

struct ApiBridge::Impl {
  ApiHost& host;
  std::string data_dir;
  std::mutex lib_mutex;
  Library lib;
  api::TokenStore tokens;
  std::unique_ptr<api::ApiServer> server;
  std::string description = "API off";

  std::atomic<bool> quit{false};
  std::thread ticker;
  std::mutex sched_mutex;
  std::vector<Scheduled> scheduled;
  std::uint64_t next_schedule_id = 1;

  Impl(ApiHost& h, const std::string& dir) : host(h), data_dir(dir), lib(dir + "/library.db"), tokens(dir + "/api-tokens.json") {}

  // ---- JSON views --------------------------------------------------------------------
  Json deck_json(int index) {
    const DeckView v = host.api_engine().deck(index);
    Json j = Json::object();
    j.set("deck", index + 1);
    j.set("loaded", v.loaded);
    j.set("playing", v.playing);
    j.set("sync", v.sync);
    j.set("pfl", v.pfl);
    j.set("position_seconds", v.sample_rate > 0 ? v.position / v.sample_rate : 0.0);
    j.set("length_seconds", v.sample_rate > 0 ? v.length / v.sample_rate : 0.0);
    j.set("bpm", v.bpm);
    j.set("tempo", v.tempo);
    j.set("tempo_range", v.tempo_range);
    j.set("speed", v.speed);
    j.set("cue_seconds", seconds_or_null(v.loaded ? v.cue : -1, v.sample_rate));
    Json loop = Json::object();
    loop.set("active", v.loop_active);
    loop.set("in_seconds", seconds_or_null(v.loop_in, v.sample_rate));
    loop.set("out_seconds", seconds_or_null(v.loop_out, v.sample_rate));
    j.set("loop", std::move(loop));
    Json hot = Json::array();
    for (int h = 0; h < kHotCues; ++h) hot.push_back(seconds_or_null(v.hot_cue[h], v.sample_rate));
    j.set("hot_cues", std::move(hot));
    j.set("level", static_cast<double>(v.level));
    j.set("key_lock", v.key_lock);
    j.set("key_shift", static_cast<double>(v.key_shift));
    j.set("quantize", v.quantize);
    j.set("slip", v.slip);
    j.set("reverse", v.reverse);
    j.set("auto_gain_db", static_cast<double>(v.auto_gain_db));
    Json fx = Json::array();
    for (int f = 0; f < kFxSlots; ++f) {
      Json e = Json::object();
      const EffectInfo* info = effect_info(v.fx[f].kind);
      e.set("effect", info ? info->name : "");
      e.set("on", v.fx[f].on);
      e.set("wet", static_cast<double>(v.fx[f].wet));
      e.set("param", static_cast<double>(v.fx[f].param));
      e.set("beats", static_cast<double>(v.fx[f].beats));
      fx.push_back(std::move(e));
    }
    j.set("fx", std::move(fx));
    Json ch = Json::object();
    ch.set("trim", static_cast<double>(v.trim));
    ch.set("eq_high", static_cast<double>(v.eq_high));
    ch.set("eq_mid", static_cast<double>(v.eq_mid));
    ch.set("eq_low", static_cast<double>(v.eq_low));
    ch.set("filter", static_cast<double>(v.filter));
    ch.set("fader", static_cast<double>(v.fader));
    j.set("channel", std::move(ch));
    if (v.loaded && v.track_id != 0) {
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (const auto row = lib.track(v.track_id)) j.set("track", track_json(*row));
    }
    return j;
  }

  Json mixer_json() {
    const MixerView m = host.api_engine().mixer();
    Json j = Json::object();
    j.set("crossfader", static_cast<double>(m.crossfader));
    j.set("head_mix", static_cast<double>(m.head_mix));
    j.set("head_level", static_cast<double>(m.head_level));
    j.set("master_level", static_cast<double>(m.master_level));
    j.set("peak_left", static_cast<double>(m.peak_left));
    j.set("peak_right", static_cast<double>(m.peak_right));
    return j;
  }

  // ---- controls and scheduling ----------------------------------------------------------
  // body: {"value": n, "in_ms": n | "at_unix_ms": n | "when": {"deck": 1, "seconds": 200.0}}
  Response control(const std::string& name, const Json& body) {
    const Json& value = body["value"];
    if (!value.is_null() && !value.is_number() && !value.is_bool()) return Response::error(400, "value must be a number");
    const bool has_value = !value.is_null();
    const double v = value.is_bool() ? (value.as_bool() ? 1.0 : 0.0) : value.as_double();

    Scheduled s;
    s.control = name;
    s.has_value = has_value;
    s.value = v;
    if (body["in_ms"].is_number()) s.at_unix_ms = unix_ms() + std::max<std::int64_t>(0, body["in_ms"].as_int());
    if (body["at_unix_ms"].is_number()) s.at_unix_ms = body["at_unix_ms"].as_int();
    const Json& when = body["when"];
    if (when.is_object()) {
      const std::int64_t deck = when["deck"].as_int(0);
      if (deck < 1 || deck > kMaxDecks || !when["seconds"].is_number()) return Response::error(400, "when needs deck (1..4) and seconds");
      s.when_deck = static_cast<int>(deck - 1);
      s.when_seconds = when["seconds"].as_double();
    }
    if (s.at_unix_ms == 0 && s.when_deck < 0) {
      std::string error;
      if (!apply_control(host.api_engine(), name, has_value, v, error)) return Response::error(400, error);
      Json j = Json::object();
      j.set("control", name);
      return Response::ok(std::move(j));
    }
    {  // validate the name now so mistakes are reported at request time
      double ignored;
      std::string error;
      bool known = read_control(host.api_engine(), name, ignored);
      if (!known) {
        for (const ControlInfo& c : list_controls()) known = known || c.name == name;
      }
      if (!known) return Response::error(400, "unknown control");
    }
    std::lock_guard<std::mutex> lock(sched_mutex);
    if (scheduled.size() >= 10000) return Response::error(429, "too many scheduled commands");
    s.id = next_schedule_id++;
    Json j = Json::object();
    j.set("control", name);
    j.set("scheduled_id", s.id);
    scheduled.push_back(std::move(s));
    Response r = Response::ok(std::move(j));
    r.status = 202;
    return r;
  }

  void run_due() {
    std::vector<Scheduled> due;
    {
      std::lock_guard<std::mutex> lock(sched_mutex);
      if (scheduled.empty()) return;
      const std::int64_t now = unix_ms();
      DeckView views[kMaxDecks];
      for (int i = 0; i < kMaxDecks; ++i) views[i] = host.api_engine().deck(i);
      for (auto it = scheduled.begin(); it != scheduled.end();) {
        bool fire = it->at_unix_ms > 0 && now >= it->at_unix_ms;
        if (it->when_deck >= 0) {
          const DeckView& v = views[it->when_deck];
          fire = v.loaded && v.sample_rate > 0 && v.position / v.sample_rate >= it->when_seconds;
        }
        if (fire) {
          due.push_back(std::move(*it));
          it = scheduled.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const Scheduled& s : due) {
      std::string error;
      const bool ok = apply_control(host.api_engine(), s.control, s.has_value, s.value, error);
      Json e = Json::object();
      e.set("type", "scheduled");
      e.set("scheduled_id", s.id);
      e.set("control", s.control);
      e.set("ok", ok);
      if (!ok) e.set("error", error);
      server->publish("events", e);
    }
  }

  // Publishes deck and mixer state, and events for changes worth reacting to.
  void tick_main() {
    struct Seen {
      std::int64_t track_id = 0;
      bool playing = false;
    } seen[kMaxDecks];
    int n = 0;
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      run_due();
      if (++n % 50 != 0) continue;  // state at 10 Hz
      for (int i = 0; i < kMaxDecks; ++i) {
        const DeckView v = host.api_engine().deck(i);
        const std::int64_t id = v.loaded ? v.track_id : 0;
        if (id != seen[i].track_id || v.playing != seen[i].playing) {
          Json e = Json::object();
          e.set("type", id != seen[i].track_id ? (id ? "deck.loaded" : "deck.unloaded") : (v.playing ? "deck.play" : "deck.pause"));
          e.set("deck", deck_json(i));
          server->publish("events", e);
          seen[i] = {id, v.playing};
        }
      }
      if (server->ws_client_count() == 0) continue;
      Json decks = Json::array();
      for (int i = 0; i < kMaxDecks; ++i) decks.push_back(deck_json(i));
      server->publish("decks", decks);
      server->publish("mixer", mixer_json());
    }
  }

  // ---- routes ---------------------------------------------------------------------------
  void add_routes() {
    auto& s = *server;
    s.add_route("GET", "/v1/status", "read", [this](const Request&) {
      Json j = Json::object();
      j.set("app", "DeckHand");
      j.set("api", 1);
      j.set("status", host.api_status());
      j.set("audio_device", host.api_audio_device());
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        j.set("tracks", lib.track_count());
      }
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/decks", "read", [this](const Request&) {
      Json a = Json::array();
      for (int i = 0; i < kMaxDecks; ++i) a.push_back(deck_json(i));
      return Response::ok(std::move(a));
    });
    s.add_route("GET", "/v1/decks/{deck}", "read", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      return Response::ok(deck_json(static_cast<int>(deck - 1)));
    });
    s.add_route("POST", "/v1/decks/{deck}/load", "control:deck:{deck}", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      if (r.body["path"].is_string()) {
        const std::string path = r.body["path"].as_string();
        if (path.empty() || path[0] != '/' || !std::filesystem::exists(path)) return Response::error(400, "path must be an existing absolute file");
        host.api_add_file(path, static_cast<int>(deck - 1));
        Response ok = Response::ok();
        ok.status = 202;
        return ok;
      }
      if (!r.body["track_id"].is_number()) return Response::error(400, "give track_id or path");
      std::string error;
      if (!host.api_load(static_cast<int>(deck - 1), r.body["track_id"].as_int(), error)) return Response::error(409, error);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });
    // Every deck control: POST /v1/decks/1/play, /v1/decks/2/hotcue.3, /v1/decks/1/seek {"value": 62.5}
    s.add_route("POST", "/v1/decks/{deck}/{control}", "control:deck:{deck}", [this](const Request& r) {
      return control("deck" + r.params.at("deck") + "." + r.params.at("control"), r.body);
    });
    s.add_route("GET", "/v1/mixer", "read", [this](const Request&) { return Response::ok(mixer_json()); });
    s.add_route("POST", "/v1/mixer/{control}", "control:mixer", [this](const Request& r) {
      return control("mixer." + r.params.at("control"), r.body);
    });
    s.add_route("GET", "/v1/controls", "read", [this](const Request&) {
      Json a = Json::array();
      for (const ControlInfo& c : list_controls()) {
        Json j = Json::object();
        j.set("name", c.name);
        j.set("kind", c.kind);
        j.set("range", c.range);
        j.set("help", c.help);
        double v = 0;
        if (read_control(host.api_engine(), c.name, v)) j.set("value", v);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    // Several controls in one request, each optionally scheduled: {"commands": [{"control": "deck1.play", ...}]}
    s.add_route("POST", "/v1/commands", "control", [this](const Request& r) {
      const Json& list = r.body["commands"];
      if (!list.is_array() || list.size() == 0 || list.size() > 500) return Response::error(400, "commands must be an array of 1..500");
      Json results = Json::array();
      for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& c = list.at(i);
        Response one = c["control"].is_string() ? control(c["control"].as_string(), c) : Response::error(400, "missing control");
        Json j = Json::object();
        j.set("status", one.status);
        j.set("result", std::move(one.body));
        results.push_back(std::move(j));
      }
      return Response::ok(std::move(results));
    });
    s.add_route("GET", "/v1/schedule", "read", [this](const Request&) {
      Json a = Json::array();
      std::lock_guard<std::mutex> lock(sched_mutex);
      for (const Scheduled& c : scheduled) {
        Json j = Json::object();
        j.set("scheduled_id", c.id);
        j.set("control", c.control);
        if (c.has_value) j.set("value", c.value);
        if (c.at_unix_ms) j.set("at_unix_ms", c.at_unix_ms);
        if (c.when_deck >= 0) {
          Json w = Json::object();
          w.set("deck", c.when_deck + 1);
          w.set("seconds", c.when_seconds);
          j.set("when", std::move(w));
        }
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("DELETE", "/v1/schedule", "control", [this](const Request&) {
      std::lock_guard<std::mutex> lock(sched_mutex);
      scheduled.clear();
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("DELETE", "/v1/schedule/{id}", "control", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such command");
      std::lock_guard<std::mutex> lock(sched_mutex);
      const auto n = std::erase_if(scheduled, [&](const Scheduled& c) { return c.id == static_cast<std::uint64_t>(id); });
      if (n == 0) return Response::error(404, "no such command");
      Response ok;
      ok.status = 204;
      return ok;
    });

    s.add_route("GET", "/v1/effects", "read", [](const Request&) {
      Json a = Json::array();
      for (int i = 0; i < effect_count(); ++i) {
        const EffectInfo& e = effect_info_at(i);
        Json j = Json::object();
        j.set("number", static_cast<int>(e.kind));
        j.set("name", e.name);
        j.set("param", e.param_name);
        j.set("beat_synced", e.beat_synced);
        j.set("colour_only", e.colour);
        j.set("release", e.release);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/sampler/{slot}/load", "control", [this](const Request& r) {
      std::int64_t slot = 0;
      const std::string path = r.body["path"].as_string();
      if (!int_param(r, "slot", slot) || slot < 1 || slot > kSamplerSlots) return Response::error(404, "no such slot");
      if (path.empty() || path[0] != '/' || !std::filesystem::is_regular_file(path)) return Response::error(400, "path must be an existing absolute file");
      host.api_load_sample(static_cast<int>(slot - 1), path);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });
    s.add_route("POST", "/v1/sampler/{slot}/{control}", "control", [this](const Request& r) {
      return control("sampler." + r.params.at("slot") + "." + r.params.at("control"), r.body);
    });
    s.add_route("GET", "/v1/recording", "read", [this](const Request&) {
      const RecorderStatus st = host.api_engine().recorder_status();
      Json j = Json::object();
      j.set("recording", st.recording);
      j.set("seconds", st.seconds);
      j.set("bytes", st.bytes);
      j.set("dropped_frames", st.dropped_frames);
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/recording/{action}", "control", [this](const Request& r) {
      const std::string& action = r.params.at("action");
      if (action != "start" && action != "stop") return Response::error(404, "use start or stop");
      std::string path, error;
      if (!host.api_record(action == "start", path, error)) return Response::error(409, error);
      Json j = Json::object();
      j.set("path", path);
      return Response::ok(std::move(j));
    });

    // ---- library ----
    s.add_route("GET", "/v1/library/tracks", "library:read", [this](const Request& r) {
      const auto q = [&](const char* k) {
        const auto it = r.query.find(k);
        return it == r.query.end() ? std::string() : it->second;
      };
      const std::int64_t limit = std::clamp<std::int64_t>(q("limit").empty() ? 200 : std::atoll(q("limit").c_str()), 1, 2000);
      const std::int64_t offset = std::max<std::int64_t>(0, std::atoll(q("offset").c_str()));
      std::vector<TrackRow> rows;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        rows = lib.tracks(std::atoll(q("playlist").c_str()), q("search"), sort_key(q("sort")), q("desc") == "1");
      }
      Json items = Json::array();
      for (std::int64_t i = offset; i < static_cast<std::int64_t>(rows.size()) && i < offset + limit; ++i)
        items.push_back(track_json(rows[static_cast<std::size_t>(i)]));
      Json j = Json::object();
      j.set("total", static_cast<std::int64_t>(rows.size()));
      j.set("offset", offset);
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/tracks/{id}", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      std::lock_guard<std::mutex> lock(lib_mutex);
      const auto row = int_param(r, "id", id) ? lib.track(id) : std::nullopt;
      if (!row) return Response::error(404, "no such track");
      Json j = track_json(*row);
      Json cues = Json::array();
      for (const Cue& c : lib.cues(id)) {
        Json cj = Json::object();
        cj.set("kind", c.kind == CueKind::Hot ? "hot" : c.kind == CueKind::Loop ? "loop" : "memory");
        if (c.slot >= 0) cj.set("slot", c.slot + 1);
        cj.set("start_ms", c.start_ms);
        if (c.end_ms >= 0) cj.set("end_ms", c.end_ms);
        cj.set("label", c.label);
        cues.push_back(std::move(cj));
      }
      j.set("cues", std::move(cues));
      Json grid = Json::array();
      for (const BeatSection& b : lib.beatgrid(id)) {
        Json g = Json::object();
        g.set("first_beat_ms", b.first_beat_ms);
        g.set("bpm", b.bpm);
        g.set("bar_beat", b.bar_beat);
        grid.push_back(std::move(g));
      }
      j.set("beatgrid", std::move(grid));
      return Response::ok(std::move(j));
    });
    s.add_route("PATCH", "/v1/library/tracks/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      TrackEdit e;
      const auto text = [&](const char* k, std::optional<std::string>& out) {
        if (r.body[k].is_string()) out = r.body[k].as_string();
      };
      text("title", e.title);
      text("artist", e.artist);
      text("album", e.album);
      text("genre", e.genre);
      text("key", e.key);
      text("comment", e.comment);
      if (r.body["rating"].is_number()) e.rating = static_cast<int>(std::clamp<std::int64_t>(r.body["rating"].as_int(), 0, 5));
      if (r.body["year"].is_number()) e.year = static_cast<int>(std::clamp<std::int64_t>(r.body["year"].as_int(), 0, 9999));
      Json out;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
        lib.edit_track(id, e);
        out = track_json(*lib.track(id));
      }
      host.api_library_changed();
      return Response::ok(std::move(out));
    });
    s.add_route("POST", "/v1/library/tracks", "library:write", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (path.empty() || path[0] != '/' || !std::filesystem::is_regular_file(path)) return Response::error(400, "path must be an existing absolute file");
      host.api_add_file(path, -1);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });
    s.add_route("GET", "/v1/library/playlists", "library:read", [this](const Request&) {
      Json a = Json::array();
      std::lock_guard<std::mutex> lock(lib_mutex);
      for (const PlaylistNode& n : lib.playlists()) {
        Json j = Json::object();
        j.set("id", n.id);
        j.set("parent", n.parent);
        j.set("name", n.name);
        j.set("folder", n.is_folder);
        j.set("tracks", n.track_count);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/library/playlists", "library:write", [this](const Request& r) {
      const std::string name = r.body["name"].as_string();
      if (name.empty() || name.size() > 200) return Response::error(400, "name is required");
      Json j = Json::object();
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        j.set("id", lib.create_playlist(r.body["parent"].as_int(0), name, r.body["folder"].as_bool(false)));
      }
      host.api_library_changed();
      Response ok = Response::ok(std::move(j));
      ok.status = 201;
      return ok;
    });
    s.add_route("PATCH", "/v1/library/playlists/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such playlist");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (r.body["name"].is_string() && !r.body["name"].as_string().empty()) lib.rename_playlist(id, r.body["name"].as_string());
        if (r.body["parent"].is_number() || r.body["position"].is_number()) {
          std::int64_t parent = 0;
          for (const PlaylistNode& n : lib.playlists())
            if (n.id == id) parent = n.parent;
          if (!lib.move_playlist(id, r.body["parent"].as_int(parent), static_cast<int>(r.body["position"].as_int(-1))))
            return Response::error(409, "cannot move a folder into itself");
        }
      }
      host.api_library_changed();
      return Response::ok();
    });
    s.add_route("DELETE", "/v1/library/playlists/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such playlist");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        lib.delete_playlist(id);
      }
      host.api_library_changed();
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("POST", "/v1/library/playlists/{id}/tracks", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id) || !r.body["track_id"].is_number()) return Response::error(400, "track_id is required");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!lib.track(r.body["track_id"].as_int())) return Response::error(404, "no such track");
        lib.add_to_playlist(id, r.body["track_id"].as_int(), static_cast<int>(r.body["position"].as_int(-1)));
      }
      host.api_library_changed();
      Response ok = Response::ok();
      ok.status = 201;
      return ok;
    });
    s.add_route("DELETE", "/v1/library/playlists/{id}/tracks/{position}", "library:write", [this](const Request& r) {
      std::int64_t id = 0, pos = 0;
      if (!int_param(r, "id", id) || !int_param(r, "position", pos)) return Response::error(404, "no such entry");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        lib.remove_from_playlist(id, static_cast<int>(pos));
      }
      host.api_library_changed();
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("GET", "/v1/history", "library:read", [this](const Request&) {
      Json a = Json::array();
      std::lock_guard<std::mutex> lock(lib_mutex);
      for (const HistorySession& h : lib.history_sessions()) {
        Json j = Json::object();
        j.set("id", h.id);
        j.set("started_at_ms", h.started_at_ms);
        j.set("entries", h.entries);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("GET", "/v1/history/{id}", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such session");
      Json a = Json::array();
      std::lock_guard<std::mutex> lock(lib_mutex);
      for (const HistoryEntry& e : lib.history(id)) {
        Json j = Json::object();
        j.set("track_id", e.track_id);
        j.set("deck", e.deck);
        j.set("title", e.title);
        j.set("artist", e.artist);
        j.set("started_at_ms", e.started_at_ms);
        j.set("played_ms", e.played_ms);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/export", "export", [this](const Request& r) {
      const std::string dest = r.body["destination"].as_string();
      if (dest.empty() || dest[0] != '/') return Response::error(400, "destination must be an absolute folder");
      std::string error;
      if (!host.api_export(dest, r.body["playlist_id"].as_int(0), error)) return Response::error(409, error);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });

    // ---- settings (admin). Most take effect at the next start. ----
    s.add_route("GET", "/v1/settings/{key}", "admin", [this](const Request& r) {
      const std::string& key = r.params.at("key");
      if (key.find("password") != std::string::npos) return Response::error(403, "this setting is write-only");
      Json j = Json::object();
      std::lock_guard<std::mutex> lock(lib_mutex);
      j.set("key", key);
      j.set("value", lib.setting(key));
      return Response::ok(std::move(j));
    });
    s.add_route("PUT", "/v1/settings/{key}", "admin", [this](const Request& r) {
      const std::string& key = r.params.at("key");
      if (key.empty() || key.size() > 64 || !r.body["value"].is_string() || r.body["value"].as_string().size() > 4096)
        return Response::error(400, "value must be a string");
      std::lock_guard<std::mutex> lock(lib_mutex);
      lib.set_setting(key, r.body["value"].as_string());
      Response ok;
      ok.status = 204;
      return ok;
    });

    // ---- tokens (admin) ----
    s.add_route("GET", "/v1/tokens", "admin", [this](const Request&) {
      Json a = Json::array();
      for (const api::TokenInfo& t : tokens.list()) {
        Json j = Json::object();
        j.set("id", t.id);
        j.set("name", t.name);
        Json sc = Json::array();
        for (const auto& x : t.scopes) sc.push_back(x);
        j.set("scopes", std::move(sc));
        j.set("created", t.created);
        j.set("expires", t.expires);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/tokens", "admin", [this](const Request& r) {
      std::vector<std::string> scopes, cidrs;
      for (std::size_t i = 0; i < r.body["scopes"].size(); ++i) scopes.push_back(r.body["scopes"].at(i).as_string());
      for (std::size_t i = 0; i < r.body["cidrs"].size(); ++i) cidrs.push_back(r.body["cidrs"].at(i).as_string());
      api::TokenStore::Created c;
      std::string error;
      if (!tokens.create(r.body["name"].as_string(), scopes, r.body["expires"].as_int(0), cidrs, &c, &error)) return Response::error(400, error);
      Json j = Json::object();
      j.set("id", c.info.id);
      j.set("token", c.secret);  // shown once
      Response ok = Response::ok(std::move(j));
      ok.status = 201;
      return ok;
    });
    s.add_route("DELETE", "/v1/tokens/{id}", "admin", [this](const Request& r) {
      if (!tokens.revoke(r.params.at("id"))) return Response::error(404, "no such token");
      Response ok;
      ok.status = 204;
      return ok;
    });

    // WebSocket: {"cmd": "control", "args": {"control": "deck1.play", "value": 1}}
    s.add_ws_command("control", "control", [this](const api::WsRequest& r) {
      if (!r.args["control"].is_string()) return Response::error(400, "missing control");
      return control(r.args["control"].as_string(), r.args);
    });
  }

  void start() {
    std::string enabled, bind, port_text, tls;
    {
      std::lock_guard<std::mutex> lock(lib_mutex);
      enabled = lib.setting("api.enabled", "1");
      bind = lib.setting("api.bind", "127.0.0.1");
      port_text = lib.setting("api.port", "7373");
      tls = lib.setting("api.tls", "0");
    }
    if (enabled != "1") return;
    std::string error;
    if (!tokens.load(&error)) DH_LOG(Warn, "API tokens: %s", error.c_str());
    if (tokens.list().empty()) {  // first run: one admin token, kept in a private file for the owner
      api::TokenStore::Created c;
      if (tokens.create("owner", {"admin"}, 0, {}, &c, &error)) {
        const std::string path = data_dir + "/api-admin-token.txt";
        std::ofstream(path) << c.secret << "\n";
        ::chmod(path.c_str(), 0600);
        DH_LOG(Info, "created the API owner token in %s", path.c_str());
      }
    }
    api::ApiConfig cfg;
    cfg.bind_address = bind;
    cfg.port = static_cast<std::uint16_t>(std::clamp(std::atoi(port_text.c_str()), 1, 65535));
    const bool loopback = bind == "127.0.0.1" || bind == "::1" || bind == "localhost";
    if (tls == "1" || !loopback) {
      cfg.tls_cert = data_dir + "/api-cert.pem";
      cfg.tls_key = data_dir + "/api-key.pem";
      if (!std::filesystem::exists(cfg.tls_cert) && !api::generate_self_signed(cfg.tls_cert, cfg.tls_key, {}, &error)) {
        DH_LOG(Error, "API certificate: %s", error.c_str());
        description = "API off (certificate failed)";
        return;
      }
    }
    server = std::make_unique<api::ApiServer>(tokens, cfg);
    add_routes();
    if (!server->start(&error)) {
      DH_LOG(Error, "API did not start: %s", error.c_str());
      description = "API off (" + error + ")";
      server.reset();
      return;
    }
    description = std::string("API ") + (server->tls_enabled() ? "https://" : "http://") + bind + ":" + std::to_string(server->port());
    DH_LOG(Info, "%s", description.c_str());
    ticker = std::thread([this] { tick_main(); });
  }
};

ApiBridge::ApiBridge(ApiHost& host, const std::string& data_dir) : impl_(std::make_unique<Impl>(host, data_dir)) {
  try {
    impl_->start();
  } catch (const std::exception& e) {
    DH_LOG(Error, "API did not start: %s", e.what());
  }
}

ApiBridge::~ApiBridge() {
  impl_->quit = true;
  if (impl_->ticker.joinable()) impl_->ticker.join();
  if (impl_->server) impl_->server->stop();
}

bool ApiBridge::running() const { return impl_->server && impl_->server->running(); }
std::string ApiBridge::describe() const { return impl_->description; }

}  // namespace deckhand
