#include "deckhand/app/api_bridge.hpp"
#include "deckhand/app/lighting_bridge.hpp"
#include "deckhand/core/key_math.hpp"
#include "deckhand/export/export_device.hpp"
#include "deckhand/app/automix.hpp"

#ifndef DECKHAND_WITH_API
#define DECKHAND_WITH_API 1
#endif
#if DECKHAND_WITH_API

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
#include "deckhand/app/cue_suggest.hpp"
#include "deckhand/app/next_tracks.hpp"
#include "deckhand/library/palette.hpp"
#include "deckhand/services/track_analysis.hpp"
#include "deckhand/app/stems_service.hpp"
#include "deckhand/export/export_preflight.hpp"
#include "deckhand/import/interchange.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/sync/shared_playlist.hpp"
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
  j.set("comment", t.comment);
  j.set("color", t.color);
  j.set("date_added", t.date_added);
  j.set("bitrate", t.bitrate);
  j.set("play_count", t.play_count);
  j.set("last_played_ms", t.last_played_ms);
  j.set("grid_locked", t.grid_locked);
  return j;
}

Json edit_json(const TrackEditRecord& e) {
  Json j = Json::object();
  j.set("id", e.id);
  j.set("track_id", e.track_id);
  j.set("batch_id", e.batch_id);
  j.set("field", e.field);
  j.set("old_value", e.old_value ? Json(*e.old_value) : Json());
  j.set("new_value", e.new_value ? Json(*e.new_value) : Json());
  j.set("edited_at", e.edited_at);
  j.set("source", e.source);
  return j;
}

Json undo_json(const UndoResult& u) {
  Json j = Json::object();
  j.set("applied", u.applied);
  j.set("batch_id", u.batch_id);
  return j;
}

Json cue_json(const Cue& c) {
  Json j = Json::object();
  j.set("kind", c.kind == CueKind::Hot ? "hot" : c.kind == CueKind::Loop ? "loop" : "memory");
  if (c.slot >= 0) j.set("slot", c.slot);
  j.set("start_ms", c.start_ms);
  if (c.end_ms >= 0) j.set("end_ms", c.end_ms);
  if (!c.label.empty()) j.set("label", c.label);
  return j;
}

Json follow_json(const CueFollowProposal& p) {
  Json j = Json::object();
  Json moves = Json::array(), off = Json::array(), same = Json::array();
  for (const CueMove& m : p.moves) {
    Json o = Json::object();
    o.set("from", cue_json(m.before));
    o.set("to", cue_json(m.after));
    o.set("beats", m.beats);
    moves.push_back(std::move(o));
  }
  for (const Cue& c : p.off_grid) off.push_back(cue_json(c));
  for (const Cue& c : p.unchanged) same.push_back(cue_json(c));
  j.set("moves", std::move(moves));
  j.set("off_grid", std::move(off));
  j.set("unchanged", std::move(same));
  j.set("no_grid", p.no_grid);
  j.set("tolerance_ms", p.tolerance_ms);
  return j;
}

Json strip_json(const StripResult& r) {
  Json j = Json::object();
  j.set("dry_run", r.dry_run);
  j.set("batch_id", r.batch_id);
  j.set("tracks", r.tracks);
  j.set("tracks_changed", r.tracks_changed);
  Json cleared = Json::object();
  for (const auto& [k, v] : r.cleared) cleared.set(k, v);
  j.set("cleared", std::move(cleared));
  j.set("skipped_grid_locked", r.skipped_grid_locked);
  j.set("skipped_user_key", r.skipped_user_key);
  Json notes = Json::array();
  for (const std::string& n : r.notes) notes.push_back(n);
  j.set("notes", std::move(notes));
  return j;
}

Json duplicate_json(const DuplicateGroup& g) {
  Json j = Json::object();
  j.set("kind", g.kind);
  j.set("confidence", g.confidence);
  Json members = Json::array();
  for (const DuplicateMember& m : g.members) {
    Json o = track_json(m.row);
    o.set("file_size", m.file_size);
    o.set("sample_rate", m.sample_rate);
    o.set("lossless", m.lossless);
    o.set("cue_count", m.cue_count);
    o.set("has_grid", m.has_grid);
    members.push_back(std::move(o));
  }
  j.set("members", std::move(members));
  Json diff = Json::array();
  for (const std::string& d : g.differences) diff.push_back(d);
  j.set("differences", std::move(diff));
  j.set("suggested_keep", g.suggested_keep);
  j.set("suggestion_reason", g.suggestion_reason);
  return j;
}

Json merge_json(const MergeResult& m) {
  Json j = Json::object();
  j.set("dry_run", m.dry_run);
  j.set("batch_id", m.batch_id);
  j.set("kept", m.kept);
  Json removed = Json::array();
  for (const std::int64_t id : m.removed) removed.push_back(id);
  j.set("removed", std::move(removed));
  j.set("playlists_repointed", m.playlists_repointed);
  j.set("playlists_already_had", m.playlists_already_had);
  j.set("hot_cues_copied", m.hot_cues_copied);
  j.set("memory_cues_copied", m.memory_cues_copied);
  j.set("loops_copied", m.loops_copied);
  j.set("grid_copied", m.grid_copied);
  j.set("history_moved", m.history_moved);
  j.set("mix_links_moved", m.mix_links_moved);
  Json filled = Json::array();
  for (const std::string& f : m.fields_filled) filled.push_back(f);
  j.set("fields_filled", std::move(filled));
  return j;
}

Json mix_link_json(const MixLink& l) {
  Json j = Json::object();
  j.set("id", l.id);
  j.set("from_track", l.from_track);
  j.set("from_ms", l.from_ms);
  j.set("to_track", l.to_track);
  j.set("to_ms", l.to_ms);
  if (l.transition_beats) j.set("transition_beats", *l.transition_beats);
  if (l.transition_type) j.set("transition_type", *l.transition_type);
  if (!l.label.empty()) j.set("label", l.label);
  j.set("created_at", l.created_at);
  return j;
}

// Fills the fields of a link from a request body (only what is given). Returns an error text, empty when fine.
std::string mix_link_from_body(const Json& b, MixLink& l) {
  if (b["from_track"].is_number()) l.from_track = b["from_track"].as_int();
  if (b["to_track"].is_number()) l.to_track = b["to_track"].as_int();
  if (b["from_ms"].is_number()) l.from_ms = b["from_ms"].as_double();
  if (b["to_ms"].is_number()) l.to_ms = b["to_ms"].as_double();
  if (b["label"].is_string()) l.label = b["label"].as_string();
  if (b.contains("transition_beats")) {
    if (b["transition_beats"].is_null()) l.transition_beats.reset();
    else if (b["transition_beats"].is_number() && b["transition_beats"].as_double() >= 0 && b["transition_beats"].as_double() <= 128) l.transition_beats = b["transition_beats"].as_double();
    else return "transition_beats must be 0..128 or null";
  }
  if (b.contains("transition_type")) {
    if (b["transition_type"].is_null()) l.transition_type.reset();
    else {
      const std::string t = b["transition_type"].is_string() ? b["transition_type"].as_string() : std::string();
      if (t != "crossfade" && t != "cut" && t != "fade" && t != "echo_out") return "transition_type must be crossfade, cut, fade, echo_out or null";
      l.transition_type = t;
    }
  }
  if (l.from_track <= 0 || l.to_track <= 0 || l.from_track == l.to_track) return "from_track and to_track must be two different tracks";
  if (!std::isfinite(l.from_ms) || !std::isfinite(l.to_ms) || l.from_ms < 0 || l.to_ms < 0) return "from_ms and to_ms must be positions in milliseconds";
  return {};
}

bool id_list(const Json& v, std::vector<std::int64_t>& out) {
  if (!v.is_array()) return false;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (!v.at(i).is_number()) return false;
    out.push_back(v.at(i).as_int());
  }
  return true;
}

Response undo_response(const UndoResult& u) {
  if (u.status == UndoStatus::NotFound) return Response::error(404, "nothing to undo there (no such edit, or its track is gone)");
  if (u.status == UndoStatus::Conflict) return Response::error(409, "a field changed again since this edit; nothing was undone");
  return Response::ok(undo_json(u));
}

bool opt_bool(const Json& body, const char* key, bool def) { return body[key].is_bool() ? body[key].as_bool() : def; }

// ---- cue suggestions ----
bool check_suggest_options(SuggestOptions& o, std::string& error) {
  if (o.max_hot < 0 || o.max_hot > 8) {
    error = "max must be 0 to 8";
    return false;
  }
  if (o.mix_out_bars != 8 && o.mix_out_bars != 16 && o.mix_out_bars != 32) {
    error = "mix_out_bars must be 8, 16 or 32";
    return false;
  }
  return true;
}
bool suggest_options_from_query(const Request& r, SuggestOptions& o, std::string& error) {
  const auto get = [&](const char* k) { return r.query.count(k) ? r.query.at(k) : std::string(); };
  if (!get("max").empty()) o.max_hot = std::atoi(get("max").c_str());
  if (!get("mix_out_bars").empty()) o.mix_out_bars = std::atoi(get("mix_out_bars").c_str());
  o.habit = get("habit") == "1" || get("habit") == "true";
  o.memory = get("memory") == "1" || get("memory") == "true";
  return check_suggest_options(o, error);
}
bool suggest_options_from_body(const Json& b, SuggestOptions& o, std::string& error) {
  if (b["max"].is_number()) o.max_hot = static_cast<int>(b["max"].as_int());
  if (b["mix_out_bars"].is_number()) o.mix_out_bars = static_cast<int>(b["mix_out_bars"].as_int());
  o.habit = opt_bool(b, "habit", false);
  o.memory = opt_bool(b, "memory", false);
  return check_suggest_options(o, error);
}
Json suggestion_json(const CueSuggestion& c) {
  Json j = Json::object();
  j.set("position_ms", c.position_ms);
  j.set("type", c.type);
  j.set("name", c.name);
  j.set("color", c.color);
  j.set("color_rgb", static_cast<std::int64_t>(kPalette[std::clamp(c.color, 0, kPaletteSize - 1)]));
  j.set("rank", c.rank);
  j.set("memory", c.memory);
  j.set("confidence", c.confidence);
  j.set("bar", c.bar);
  j.set("anchor_ms", c.anchor_ms);
  j.set("bar_shift", c.bar_shift);
  return j;
}
Json apply_json(const ApplyResult& res) {
  Json j = Json::object(), written = Json::array(), skipped = Json::array();
  j.set("analysed", res.has_phrases);
  if (!res.note.empty()) j.set("note", res.note);
  for (const AppliedCue& a : res.written) {
    Json o = suggestion_json(a.suggestion);
    o.set("kind", a.slot >= 0 ? "hot" : "memory");
    if (a.slot >= 0) o.set("slot", a.slot + 1);  // 1-based like the rest of the API
    written.push_back(std::move(o));
  }
  for (const SkippedCue& k : res.skipped) {
    Json o = suggestion_json(k.suggestion);
    o.set("reason", k.reason);
    skipped.push_back(std::move(o));
  }
  j.set("written", std::move(written));
  j.set("skipped", std::move(skipped));
  return j;
}

Json export_stats_json(const InterchangeExportStats& s) {
  Json j = Json::object();
  j.set("tracks", s.tracks);
  j.set("playlists", s.playlists);
  j.set("cues", s.cues);
  j.set("files", s.files);
  return j;
}

Json import_stats_json(const InterchangeStats& s) {
  Json j = Json::object();
  j.set("dry_run", s.dry_run);
  for (const auto& [k, v] : std::initializer_list<std::pair<const char*, int>>{
           {"tracks_in_file", s.tracks_in_file}, {"tracks_added", s.tracks_added},
           {"tracks_matched_by_path", s.tracks_matched_by_path}, {"tracks_matched_by_name", s.tracks_matched_by_name},
           {"tracks_skipped", s.tracks_skipped}, {"tracks_missing_file", s.tracks_missing_file},
           {"tracks_updated", s.tracks_updated}, {"cues", s.cues}, {"grids", s.grids}, {"marks_ignored", s.marks_ignored},
           {"playlists_created", s.playlists_created}, {"playlists_merged", s.playlists_merged},
           {"playlist_entries_added", s.playlist_entries_added},
           {"playlist_entries_unresolved", s.playlist_entries_unresolved}, {"history_entries", s.history_entries}})
    j.set(k, v);
  Json missing = Json::array();
  for (const std::string& p : s.missing_paths) missing.push_back(p);
  j.set("missing_paths", std::move(missing));
  Json warnings = Json::array();
  for (const std::string& w : s.warnings) warnings.push_back(w);
  j.set("warnings", std::move(warnings));
  return j;
}

bool absolute_path(const std::string& p) { return !p.empty() && (p[0] == '/' || (p.size() > 2 && p[1] == ':')); }

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
  if (s == "comment") return SortKey::Comment;
  if (s == "color") return SortKey::Color;
  if (s == "format") return SortKey::Format;
  if (s == "bitrate") return SortKey::Bitrate;
  if (s == "plays") return SortKey::PlayCount;
  if (s == "played") return SortKey::LastPlayed;
  return SortKey::Default;
}

const char* const kRuleFields[] = {"text", "bpm", "key", "genre", "rating", "color", "added_days", "plays", "playlist", "missing"};

Json smart_json(const SmartPlaylist& p) {
  Json j = Json::object();
  j.set("id", p.id);
  j.set("name", p.name);
  j.set("match", p.match_all ? "all" : "any");
  Json rules = Json::array();
  for (const SmartRule& r : p.rules) {
    if (static_cast<int>(r.field) > 9) continue;
    Json e = Json::object();
    e.set("field", kRuleFields[static_cast<int>(r.field)]);
    e.set("not", r.negate);
    switch (r.field) {
      case SmartRule::Field::Text: case SmartRule::Field::Genre: case SmartRule::Field::Key: e.set("text", r.text); break;
      case SmartRule::Field::Bpm: e.set("min", r.lo); if (r.hi > 0) e.set("max", r.hi); break;
      case SmartRule::Field::Rating: e.set("min", r.lo); break;
      case SmartRule::Field::Color: e.set("color", static_cast<std::int64_t>(r.lo)); break;
      case SmartRule::Field::AddedDays: e.set("days", r.lo); break;
      case SmartRule::Field::Plays: e.set("min", r.lo); if (r.hi >= 0) e.set("max", r.hi); break;
      case SmartRule::Field::InPlaylist: e.set("playlist_id", r.id); break;
      default: break;
    }
    rules.push_back(std::move(e));
  }
  j.set("rules", std::move(rules));
  return j;
}

// Fills p from a request body: any of name, match ("all" | "any"), rules [ {field, not, text, min, max, days, color, playlist_id} ].
bool smart_from_json(const Json& body, SmartPlaylist& p, std::string& error) {
  if (body["name"].is_string()) p.name = body["name"].as_string().substr(0, 120);
  if (body["match"].is_string()) {
    const std::string m = body["match"].as_string();
    if (m != "all" && m != "any") {
      error = "match must be \"all\" or \"any\"";
      return false;
    }
    p.match_all = m == "all";
  }
  if (body.contains("rules")) {
    if (!body["rules"].is_array() || body["rules"].size() > 20) {
      error = "rules must be an array of at most 20 rules";
      return false;
    }
    std::vector<SmartRule> rules;
    for (std::size_t i = 0; i < body["rules"].size(); ++i) {
      const Json& e = body["rules"].at(i);
      SmartRule r;
      int field = -1;
      for (int f = 0; f < 10; ++f)
        if (e["field"].is_string() && e["field"].as_string() == kRuleFields[f]) field = f;
      if (field < 0) {
        error = "rule field must be one of text, bpm, key, genre, rating, color, added_days, plays, playlist, missing";
        return false;
      }
      r.field = static_cast<SmartRule::Field>(field);
      r.negate = e["not"].as_bool(false);
      if (e["text"].is_string()) r.text = e["text"].as_string().substr(0, 200);
      switch (r.field) {
        case SmartRule::Field::Bpm: r.lo = e["min"].as_double(0); r.hi = e["max"].as_double(0); break;
        case SmartRule::Field::Rating: r.lo = std::clamp(e["min"].as_double(1), 0.0, 5.0); break;
        case SmartRule::Field::Color: r.lo = std::clamp(e["color"].as_double(0), 0.0, 16777215.0); break;
        case SmartRule::Field::AddedDays: r.lo = std::clamp(e["days"].as_double(30), 0.0, 36500.0); break;
        case SmartRule::Field::Plays: r.lo = e["min"].as_double(0); r.hi = e.contains("max") ? e["max"].as_double(-1) : -1; break;
        case SmartRule::Field::InPlaylist: r.id = e["playlist_id"].as_int(0); break;
        default: break;
      }
      rules.push_back(std::move(r));
    }
    p.rules = std::move(rules);
  }
  return true;
}

// A scheduled control change, kept for listing; the engine holds the real entry.
struct Scheduled {
  std::uint64_t id = 0;               // first engine schedule id
  std::vector<std::uint64_t> engine_ids;
  std::string control;
  bool has_value = false;
  double value = 0;
  std::string when;                   // human-readable trigger
};

}  // namespace

struct ApiBridge::Impl {
  ApiHost& host;
  std::string data_dir;
  std::mutex lib_mutex;
  Library lib;
  HabitCache habit_cache;
  api::TokenStore tokens;
  std::unique_ptr<api::ApiServer> server;
  std::string description = "API off";

  std::atomic<bool> quit{false};
  std::thread ticker;
  std::mutex sched_mutex;
  std::vector<Scheduled> scheduled;

  Impl(ApiHost& h, const std::string& dir) : host(h), data_dir(dir), lib(dir + "/library.db"), tokens(dir + "/api-tokens.json") {}

  // ---- JSON views --------------------------------------------------------------------
  // Stems of a deck: availability (cache and engine), whether they play, per-stem mute / solo / isolate / level / meter.
  Json stems_json(int index) {
    const DeckView v = host.api_engine().deck(index);
    static const char* const names[kStemCount] = {"vocals", "drums", "bass", "other"};
    Json j = Json::object();
    const char* state = v.stems_state == 2 ? "attached" : v.stems_state == 1 ? "loading" : v.stems_state == 3 ? "failed" : "none";
    j.set("state", state);
    j.set("attached", v.stems_state == 2);
    j.set("active", v.stems_state == 2 && v.stems_on != 0);
    j.set("mix", static_cast<double>(v.stems_mix));
    if (StemService* st = host.api_stems(); st && v.loaded && v.track_id != 0) j.set("cached", !st->stems_file(v.track_id).empty());
    Json arr = Json::object();
    for (int k = 0; k < kStemCount; ++k) {
      Json e = Json::object();
      e.set("mute", (v.stem_mute >> k & 1u) != 0);
      e.set("solo", (v.stem_solo >> k & 1u) != 0);
      e.set("isolate", (v.stem_isolate >> k & 1u) != 0);
      e.set("level", static_cast<double>(v.stem_level[k]));
      e.set("meter", static_cast<double>(v.stem_meter[k]));
      e.set("fx", (v.stem_fx >> k & 1u) != 0);
      arr.set(names[k], std::move(e));
    }
    j.set("stems", std::move(arr));
    return j;
  }

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
    {  // key sync: see POST /v1/decks/{deck}/key_sync
      Json ks = Json::object();
      if (v.key_index >= 0) ks.set("key", keymath::camelot(v.key_index));
      else ks.set("key", Json());
      ks.set("follow", v.key_sync_follow != 0);
      ks.set("mode", static_cast<double>(v.key_sync_mode));
      ks.set("fine", v.key_sync_fine != 0);
      ks.set("limit", static_cast<double>(v.key_sync_limit));
      static const char* const names[] = {"none", "ok", "too_far", "no_own_key", "no_reference_key", "leader"};
      ks.set("result", std::string(names[v.key_sync_result < 6 ? v.key_sync_result : 0]));
      ks.set("pitch", static_cast<double>(v.key_pitch));
      j.set("key_sync", std::move(ks));
    }
    j.set("quantize", v.quantize);
    j.set("slip", v.slip);
    j.set("reverse", v.reverse);
    j.set("auto_gain_db", static_cast<double>(v.auto_gain_db));
    j.set("stems", stems_json(index));
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
    j.set("mic_on", m.mic_on);
    j.set("mic_present", m.mic_present);
    j.set("mic_level", static_cast<double>(m.mic_level));
    j.set("mic_peak", static_cast<double>(m.mic_peak));
    j.set("mic_talkover", static_cast<double>(m.mic_talkover));
    j.set("mic_ducking", m.mic_ducking);
    return j;
  }

  // ---- track preparation: grid and cue editing ---------------------------------------------------------
  // body: {"value": n, "value2": n, "slot": n, "text": "...", "color": 0xRRGGBB number or null}
  Response prep_response(int deck, const std::string& action, int slot_from_name, const Json& body) {
    PrepArgs a;
    const Json& value = body["value"];
    if (!value.is_null() && !value.is_number() && !value.is_bool()) return Response::error(400, "value must be a number");
    a.has_value = !value.is_null();
    a.value = value.is_bool() ? (value.as_bool() ? 1.0 : 0.0) : value.as_double();
    a.value2 = body["value2"].as_double();
    a.slot = slot_from_name > 0 ? slot_from_name : static_cast<int>(std::clamp<std::int64_t>(body["slot"].as_int(0), 0, 100000));
    if (body.contains("color")) {
      const Json& c = body["color"];
      if (!c.is_null() && !(c.is_number() && c.as_double() >= 0 && c.as_double() <= 16777215)) return Response::error(400, "color must be a number 0..16777215 or null");
      a.has_color = true;
      a.color = c.is_null() ? -1 : static_cast<int>(c.as_int());
    }
    if (body["text"].is_string()) {
      a.has_text = true;
      a.text = body["text"].as_string().substr(0, 80);
    }
    std::string error;
    if (!host.api_prep(deck, action, a, error)) return Response::error(409, error);
    Json j = Json::object();
    j.set("action", action);
    return Response::ok(std::move(j));
  }

  Response prep_state(int deck, const char* part) {
    const std::string text = host.api_deck_prep_json(deck);
    if (text.empty()) return Response::error(409, "no track loaded on that deck");
    Json all = Json::parse(text);
    if (!part) return Response::ok(std::move(all));
    Json j = Json::object();
    j.set("track_id", all["track_id"]);
    if (std::string(part) == "grid") {
      for (const char* k : {"grid", "grid_locked", "grid_edit", "undo_depth", "metronome"}) j.set(k, all[k]);
    } else {
      for (const char* k : {"memory_cues", "hot_cues", "loop"}) j.set(k, all[k]);
    }
    return Response::ok(std::move(j));
  }

  // ---- controls and scheduling ----------------------------------------------------------
  // body: {"value": n, "in_ms": n | "at_unix_ms": n | "when": {"deck": 1, "seconds": 200.0}}
  Response control(const std::string& name, const Json& body) {
    const Json& value = body["value"];
    if (!value.is_null() && !value.is_number() && !value.is_bool()) return Response::error(400, "value must be a number");
    const bool has_value = !value.is_null();
    const double v = value.is_bool() ? (value.as_bool() ? 1.0 : 0.0) : value.as_double();

    {  // controls that edit the loaded track are the application's, not the engine's
      int pdeck = 0, pslot = 0;
      std::string paction;
      if (parse_prep_control(name, pdeck, paction, pslot)) return prep_response(pdeck, paction, pslot, body);
    }
    {  // lighting controls are the lighting module's
      std::string action;
      if (parse_lighting_control(name, action)) {
        LightingBridge* lb = host.api_lighting();
        std::string lerror;
        if (!lb || !lb->control(action, has_value, v, lerror)) return Response::error(409, lerror.empty() ? "lighting is not available" : lerror);
        Json j = Json::object();
        j.set("control", name);
        return Response::ok(std::move(j));
      }
    }
    {  // automix and queue controls are the application's
      std::string action;
      if (parse_automix_control(name, action)) {
        std::string aerror;
        if (!host.api_automix_control(action, has_value, v, aerror)) return Response::error(409, aerror);
        Json j = Json::object();
        j.set("control", name);
        return Response::ok(std::move(j));
      }
    }
    if (is_chain_control(name)) {  // effect chain edits: structural, applied at once, never scheduled
      if (body["in_ms"].is_number() || body["at_unix_ms"].is_number() || body["when"].is_object() || body["at_beat"].is_object() ||
          body["in_beats"].is_object())
        return Response::error(400, "effect chain edits cannot be scheduled");
      std::string out_text, cerror;
      if (!apply_chain_control(host.api_engine(), name, has_value, v, body["text"].as_string(), out_text, cerror)) return Response::error(400, cerror);
      Json j = Json::object();
      j.set("control", name);
      const auto at = name.rfind(".chain.");
      const std::string verb = at == std::string::npos ? std::string() : name.substr(at + 7);
      if (verb.rfind("insert", 0) == 0) j.set("id", static_cast<std::int64_t>(std::atoll(out_text.c_str())));
      if (verb == "save") {
        Json preset = Json::parse(out_text);
        j.set("preset", std::move(preset));
      }
      return Response::ok(std::move(j));
    }
    std::vector<Command> commands;
    std::string error;
    if (!build_control(host.api_engine(), name, has_value, v, commands, error)) return Response::error(400, error);

    // Optional trigger: wall clock, a deck position, or a beat of a deck's grid. Executed by the
    // engine on the exact output sample.
    IEngine& engine = host.api_engine();
    bool scheduled_request = false;
    Schedule when;
    std::string when_text;
    const auto deck_of = [&](const Json& j, int& deck) {
      const std::int64_t d = j["deck"].as_int(0);
      if (d < 1 || d > kMaxDecks) return false;
      deck = static_cast<int>(d - 1);
      return true;
    };
    if (body["in_ms"].is_number() || body["at_unix_ms"].is_number()) {
      const double delay_ms = body["in_ms"].is_number() ? std::max(0.0, body["in_ms"].as_double())
                                                        : static_cast<double>(body["at_unix_ms"].as_int() - unix_ms());
      const auto at = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<std::int64_t>(std::max(0.0, delay_ms) * 1000.0));
      when = Schedule::at_clock_frame(engine.clock_at_wall(at));
      when_text = "clock";
      scheduled_request = true;
    } else if (body["when"].is_object()) {
      int deck = 0;
      if (!deck_of(body["when"], deck) || !body["when"]["seconds"].is_number()) return Response::error(400, "when needs deck (1..4) and seconds");
      const DeckView dv = engine.deck(deck);
      if (!dv.loaded) return Response::error(409, "that deck has no track");
      when = Schedule::at_deck_frame(deck, body["when"]["seconds"].as_double() * dv.sample_rate, true);
      when_text = "deck " + std::to_string(deck + 1) + " at " + std::to_string(body["when"]["seconds"].as_double()) + " s";
      scheduled_request = true;
    } else if (body["at_beat"].is_object() || body["in_beats"].is_object()) {
      const bool relative = body["in_beats"].is_object();
      const Json& b = relative ? body["in_beats"] : body["at_beat"];
      int deck = 0;
      const char* field = relative ? "beats" : "beat";
      if (!deck_of(b, deck) || !b[field].is_number()) return Response::error(400, "give deck (1..4) and a beat number");
      if (!engine.deck(deck).loaded) return Response::error(409, "that deck has no track");
      when = relative ? Schedule::beats_from_now(deck, b[field].as_double(), b["bar"].as_bool(false))
                      : Schedule::at_deck_beat(deck, b[field].as_double(), b["bar"].as_bool(false), false);
      when_text = "deck " + std::to_string(deck + 1) + (relative ? " in " : " at beat ") + std::to_string(b[field].as_double()) + (relative ? " beats" : "");
      scheduled_request = true;
    }
    if (scheduled_request)
      for (const Command& c : commands)
        if (c.op == Op::FxSelect) return Response::error(400, "choosing an effect for a slot builds an effect and cannot be scheduled; insert a unit with deckN.chain.insert instead, or send it now");
    Json j = Json::object();
    j.set("control", name);
    if (!scheduled_request) {
      for (const Command& c : commands) engine.send(c);
      return Response::ok(std::move(j));
    }
    Scheduled s;
    s.control = name;
    s.has_value = has_value;
    s.value = v;
    s.when = when_text;
    for (const Command& c : commands) {
      const std::uint64_t id = engine.schedule(c, when);
      if (id == 0) {
        for (const auto done : s.engine_ids) engine.cancel(done);
        return Response::error(429, "the schedule is full or this control cannot be scheduled");
      }
      s.engine_ids.push_back(id);
    }
    if (s.engine_ids.empty()) return Response::ok(std::move(j));  // nothing to do (already in that state)
    s.id = s.engine_ids.front();
    j.set("scheduled_id", s.id);
    {
      std::lock_guard<std::mutex> lock(sched_mutex);
      scheduled.push_back(std::move(s));
    }
    Response r = Response::ok(std::move(j));
    r.status = 202;
    return r;
  }

  // Called for ScheduledFired / ScheduledDropped engine events (any thread).
  void engine_event(const EngineEvent& e) {
    if (e.type != EventType::ScheduledFired && e.type != EventType::ScheduledDropped) return;
    Json out;
    {
      std::lock_guard<std::mutex> lock(sched_mutex);
      for (auto it = scheduled.begin(); it != scheduled.end(); ++it) {
        const auto pos = std::find(it->engine_ids.begin(), it->engine_ids.end(), e.sched_id);
        if (pos == it->engine_ids.end()) continue;
        it->engine_ids.erase(pos);
        if (!it->engine_ids.empty()) return;  // wait for the last command of this control
        out = Json::object();
        out.set("type", "scheduled");
        out.set("scheduled_id", it->id);
        out.set("control", it->control);
        out.set("ok", e.type == EventType::ScheduledFired);
        out.set("clock_frame", e.clock_frame);
        if (e.type == EventType::ScheduledDropped)
          out.set("error", e.slot == 1 ? "deck unloaded" : e.slot == 2 ? "schedule full" : "no track or beat grid");
        scheduled.erase(it);
        break;
      }
    }
    if (!out.is_null() && server) server->publish("events", out);
  }

  // Publishes deck and mixer state, and events for changes worth reacting to.
  void tick_main() {
    struct Seen {
      std::int64_t track_id = 0;
      bool playing = false;
    } seen[kMaxDecks];
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));  // state at 10 Hz
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


  // ---- effect chains ------------------------------------------------------------------------------------------------
  Json chain_json(int target) {
    const FxChainInfo c = host.api_engine().fx_chain(target);
    Json j = Json::object();
    j.set("target", fx_target_name(target));
    j.set("exists", c.exists != 0);
    j.set("bypass", c.bypass != 0);
    j.set("wet", static_cast<double>(c.wet));
    j.set("count", c.count);
    j.set("max_units", kFxChainMax);
    j.set("rev", static_cast<std::int64_t>(c.rev));
    Json units = Json::array();
    for (int i = 0; i < c.count && i < kFxChainMax; ++i) {
      const FxUnitInfo& u = c.unit[i];
      const EffectInfo* e = effect_info(u.kind);
      Json o = Json::object();
      o.set("id", u.id);
      o.set("pos", i);
      o.set("effect", e ? e->slug : "");
      o.set("number", static_cast<int>(u.kind));
      o.set("name", e ? e->name : "");
      o.set("on", u.on != 0);
      o.set("wet", static_cast<double>(u.wet));
      o.set("param", static_cast<double>(u.param));
      o.set("param2", static_cast<double>(u.param2));
      o.set("param3", static_cast<double>(u.param3));
      o.set("beats", static_cast<double>(u.beats));
      o.set("seconds", u.seconds);
      if (u.legacy_slot >= 0) o.set("slot", u.legacy_slot + 1);
      units.push_back(std::move(o));
    }
    j.set("units", std::move(units));
    return j;
  }

  static bool effect_from_json(const Json& v, EffectKind& out) {
    if (v.is_string()) {
      out = effect_from_slug(v.as_string());
      return out != EffectKind::None;
    }
    if (v.is_integer()) {
      const std::int64_t n = v.as_int();
      if (n < 1 || n >= static_cast<std::int64_t>(EffectKind::Count)) return false;
      out = static_cast<EffectKind>(n);
      return true;
    }
    return false;
  }

  bool unit_exists(int target, int id) {
    const FxChainInfo c = host.api_engine().fx_chain(target);
    for (int i = 0; i < c.count && i < kFxChainMax; ++i)
      if (c.unit[i].id == id) return true;
    return false;
  }

  // Resolves {target}; sets `bad` to a response when it does not name a chain.
  bool chain_target(const Request& r, int& target, Response& bad) {
    if (!parse_fx_target(r.params.at("target"), target) || !fx_target_valid(target)) {
      bad = Response::error(404, "no such chain; target is deck1..deck4, master, mic, sampler or deckN.stemM (M 1..8)");
      return false;
    }
    return true;
  }
  bool chain_edit(const FxChainEdit& e, FxChainResult& res, Response& bad) {
    if (host.api_engine().fx_chain_edit(e, res)) return true;
    bad = Response::error(409, res.error.empty() ? "the engine refused the chain edit" : res.error);
    return false;
  }


  // ---- play queue and automix -------------------------------------------------------------------------------------------
  static Json num_or_null(const std::optional<double>& v) { return v ? Json(*v) : Json(); }
  static Json points_json(const AutomixPoints& p) {
    Json j = Json::object();
    j.set("mix_in_ms", num_or_null(p.mix_in_ms));
    j.set("mix_out_ms", num_or_null(p.mix_out_ms));
    j.set("transition_beats", num_or_null(p.transition_beats));
    j.set("transition_type", p.transition_type ? Json(*p.transition_type) : Json());
    j.set("max_play_ms", num_or_null(p.max_play_ms));
    return j;
  }
  // Reads the five fields of `b` into `p`. With `merge` a missing key keeps the old value; otherwise it is unset. null always unsets.
  static bool parse_points(const Json& b, AutomixPoints& p, bool merge, std::string& error) {
    const auto number = [&](const char* key, std::optional<double>& out, double lo, double hi) {
      if (!b.contains(key)) {
        if (!merge) out.reset();
        return true;
      }
      const Json& v = b[key];
      if (v.is_null()) {
        out.reset();
        return true;
      }
      if (!v.is_number() || !(v.as_double() >= lo && v.as_double() <= hi)) {
        error = std::string(key) + " must be a number " + std::to_string(static_cast<long long>(lo)) + ".." + std::to_string(static_cast<long long>(hi)) + " or null";
        return false;
      }
      out = v.as_double();
      return true;
    };
    if (!number("mix_in_ms", p.mix_in_ms, 0, 1e9) || !number("mix_out_ms", p.mix_out_ms, 0, 1e9) ||
        !number("transition_beats", p.transition_beats, 1, 64) || !number("max_play_ms", p.max_play_ms, 1000, 1e9))
      return false;
    if (b.contains("transition_type")) {
      const Json& v = b["transition_type"];
      if (v.is_null()) {
        p.transition_type.reset();
      } else if (v.is_string() && AutomixSettings::valid_type(v.as_string())) {
        p.transition_type = v.as_string();
      } else {
        error = "transition_type must be crossfade, cut, fade, echo_out or null";
        return false;
      }
    } else if (!merge) {
      p.transition_type.reset();
    }
    return true;
  }
  Json entry_json(const QueueEntry& e) {
    Json j = Json::object();
    j.set("entry_id", e.id);
    j.set("track_id", e.track_id);
    j.set("play_next", e.play_next);
    j.set("overrides", points_json(e.overrides));
    std::lock_guard<std::mutex> lock(lib_mutex);
    if (const auto row = lib.track(e.track_id)) {
      j.set("title", row->title);
      j.set("artist", row->artist);
      j.set("duration_ms", row->duration_ms);
    }
    return j;
  }
  Json queue_json(AutomixService& a) {
    PlayQueue& q = a.queue();
    Json j = Json::object();
    j.set("revision", static_cast<std::int64_t>(q.revision()));
    if (const auto c = q.current()) j.set("current", entry_json(*c));
    else j.set("current", Json());
    if (const auto p = q.pending()) j.set("pending", entry_json(*p));
    else j.set("pending", Json());
    Json up = Json::array();
    for (const QueueEntry& e : q.upcoming()) up.push_back(entry_json(e));
    j.set("upcoming", std::move(up));
    const QueuePolicy pol = q.policy();
    j.set("duplicates", pol.skip_recent ? "skip_recent" : "allow");
    j.set("duplicates_window", pol.window);
    return j;
  }
  static Json status_json(const AutomixStatus& s) {
    Json j = Json::object();
    j.set("state", s.state);
    j.set("running", s.running);
    j.set("paused", s.paused);
    j.set("resume_available", s.resume_available);
    j.set("current_deck", s.current_deck >= 0 ? Json(s.current_deck + 1) : Json());
    j.set("next_deck", s.next_deck >= 0 ? Json(s.next_deck + 1) : Json());
    j.set("current_track_id", s.current_track ? Json(s.current_track) : Json());
    j.set("next_track_id", s.next_track ? Json(s.next_track) : Json());
    j.set("current_entry_id", s.current_entry ? Json(s.current_entry) : Json());
    j.set("next_entry_id", s.next_entry ? Json(s.next_entry) : Json());
    j.set("seconds_to_transition", s.seconds_to_transition >= 0 ? Json(s.seconds_to_transition) : Json());
    j.set("transition_type", s.transition_type.empty() ? Json() : Json(s.transition_type));
    j.set("transition_beats", s.transition_beats);
    j.set("synced", s.synced);
    j.set("planned_mix_out_ms", s.planned_mix_out_ms >= 0 ? Json(s.planned_mix_out_ms) : Json());
    j.set("note", s.note);
    j.set("queue_revision", static_cast<std::int64_t>(s.queue_revision));
    return j;
  }
  static Json settings_json(const AutomixSettings& st) {
    Json j = Json::object();
    j.set("transition_type", st.transition_type);
    j.set("unsynced_type", st.unsynced_type);
    j.set("transition_beats", st.transition_beats);
    j.set("mix_out_bars", st.mix_out_bars);
    j.set("max_sync_gap_pct", st.max_sync_gap_pct);
    j.set("bass_swap", st.bass_swap);
    j.set("override_mode", st.override_mode);
    j.set("on_empty", st.on_empty);
    j.set("fade_seconds", st.fade_seconds);
    Json endless = Json::object();
    endless.set("enabled", st.endless);
    endless.set("source", st.endless_source);
    endless.set("keep", st.endless_keep);
    endless.set("order", st.endless_order);
    j.set("endless", std::move(endless));
    return j;
  }
  // Applies the keys of `b` that are present; false with an error text for a bad value.
  static bool apply_settings(const Json& b, AutomixSettings& st, std::string& error) {
    const auto num = [&](const char* k, double& v, double lo, double hi) {
      if (!b.contains(k)) return true;
      if (!b[k].is_number() || b[k].as_double() < lo || b[k].as_double() > hi) {
        error = std::string(k) + " must be a number " + std::to_string(static_cast<long long>(lo)) + ".." + std::to_string(static_cast<long long>(hi));
        return false;
      }
      v = b[k].as_double();
      return true;
    };
    const auto choice = [&](const Json& src, const char* k, std::string& v, std::initializer_list<const char*> allowed) {
      if (!src.contains(k)) return true;
      for (const char* a : allowed)
        if (src[k].is_string() && src[k].as_string() == a) {
          v = a;
          return true;
        }
      error = std::string(k) + " has an unknown value";
      return false;
    };
    if (!choice(b, "transition_type", st.transition_type, {"crossfade", "cut", "fade", "echo_out"}) ||
        !choice(b, "unsynced_type", st.unsynced_type, {"cut", "fade", "echo_out"}) ||
        !choice(b, "override_mode", st.override_mode, {"yield", "hands_off"}) || !choice(b, "on_empty", st.on_empty, {"finish", "fade"}) ||
        !num("transition_beats", st.transition_beats, 1, 64) || !num("mix_out_bars", st.mix_out_bars, 0, 128) ||
        !num("max_sync_gap_pct", st.max_sync_gap_pct, 0, 50) || !num("fade_seconds", st.fade_seconds, 1, 60))
      return false;
    if (b.contains("bass_swap")) st.bass_swap = b["bass_swap"].as_bool(st.bass_swap);
    if (b["endless"].is_object()) {
      const Json& e = b["endless"];
      if (e.contains("enabled")) st.endless = e["enabled"].as_bool(st.endless);
      if (e["source"].is_string()) st.endless_source = e["source"].as_string().substr(0, 200);
      if (e["keep"].is_number()) st.endless_keep = static_cast<int>(std::clamp<std::int64_t>(e["keep"].as_int(), 1, 100));
      if (!choice(e, "order", st.endless_order, {"sequential", "shuffle"})) return false;
    }
    return true;
  }
  // Track ids named by an add / fill request body.
  bool request_tracks(const Json& b, std::vector<std::int64_t>& ids, std::string& error) {
    if (b["track_ids"].is_array()) {
      for (std::size_t i = 0; i < b["track_ids"].size() && i < 5000; ++i) ids.push_back(b["track_ids"].at(i).as_int());
    } else if (b["track_id"].is_number()) {
      ids.push_back(b["track_id"].as_int());
    } else {
      std::string source;
      if (b["playlist"].is_number()) source = "playlist:" + std::to_string(b["playlist"].as_int());
      else if (b["smart_playlist"].is_number()) source = "smart:" + std::to_string(b["smart_playlist"].as_int());
      else if (b["search"].is_string()) source = "search:" + b["search"].as_string();
      else {
        error = "give track_ids, track_id, playlist, smart_playlist or search";
        return false;
      }
      std::lock_guard<std::mutex> lock(lib_mutex);
      ids = resolve_track_source(lib, source);
    }
    if (b["limit"].is_number() && b["limit"].as_int() > 0 && ids.size() > static_cast<std::size_t>(b["limit"].as_int())) ids.resize(static_cast<std::size_t>(b["limit"].as_int()));
    return true;
  }

  void add_queue_routes() {
    auto& s = *server;
    const auto svc = [this]() { return host.api_automix(); };
    s.add_route("GET", "/v1/queue", "read", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      return Response::ok(queue_json(*a));
    });
    s.add_route("POST", "/v1/queue/add", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      std::vector<std::int64_t> ids;
      std::string error;
      if (!request_tracks(r.body, ids, error)) return Response::error(400, error);
      const std::string mode = r.body["mode"].is_string() ? r.body["mode"].as_string() : "end";
      const bool force = r.body["force"].as_bool(false);
      PlayQueue& q = a->queue();
      QueueAdd res;
      if (r.body["replace"].as_bool(false) || r.body["shuffle"].as_bool(false)) {
        if (mode != "end") return Response::error(400, "replace and shuffle go with mode end");
        res = q.fill(ids, r.body["shuffle"].as_bool(false), r.body["replace"].as_bool(false), force);
      } else if (mode == "end") {
        res = q.add_end(ids, force);
      } else if (mode == "next") {
        res = q.play_next(ids, force);
      } else if (mode == "at") {
        if (!r.body["position"].is_number()) return Response::error(400, "mode at needs position");
        res = q.insert(ids, static_cast<int>(std::clamp<std::int64_t>(r.body["position"].as_int(), 0, 100000)), force);
      } else {
        return Response::error(400, "mode must be end, next or at");
      }
      a->flush_events();
      Json j = Json::object();
      Json created = Json::array();
      for (const std::int64_t id : res.entry_ids) created.push_back(Json(id));
      j.set("entry_ids", std::move(created));
      j.set("added", static_cast<std::int64_t>(res.entry_ids.size()));
      j.set("skipped", res.skipped);
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/queue/move", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      if (!r.body["entry_id"].is_number() || !r.body["position"].is_number()) return Response::error(400, "give entry_id and position");
      if (!a->queue().move(r.body["entry_id"].as_int(), static_cast<int>(std::clamp<std::int64_t>(r.body["position"].as_int(), 0, 100000)))) return Response::error(404, "no such entry in the upcoming queue");
      a->flush_events();
      return Response::ok(queue_json(*a));
    });
    s.add_route("DELETE", "/v1/queue/entries/{id}", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      std::int64_t id = 0;
      if (!a) return Response::error(503, "automix is not available");
      if (!int_param(r, "id", id) || !a->queue().remove(id)) return Response::error(404, "no such entry in the upcoming queue");
      a->flush_events();
      return Response::ok();
    });
    s.add_route("PATCH", "/v1/queue/entries/{id}", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      std::int64_t id = 0;
      if (!a) return Response::error(503, "automix is not available");
      if (!int_param(r, "id", id)) return Response::error(404, "no such entry");
      PlayQueue& q = a->queue();
      QueueEntry found;
      bool have = false;
      for (const QueueEntry& e : q.upcoming())
        if (e.id == id) { found = e; have = true; }
      if (!have) {
        const auto c = q.current();
        if (c && c->id == id) { found = *c; have = true; }
      }
      if (!have) return Response::error(404, "no such entry");
      AutomixPoints p = found.overrides;
      std::string error;
      if (!parse_points(r.body, p, true, error)) return Response::error(400, error);
      q.set_overrides(id, p);
      if (r.body["position"].is_number()) q.move(id, static_cast<int>(std::clamp<std::int64_t>(r.body["position"].as_int(), 0, 100000)));
      a->flush_events();
      Json j = Json::object();
      j.set("entry_id", id);
      j.set("overrides", points_json(p));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/queue/clear", "control", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      a->queue().clear();
      a->flush_events();
      return Response::ok();
    });
    s.add_route("POST", "/v1/queue/shuffle", "control", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      a->queue().shuffle();
      a->flush_events();
      return Response::ok(queue_json(*a));
    });
    s.add_route("POST", "/v1/queue/skip", "control", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      const auto e = a->queue().skip();
      if (!e) return Response::error(409, "the queue is empty");
      a->flush_events();
      return Response::ok(entry_json(*e));
    });
    s.add_route("POST", "/v1/queue/back", "control", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      if (!a->queue().back()) return Response::error(409, "nothing was played before");
      a->skip();
      a->flush_events();
      return Response::ok(queue_json(*a));
    });
    s.add_route("GET", "/v1/queue/history", "read", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      const auto it = r.query.find("limit");
      const int limit = it == r.query.end() ? 50 : static_cast<int>(std::clamp<long>(std::atol(it->second.c_str()), 1, 500));
      Json items = Json::array();
      std::lock_guard<std::mutex> lock(lib_mutex);
      for (const PlayedEntry& h : a->queue().history(limit)) {
        Json e = Json::object();
        e.set("entry_id", h.entry_id);
        e.set("track_id", h.track_id);
        e.set("played_at_ms", h.played_at_ms);
        if (const auto row = lib.track(h.track_id)) {
          e.set("title", row->title);
          e.set("artist", row->artist);
        }
        items.push_back(std::move(e));
      }
      Json j = Json::object();
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/queue/settings", "read", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      const QueuePolicy pol = a->queue().policy();
      const AutomixSettings st = a->settings();
      Json j = Json::object();
      j.set("duplicates", pol.skip_recent ? "skip_recent" : "allow");
      j.set("duplicates_window", pol.window);
      j.set("endless", settings_json(st)["endless"]);
      return Response::ok(std::move(j));
    });
    s.add_route("PATCH", "/v1/queue/settings", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      QueuePolicy pol = a->queue().policy();
      if (r.body.contains("duplicates")) {
        const std::string d = r.body["duplicates"].as_string();
        if (d != "allow" && d != "skip_recent") return Response::error(400, "duplicates must be allow or skip_recent");
        pol.skip_recent = d == "skip_recent";
      }
      if (r.body["duplicates_window"].is_number()) pol.window = static_cast<int>(std::clamp<std::int64_t>(r.body["duplicates_window"].as_int(), 1, 1000));
      AutomixSettings st = a->settings();
      std::string error;
      Json only = Json::object();
      if (r.body["endless"].is_object()) only.set("endless", r.body["endless"]);
      if (!apply_settings(only, st, error)) return Response::error(400, error);
      a->queue().set_policy(pol);
      a->set_settings(st);
      a->flush_events();
      Json j = Json::object();
      j.set("duplicates", pol.skip_recent ? "skip_recent" : "allow");
      j.set("duplicates_window", pol.window);
      j.set("endless", settings_json(st)["endless"]);
      return Response::ok(std::move(j));
    });

    // ---- automix ----
    s.add_route("GET", "/v1/automix", "read", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      Json j = status_json(a->status());
      j.set("settings", settings_json(a->settings()));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/automix/{action}", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      const std::string action = r.params.at("action");
      std::string error;
      if (action != "start" && action != "stop" && action != "pause" && action != "resume" && action != "skip") return Response::error(404, "unknown action");
      const bool ok = action == "resume" ? host.api_automix_control("pause", true, 0, error) : host.api_automix_control(action, false, 0, error);
      if (!ok) return Response::error(409, error);
      return Response::ok(status_json(a->status()));
    });
    s.add_route("GET", "/v1/automix/settings", "read", [this, svc](const Request&) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      return Response::ok(settings_json(a->settings()));
    });
    s.add_route("PATCH", "/v1/automix/settings", "control", [this, svc](const Request& r) {
      AutomixService* a = svc();
      if (!a) return Response::error(503, "automix is not available");
      AutomixSettings st = a->settings();
      std::string error;
      if (!apply_settings(r.body, st, error)) return Response::error(400, error);
      a->set_settings(st);
      return Response::ok(settings_json(st));
    });
    s.add_route("GET", "/v1/library/tracks/{id}/automix", "library:read", [this, svc](const Request& r) {
      std::int64_t id = 0;
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
      Json j = points_json(lib.automix_points(id));
      AutomixService* a = svc();
      const TrackFacts f = read_track_facts(lib, id);
      const automix::ResolvedTrack rt = automix::resolve_track(f, AutomixPoints{}, a ? a->settings() : AutomixSettings{});
      Json eff = Json::object();  // what automix would use now (the queue entry's overrides not included)
      eff.set("mix_in_ms", rt.mix_in_ms);
      eff.set("mix_out_ms", rt.mix_out_ms);
      eff.set("mix_out_automatic", rt.mix_out_automatic);
      eff.set("last_sound_ms", rt.last_sound_ms);
      j.set("effective", std::move(eff));
      return Response::ok(std::move(j));
    });
    // ---- phrases and cue suggestions ----
    s.add_route("GET", "/v1/library/tracks/{id}/phrases", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
      const auto sp = lib.phrases(id);
      Json j = Json::object();
      j.set("track_id", id);
      j.set("analysed", sp.has_value() && !sp->sections.empty());
      if (!sp) return Response::ok(std::move(j));
      j.set("version", sp->version);
      j.set("gridded", sp->gridded);
      j.set("time_based", sp->time_based);
      j.set("origin_ms", sp->origin_ms);
      j.set("pickup_ms", sp->pickup_ms);
      j.set("bar_ms", sp->bar_ms);
      j.set("total_bars", sp->total_bars);
      Json secs = Json::array();
      for (const StoredPhraseSection& x : sp->sections) {
        Json o = Json::object();
        o.set("start_ms", x.start_ms);
        o.set("end_ms", x.end_ms);
        o.set("start_bar", x.start_bar);
        o.set("end_bar", x.end_bar);
        o.set("bars", x.bars);
        o.set("energy", x.energy);
        o.set("label", x.label);
        o.set("label_confidence", x.label_confidence);
        o.set("kind", x.kind);
        o.set("boundary_confidence", x.boundary_confidence);
        o.set("phrase_aligned", x.phrase_aligned);
        o.set("kick", x.kick);
        secs.push_back(std::move(o));
      }
      j.set("sections", std::move(secs));
      if (r.query.count("bars") && r.query.at("bars") == "1") {  // the compact per-bar data, for drawing an energy strip
        Json starts = Json::array(), energy = Json::array();
        for (const double v : sp->bar_starts_ms) starts.push_back(Json(v));
        for (const float v : sp->bar_energy) energy.push_back(Json(static_cast<double>(v)));
        j.set("bar_starts_ms", std::move(starts));
        j.set("bar_energy", std::move(energy));
      }
      return Response::ok(std::move(j));
    });
    // ---- what goes well after this track (suggestions) and the pairs the user played back to back ----
    const auto next_response = [this, svc](std::int64_t seed, const Request& r) {
      NextSettings ns;
      std::int64_t playlist = 0;
      std::size_t limit = 20;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        ns = next_settings(lib);
      }
      const auto q = [&](const char* name) -> const std::string* {
        const auto it = r.query.find(name);
        return it == r.query.end() ? nullptr : &it->second;
      };
      if (const auto* v = q("max_pitch")) {  // percent
        const double p = std::atof(v->c_str());
        if (!(p > 0 && p <= 50)) return Response::error(400, "max_pitch is a percentage between 0 and 50");
        ns.max_pitch = p / 100.0;
      }
      if (const auto* v = q("half_double")) ns.half_double = *v == "1";
      if (const auto* v = q("harmonic_only")) ns.harmonic_only = *v == "1";
      if (const auto* v = q("exclude_queued")) ns.exclude_queued = *v == "1";
      if (const auto* v = q("exclude_played")) ns.exclude_played = *v == "1";
      if (const auto* v = q("playlist")) playlist = std::atoll(v->c_str());
      if (const auto* v = q("limit")) limit = static_cast<std::size_t>(std::clamp<long long>(std::atoll(v->c_str()), 1, 200));
      AutomixService* a = svc();
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!lib.track(seed)) return Response::error(404, "no such track");
      if (playlist > 0) {
        bool found = false;
        for (const PlaylistNode& n : lib.playlists())
          if (n.id == playlist && !n.is_folder) found = true;
        if (!found) return Response::error(404, "no such playlist");
      }
      const NextTrackOptions o = next_options(lib, a ? &a->queue() : nullptr, ns, playlist, limit);
      return Response::ok(next_tracks_json(lib, seed, lib.suggest(seed, o)));
    };
    s.add_route("GET", "/v1/library/tracks/{id}/suggestions", "library:read", [next_response](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      return next_response(id, r);
    });
    s.add_route("GET", "/v1/decks/{deck}/suggestions", "read", [this, next_response](const Request& r) {
      int deck = -1;
      const auto it = r.params.find("deck");
      if (it != r.params.end() && it->second == "master") {
        deck = master_deck(host.api_engine());
        if (deck < 0) return Response::error(409, "no deck has a track loaded");
      } else {
        std::int64_t d = 0;
        if (!int_param(r, "deck", d) || d < 1 || d > kMaxDecks) return Response::error(404, "no such deck");
        deck = static_cast<int>(d - 1);
      }
      const DeckView v = host.api_engine().deck(deck);
      if (!v.loaded || v.track_id <= 0) return Response::error(409, "no track is loaded on that deck");
      Response resp = next_response(v.track_id, r);
      return resp;
    });
    // ---- linked mix points (schema 10) and vocal regions ----
    s.add_route("GET", "/v1/library/mix-links", "library:read", [this](const Request& r) {
      const auto q = [&](const char* k) {
        const auto it = r.query.find(k);
        return it == r.query.end() ? std::string() : it->second;
      };
      const std::int64_t track = std::atoll(q("track").c_str()), from = std::atoll(q("from").c_str()), to = std::atoll(q("to").c_str());
      if (!track && !from && !to) return Response::error(400, "give track (links out of and into it), from, to, or from and to (links between two tracks)");
      std::vector<MixLink> links;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (from && to) links = lib.mix_links_between(from, to);
        else if (from) links = lib.mix_links_from(from);
        else if (to) links = lib.mix_links_to(to);
        else {
          links = lib.mix_links_from(track);
          for (const MixLink& l : lib.mix_links_to(track)) links.push_back(l);
        }
      }
      Json items = Json::array();
      for (const MixLink& l : links) items.push_back(mix_link_json(l));
      Json j = Json::object();
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/tracks/{id}/mix-links", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      Json out = Json::array(), in = Json::array(), j = Json::object();
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!lib.track(id)) return Response::error(404, "no such track");
      for (const MixLink& l : lib.mix_links_from(id)) out.push_back(mix_link_json(l));
      for (const MixLink& l : lib.mix_links_to(id)) in.push_back(mix_link_json(l));
      j.set("track_id", id);
      j.set("from", std::move(out));
      j.set("to", std::move(in));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/mix-links", "library:write", [this](const Request& r) {
      MixLink l;
      const std::string bad = mix_link_from_body(r.body, l);
      if (!bad.empty()) return Response::error(400, bad);
      std::int64_t id = 0;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!lib.track(l.from_track) || !lib.track(l.to_track)) return Response::error(404, "no such track");
        EditScope api_edit("api");
        id = lib.add_mix_link(l);
        if (id > 0) l = *lib.mix_link(id);
      }
      if (id <= 0) return Response::error(400, "the link was not stored");
      host.api_links_changed();
      Response ok = Response::ok(mix_link_json(l));
      ok.status = 201;
      return ok;
    });
    s.add_route("GET", "/v1/library/mix-links/{id}", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such link");
      std::lock_guard<std::mutex> lock(lib_mutex);
      const auto l = lib.mix_link(id);
      if (!l) return Response::error(404, "no such link");
      return Response::ok(mix_link_json(*l));
    });
    s.add_route("PUT", "/v1/library/mix-links/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such link");
      MixLink l;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        const auto cur = lib.mix_link(id);
        if (!cur) return Response::error(404, "no such link");
        l = *cur;
        const std::string bad = mix_link_from_body(r.body, l);
        if (!bad.empty()) return Response::error(400, bad);
        if (!lib.track(l.from_track) || !lib.track(l.to_track)) return Response::error(404, "no such track");
        EditScope api_edit("api");
        if (!lib.update_mix_link(l)) return Response::error(400, "the link was not changed");
        l = *lib.mix_link(id);
      }
      host.api_links_changed();
      return Response::ok(mix_link_json(l));
    });
    s.add_route("DELETE", "/v1/library/mix-links/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such link");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        EditScope api_edit("api");
        if (!lib.remove_mix_link(id)) return Response::error(404, "no such link");
      }
      host.api_links_changed();
      return Response::ok();
    });
    // Links the two playheads: "when deck A reaches here, deck B starts from where it is now". body: from_deck, to_deck (1..4).
    s.add_route("POST", "/v1/mix-links/from-decks", "library:write", [this](const Request& r) {
      const std::int64_t a = r.body["from_deck"].as_int(0), b = r.body["to_deck"].as_int(0);
      if (a < 1 || a > kMaxDecks || b < 1 || b > kMaxDecks || a == b) return Response::error(400, "from_deck and to_deck must be two different decks (1..4)");
      IEngine& eng = host.api_engine();
      MixLink l;
      l.from_track = host.api_deck_track(static_cast<int>(a - 1));
      l.to_track = host.api_deck_track(static_cast<int>(b - 1));
      const DeckView da = eng.deck(static_cast<int>(a - 1)), db = eng.deck(static_cast<int>(b - 1));
      if (l.from_track <= 0 || l.to_track <= 0 || da.sample_rate <= 0 || db.sample_rate <= 0) return Response::error(409, "both decks need a loaded library track");
      if (l.from_track == l.to_track) return Response::error(409, "both decks hold the same track");
      l.from_ms = da.position / da.sample_rate * 1000.0;
      l.to_ms = db.position / db.sample_rate * 1000.0;
      std::int64_t id = 0;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        EditScope api_edit("api");
        id = lib.add_mix_link(l);
        if (id > 0) l = *lib.mix_link(id);
      }
      if (id <= 0) return Response::error(400, "the link was not stored");
      host.api_links_changed();
      Response ok = Response::ok(mix_link_json(l));
      ok.status = 201;
      return ok;
    });
    s.add_route("GET", "/v1/library/tracks/{id}/vocals", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!lib.track(id)) return Response::error(404, "no such track");
      const auto v = lib.vocals(id);
      Json j = Json::object();
      j.set("track_id", id);
      j.set("analysed", v.has_value());
      if (v) {
        j.set("version", static_cast<std::int64_t>(v->version));
        j.set("current_version", static_cast<std::int64_t>(kVocalAnalysisVersion));
        j.set("source", v->source);
        Json regions = Json::array();
        for (const StoredVocalRegion& g : v->regions) {
          Json o = Json::object();
          o.set("start_ms", g.start_ms);
          o.set("end_ms", g.end_ms);
          regions.push_back(std::move(o));
        }
        j.set("regions", std::move(regions));
      }
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/tracks/{id}/transitions", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
      Json items = Json::array();
      for (const TransitionRow& t : lib.transitions_from(id, 100)) {
        const auto row = lib.track(t.to_id);
        if (!row) continue;
        Json o = Json::object();
        o.set("to_id", t.to_id);
        o.set("title", row->title);
        o.set("artist", row->artist);
        o.set("count", t.count);
        o.set("last_played_ms", t.last_ms);
        o.set("saved", t.saved);
        if (!t.note.empty()) o.set("note", t.note);
        items.push_back(std::move(o));
      }
      Json j = Json::object();
      j.set("track_id", id);
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/transitions", "library:write", [this](const Request& r) {
      const std::int64_t from = r.body["from"].as_int(0), to = r.body["to"].as_int(0);
      if (from <= 0 || to <= 0 || from == to) return Response::error(400, "from and to must be two different tracks");
      std::string note = r.body["note"].is_string() ? r.body["note"].as_string() : std::string();
      if (note.size() > 500) note.resize(500);
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!lib.track(from) || !lib.track(to)) return Response::error(404, "no such track");
      lib.save_transition(from, to, note);
      const auto t = lib.transition(from, to);
      Json j = Json::object();
      j.set("from", from);
      j.set("to", to);
      j.set("count", t ? t->count : 0);
      j.set("note", note);
      j.set("saved", true);
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/tracks/{id}/cue-suggestions", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      SuggestOptions o;
      std::string error;
      if (!suggest_options_from_query(r, o, error)) return Response::error(400, error);
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
      const SuggestResult res = suggest_for_track(lib, id, o, &habit_cache);
      Json j = Json::object();
      j.set("track_id", id);
      j.set("analysed", res.has_phrases);
      j.set("habit", o.habit);
      j.set("habit_samples", res.habit_samples);
      if (!res.note.empty()) j.set("note", res.note);
      Json items = Json::array();
      for (const CueSuggestion& c : res.items) items.push_back(suggestion_json(c));
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/tracks/{id}/cue-suggestions/apply", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      SuggestOptions o;
      std::string error;
      if (!suggest_options_from_body(r.body, o, error)) return Response::error(400, error);
      const bool dry = opt_bool(r.body, "dry_run", false);
      Json j = apply_json(ApplyResult{});
      ApplyResult res;
      std::int64_t batch = 0;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
        EditScope api_edit("api");
        res = apply_suggestions(lib, id, o, &habit_cache, dry);
        batch = dry || res.written.empty() ? 0 : api_edit.batch();
      }
      j = apply_json(res);
      j.set("track_id", id);
      j.set("dry_run", dry);
      j.set("batch_id", batch);
      if (!dry && !res.written.empty()) {
        host.api_library_changed();
        host.api_cues_changed({id});
      }
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/cue-suggestions/apply", "library:write", [this](const Request& r) {
      SuggestOptions o;
      std::string error;
      if (!suggest_options_from_body(r.body, o, error)) return Response::error(400, error);
      const bool dry = opt_bool(r.body, "dry_run", false);
      std::vector<std::int64_t> ids, changed;
      Json tracks = Json::array();
      int written = 0, no_phrases = 0;
      std::int64_t batch = 0;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (r.body["ids"].is_array()) {
          if (!id_list(r.body["ids"], ids)) return Response::error(400, "ids must be numbers");
        } else if (r.body["playlist_id"].is_number() || r.body["search"].is_string()) {
          ids = lib.track_ids(r.body["playlist_id"].as_int(), r.body["search"].as_string());
        } else {
          return Response::error(400, "select tracks with ids, or playlist_id and/or search");
        }
        if (ids.size() > 5000) return Response::error(400, "at most 5000 tracks per call");
        EditScope api_edit("api");  // one batch for every track: one undo_batch takes it all back
        if (!dry) lib.db().exec("BEGIN IMMEDIATE");
        try {
          for (const std::int64_t id : ids) {
            if (!lib.track(id)) continue;
            const ApplyResult res = apply_suggestions(lib, id, o, &habit_cache, dry);
            if (!res.has_phrases) ++no_phrases;
            if (!res.written.empty()) {
              written += static_cast<int>(res.written.size());
              changed.push_back(id);
            }
            if (!res.written.empty() || !res.has_phrases || !res.skipped.empty()) {
              Json t = apply_json(res);
              t.set("track_id", id);
              tracks.push_back(std::move(t));
            }
          }
          if (!dry) lib.db().exec("COMMIT");
        } catch (...) {
          if (!dry) lib.db().exec("ROLLBACK");
          throw;
        }
        batch = dry || changed.empty() ? 0 : api_edit.batch();
      }
      Json j = Json::object();
      j.set("dry_run", dry);
      j.set("batch_id", batch);
      j.set("tracks_looked_at", static_cast<std::int64_t>(ids.size()));
      j.set("tracks_changed", static_cast<std::int64_t>(changed.size()));
      j.set("cues_written", written);
      j.set("tracks_without_phrases", no_phrases);
      j.set("tracks", std::move(tracks));
      if (!dry && !changed.empty()) {
        host.api_library_changed();
        host.api_cues_changed(changed);
      }
      return Response::ok(std::move(j));
    });
    s.add_route("PUT", "/v1/library/tracks/{id}/automix", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      AutomixPoints p;
      std::string error;
      if (!parse_points(r.body, p, false, error)) return Response::error(400, error);
      std::lock_guard<std::mutex> lock(lib_mutex);
      if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
      lib.set_automix_points(id, p);
      return Response::ok(points_json(p));
    });
  }

  void add_chain_routes() {
    auto& s = *server;
    s.add_route("GET", "/v1/fx/chains/{target}", "read", [this](const Request& r) {
      int t = 0;
      Response bad;
      if (!chain_target(r, t, bad)) return bad;
      return Response::ok(chain_json(t));
    });
    s.add_route("POST", "/v1/fx/chains/{target}/units", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      if (!chain_target(r, t, bad)) return bad;
      EffectKind kind = EffectKind::None;
      if (!effect_from_json(r.body["effect"], kind)) return Response::error(400, "effect must be a slug or number from GET /v1/effects");
      FxChainEdit e;
      e.verb = FxChainVerb::Insert;
      e.target = t;
      e.kind = kind;
      e.pos = r.body["pos"].is_number() ? static_cast<int>(r.body["pos"].as_int()) : -1;
      e.value = opt_bool(r.body, "on", false) ? 1 : 0;
      FxChainResult res;
      if (!chain_edit(e, res, bad)) return bad;
      Json j = chain_json(t);
      j.set("id", res.id);
      Response ok = Response::ok(std::move(j));
      ok.status = 201;
      return ok;
    });
    s.add_route("PATCH", "/v1/fx/chains/{target}/units/{id}", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      std::int64_t id = 0;
      if (!chain_target(r, t, bad)) return bad;
      if (!int_param(r, "id", id) || !unit_exists(t, static_cast<int>(id))) return Response::error(404, "no such unit in this chain");
      struct F { const char* key; FxField f; double lo, hi; };
      const F fields[] = {{"wet", FxField::Wet, 0, 1},       {"param", FxField::Param, 0, 1}, {"param2", FxField::Param2, 0, 1},
                          {"param3", FxField::Param3, 0, 1}, {"beats", FxField::Beats, 0.0625, 64}, {"seconds", FxField::Seconds, 0, 60}};
      for (const F& f : fields)
        if (r.body.contains(f.key) && (!r.body[f.key].is_number() || r.body[f.key].as_double() < f.lo || r.body[f.key].as_double() > f.hi))
          return Response::error(400, std::string(f.key) + " must be a number " + std::to_string(f.lo) + ".." + std::to_string(f.hi));
      if (r.body.contains("on") && !r.body["on"].is_bool()) return Response::error(400, "on must be true or false");
      EffectKind kind = EffectKind::None;
      if (r.body.contains("effect") && !effect_from_json(r.body["effect"], kind)) return Response::error(400, "effect must be a slug or number from GET /v1/effects");
      FxChainResult res;
      if (kind != EffectKind::None) {  // replacing resets the unit's other fields, so it goes first
        FxChainEdit e;
        e.verb = FxChainVerb::Replace;
        e.target = t;
        e.id = static_cast<int>(id);
        e.kind = kind;
        if (!chain_edit(e, res, bad)) return bad;
      }
      const auto set = [&](FxField f, double v) {
        FxChainEdit e;
        e.verb = FxChainVerb::Set;
        e.target = t;
        e.id = static_cast<int>(id);
        e.field = static_cast<int>(f);
        e.value = v;
        return chain_edit(e, res, bad);
      };
      if (r.body.contains("on") && !set(FxField::On, r.body["on"].as_bool() ? 1 : 0)) return bad;
      for (const F& f : fields)
        if (r.body.contains(f.key) && !set(f.f, r.body[f.key].as_double())) return bad;
      return Response::ok(chain_json(t));
    });
    s.add_route("DELETE", "/v1/fx/chains/{target}/units/{id}", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      std::int64_t id = 0;
      if (!chain_target(r, t, bad)) return bad;
      if (!int_param(r, "id", id) || !unit_exists(t, static_cast<int>(id))) return Response::error(404, "no such unit in this chain");
      FxChainEdit e;
      e.verb = FxChainVerb::Remove;
      e.target = t;
      e.id = static_cast<int>(id);
      FxChainResult res;
      if (!chain_edit(e, res, bad)) return bad;
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("POST", "/v1/fx/chains/{target}/units/{id}/move", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      std::int64_t id = 0;
      if (!chain_target(r, t, bad)) return bad;
      if (!int_param(r, "id", id) || !unit_exists(t, static_cast<int>(id))) return Response::error(404, "no such unit in this chain");
      if (!r.body["pos"].is_integer() || r.body["pos"].as_int() < 0) return Response::error(400, "pos must be a non-negative integer (the new index)");
      FxChainEdit e;
      e.verb = FxChainVerb::Move;
      e.target = t;
      e.id = static_cast<int>(id);
      e.pos = static_cast<int>(std::min<std::int64_t>(r.body["pos"].as_int(), kFxChainMax));
      FxChainResult res;
      if (!chain_edit(e, res, bad)) return bad;
      return Response::ok(chain_json(t));
    });
    s.add_route("PUT", "/v1/fx/chains/{target}/settings", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      if (!chain_target(r, t, bad)) return bad;
      if (r.body.contains("bypass") && !r.body["bypass"].is_bool()) return Response::error(400, "bypass must be true or false");
      if (r.body.contains("wet") && (!r.body["wet"].is_number() || r.body["wet"].as_double() < 0 || r.body["wet"].as_double() > 1))
        return Response::error(400, "wet must be a number 0..1");
      if (!r.body.contains("bypass") && !r.body.contains("wet")) return Response::error(400, "give bypass and/or wet");
      FxChainResult res;
      if (r.body.contains("bypass")) {
        FxChainEdit e;
        e.verb = FxChainVerb::SetChain;
        e.target = t;
        e.field = 0;
        e.value = r.body["bypass"].as_bool() ? 1 : 0;
        if (!chain_edit(e, res, bad)) return bad;
      }
      if (r.body.contains("wet")) {
        FxChainEdit e;
        e.verb = FxChainVerb::SetChain;
        e.target = t;
        e.field = 1;
        e.value = r.body["wet"].as_double();
        if (!chain_edit(e, res, bad)) return bad;
      }
      return Response::ok(chain_json(t));
    });
    s.add_route("GET", "/v1/fx/chains/{target}/preset", "read", [this](const Request& r) {
      int t = 0;
      Response bad;
      if (!chain_target(r, t, bad)) return bad;
      FxChainEdit e;
      e.verb = FxChainVerb::Save;
      e.target = t;
      FxChainResult res;
      if (!chain_edit(e, res, bad)) return bad;
      return Response::ok(Json::parse(res.text));
    });
    s.add_route("PUT", "/v1/fx/chains/{target}/preset", "control:fx:{target}", [this](const Request& r) {
      int t = 0;
      Response bad;
      if (!chain_target(r, t, bad)) return bad;
      if (!r.body.is_object()) return Response::error(400, "the body is a chain preset (the object GET .../preset returns)");
      FxChainInfo parsed;
      std::string perr;
      if (!fx_chain_from_json(r.body.dump(), parsed, perr)) return Response::error(400, "not a chain preset: " + perr);
      FxChainEdit e;
      e.verb = FxChainVerb::Load;
      e.target = t;
      e.text = r.body.dump();
      FxChainResult res;
      if (!chain_edit(e, res, bad)) return bad;
      return Response::ok(chain_json(t));
    });
  }

  // ---- keymap and stems ---------------------------------------------------------------------------------------------
  static Json stem_job_json(const stems::JobInfo& j) {
    Json o = Json::object();
    o.set("job", j.id);
    o.set("label", j.label);
    o.set("state", stems::job_state_name(j.state));
    o.set("progress", static_cast<double>(j.progress));
    o.set("seconds", j.seconds);
    if (!j.error.empty()) o.set("error", j.error);
    return o;
  }

  void add_keymap_stem_routes() {
    auto& s = *server;
    s.add_route("GET", "/v1/keymap", "read", [this](const Request&) {
      const std::string text = host.api_keymap_json();
      if (text.empty()) return Response::error(404, "keyboard shortcuts are not available");
      return Response::ok(Json::parse(text));
    });
    s.add_route("PUT", "/v1/keymap", "admin", [this](const Request& r) {
      // The body is the .dhkeys text: a JSON string, or an object {"text": "..."}. Empty resets to the shipped defaults.
      std::string text;
      if (r.body.is_string()) text = r.body.as_string();
      else if (r.body["text"].is_string()) text = r.body["text"].as_string();
      else if (!r.body.is_null()) return Response::error(400, "the body is the dhkeys text as a string, or {\"text\": \"...\"}");
      std::vector<std::string> warnings;
      std::string error;
      if (!host.api_keymap_set(text, warnings, error)) return Response::error(400, error);
      Json j = Json::object();
      Json w = Json::array();
      for (const std::string& x : warnings) w.push_back(x);
      j.set("warnings", std::move(w));
      return Response::ok(std::move(j));
    });

    s.add_route("GET", "/v1/stems/status", "read", [this](const Request&) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      const StemStatus x = st->status();
      Json j = Json::object();
      j.set("available", x.available);
      if (!x.available) j.set("why_not", x.why_not);
      j.set("backend", x.backend);
      j.set("model", x.model);
      Json cache = Json::object();
      cache.set("bytes", x.cache_bytes);
      cache.set("cap_bytes", x.cache_cap);
      cache.set("entries", x.cache_entries);
      j.set("cache", std::move(cache));
      Json q = Json::object();
      q.set("paused", x.paused);
      q.set("paused_by_user", x.user_paused);
      q.set("paused_while_playing", x.auto_paused);
      q.set("pending", x.pending);
      Json jobs = Json::array();
      for (const stems::JobInfo& job : x.jobs) jobs.push_back(stem_job_json(job));
      q.set("jobs", std::move(jobs));
      j.set("queue", std::move(q));
      const StemInstallState in = st->install_state();
      Json inst = Json::object();
      inst.set("model", in.id);
      inst.set("state", in.state);
      inst.set("progress", static_cast<double>(in.progress));
      if (!in.error.empty()) inst.set("error", in.error);
      j.set("install", std::move(inst));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/stems/models", "read", [this](const Request&) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      Json a = Json::array();
      for (const StemModelRow& m : st->models()) {
        const stems::CatalogueEntry& e = m.entry;
        Json o = Json::object();
        o.set("id", e.id);
        o.set("name", e.name);
        o.set("kind", e.kind);
        o.set("summary", e.summary);
        o.set("quality", e.quality);
        o.set("speed", e.speed);
        Json stems_list = Json::array();
        for (const std::string& n : e.stems) stems_list.push_back(n);
        o.set("stems", std::move(stems_list));
        o.set("download_bytes", e.total_bytes());
        o.set("installed", m.installed);
        o.set("usable", m.usable);
        o.set("installed_bytes", m.installed_bytes);
        if (!m.licence_accepted_at.empty()) o.set("licence_accepted_at", m.licence_accepted_at);
        o.set("install_supported", e.install_supported);
        if (!e.blocked_by.empty()) o.set("blocked_by", e.blocked_by);
        Json lic = Json::object();
        lic.set("summary", e.licence_summary);
        lic.set("statement_source", e.licence_source);
        lic.set("link", e.licence_link);
        lic.set("weights", e.weights_licence);
        lic.set("code", e.code_licence);
        lic.set("training_data", e.training_data);
        lic.set("restriction", e.restriction);
        o.set("licence", std::move(lic));
        o.set("source", e.upstream_url);
        a.push_back(std::move(o));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/stems/models/{id}/install", "admin", [this](const Request& r) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      const std::string id = r.params.at("id");
      std::string why;
      if (!st->available(&why)) return Response::error(409, why);
      const bool accepted = r.body["accept_licence"].is_bool() && r.body["accept_licence"].as_bool();
      for (const StemModelRow& m : st->models()) {
        if (m.entry.id != id) continue;
        if (!accepted)
          return Response::error(400, "accept_licence: true is required. Licence: " + m.entry.licence_summary + " (" + m.entry.licence_source + ")");
        break;
      }
      std::string error;
      switch (st->install(id, accepted, error)) {
        case StemService::Result::Ok: break;
        case StemService::Result::NotFound: return Response::error(404, error);
        case StemService::Result::NeedLicence: return Response::error(400, error);
        case StemService::Result::Busy:
        case StemService::Result::Unavailable: return Response::error(409, error);
        case StemService::Result::Refused: return Response::error(409, error);
      }
      Json j = Json::object();
      j.set("model", id);
      j.set("state", "running");
      Response ok = Response::ok(std::move(j));
      ok.status = 202;
      return ok;
    });
    s.add_route("DELETE", "/v1/stems/models/{id}", "admin", [this](const Request& r) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      if (!st->remove(r.params.at("id"))) return Response::error(404, "that model is not installed (or is being installed)");
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("POST", "/v1/stems/queue", "library:write", [this](const Request& r) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      std::string why;
      if (!st->available(&why)) return Response::error(409, why);
      std::vector<std::int64_t> ids;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (r.body["track_ids"].is_array()) {
          if (!id_list(r.body["track_ids"], ids)) return Response::error(400, "track_ids must be numbers");
        } else if (r.body["playlist_id"].is_number()) {
          ids = lib.playlist_track_ids(r.body["playlist_id"].as_int());
        } else {
          return Response::error(400, "give track_ids or playlist_id");
        }
      }
      if (ids.size() > 5000) return Response::error(400, "at most 5000 tracks at a time");
      Json queued = Json::array(), skipped = Json::array();
      for (const std::int64_t id : ids) {
        std::optional<TrackRow> row;
        {
          std::lock_guard<std::mutex> lock(lib_mutex);
          row = lib.track(id);
        }
        Json o = Json::object();
        o.set("track_id", id);
        if (!row) {
          o.set("reason", "no such track");
          skipped.push_back(std::move(o));
          continue;
        }
        if (st->track(id).cached && !opt_bool(r.body, "force", false)) {
          o.set("reason", "already separated");
          skipped.push_back(std::move(o));
          continue;
        }
        std::uint64_t job = 0;
        std::string error;
        const std::string label = row->artist.empty() ? row->title : row->artist + " - " + row->title;
        const StemService::Result res = st->enqueue(id, row->path, label, job, error);
        if (res == StemService::Result::Ok) {
          o.set("job", job);
          queued.push_back(std::move(o));
        } else if (res == StemService::Result::Unavailable) {
          return Response::error(409, error);
        } else {
          o.set("reason", error);
          skipped.push_back(std::move(o));
        }
      }
      Json j = Json::object();
      j.set("queued", std::move(queued));
      j.set("skipped", std::move(skipped));
      Response ok = Response::ok(std::move(j));
      ok.status = 202;
      return ok;
    });
    s.add_route("DELETE", "/v1/stems/queue/{job}", "library:write", [this](const Request& r) {
      StemService* st = host.api_stems();
      std::int64_t job = 0;
      if (!st || !int_param(r, "job", job) || job < 1 || !st->cancel_job(static_cast<std::uint64_t>(job))) return Response::error(404, "no such queued job");
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("POST", "/v1/stems/queue/pause", "library:write", [this](const Request&) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      st->pause();
      return Response::ok();
    });
    s.add_route("POST", "/v1/stems/queue/resume", "library:write", [this](const Request&) {
      StemService* st = host.api_stems();
      if (!st) return Response::error(404, "stem separation is not part of this build");
      st->resume();  // the queue still waits while a deck is playing
      return Response::ok();
    });
    s.add_route("GET", "/v1/tracks/{id}/stems", "library:read", [this](const Request& r) {
      StemService* st = host.api_stems();
      std::int64_t id = 0;
      if (!st) return Response::error(404, "stem separation is not part of this build");
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!lib.track(id)) return Response::error(404, "no such track");
      }
      const StemTrackInfo t = st->track(id);
      Json j = Json::object();
      j.set("track_id", id);
      j.set("cached", t.cached);
      if (t.cached) {
        j.set("bytes", t.bytes);
        j.set("model", t.model);
        j.set("key", t.key);
      }
      j.set("queued", t.queued);
      if (t.queued) {
        j.set("job", t.job);
        j.set("state", t.state);
        j.set("progress", static_cast<double>(t.progress));
      }
      return Response::ok(std::move(j));
    });
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
    s.add_route("GET", "/v1/decks/{deck}/stems", "read", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      return Response::ok(stems_json(static_cast<int>(deck - 1)));
    });
    // Body (every key optional): {"preset": "acapella|instrumental|drums_only|no_drums|reset", "enabled": true,
    //   "attach": true, "detach": true, "stems": {"vocals": {"mute": true, "solo": false, "isolate": false, "level": 0.8}}}
    s.add_route("POST", "/v1/decks/{deck}/stems", "control:deck:{deck}", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      const int di = static_cast<int>(deck - 1);
      const std::string dn = "deck" + std::to_string(deck);
      IEngine& eng = host.api_engine();
      std::string error;
      const auto set = [&](const std::string& name, double v) { return apply_control(eng, name, true, v, error); };
      if (r.body["attach"].is_bool() && r.body["attach"].as_bool()) {
        const DeckView v = eng.deck(di);
        StemService* st = host.api_stems();
        const std::string file = st && v.loaded ? st->stems_file(v.track_id) : std::string();
        if (file.empty()) return Response::error(409, "this track has no stems yet; separate it first (POST /v1/stems/tracks/{id})");
        if (!eng.stems_attach(di, v.track_id, file, error)) return Response::error(409, error);
      }
      if (r.body["detach"].is_bool() && r.body["detach"].as_bool() && !set(dn + ".stems.detach", 1)) return Response::error(400, error);
      if (r.body["preset"].is_string() && !set(dn + ".stems." + r.body["preset"].as_string(), 1)) return Response::error(400, error);
      if (r.body["enabled"].is_bool() && !set(dn + ".stems.enable", r.body["enabled"].as_bool() ? 1 : 0)) return Response::error(400, error);
      if (r.body["stems"].is_object()) {
        for (const char* name : {"vocals", "drums", "bass", "other"}) {
          const Json& e = r.body["stems"][name];
          if (!e.is_object()) continue;
          for (const char* f : {"mute", "solo", "isolate"})
            if (e[f].is_bool() && !set(dn + ".stem." + name + "." + f, e[f].as_bool() ? 1 : 0)) return Response::error(400, error);
          if (e["level"].is_number() && !set(dn + ".stem." + name + ".level", e["level"].as_double())) return Response::error(400, error);
        }
      }
      return Response::ok(stems_json(di));
    });
    // Key sync (also reachable through the generic deck control route): follow, match once, reset.
    s.add_route("POST", "/v1/decks/{deck}/key_sync", "control:deck:{deck}",
                [this](const Request& r) { return control("deck" + r.params.at("deck") + ".key_sync", r.body); });
    s.add_route("POST", "/v1/decks/{deck}/key_match", "control:deck:{deck}",
                [this](const Request& r) { return control("deck" + r.params.at("deck") + ".key_match", r.body); });
    s.add_route("POST", "/v1/decks/{deck}/key_reset", "control:deck:{deck}",
                [this](const Request& r) { return control("deck" + r.params.at("deck") + ".key_reset", r.body); });
    // Key sync behaviour for every deck: mode (0 match the key, 1 any compatible key), fine (cents instead of whole
    // semitones) and limit (largest shift in semitones, 0..12). Stored as settings keysync.mode / .fine / .limit.
    auto key_sync_config_json = [this]() {
      KeySyncSettings st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = parse_key_sync_settings(lib.setting("keysync.mode"), lib.setting("keysync.fine"), lib.setting("keysync.limit"));
      }
      Json j = Json::object();
      j.set("mode", st.mode);
      j.set("fine", st.fine);
      j.set("limit", st.limit);
      return j;
    };
    s.add_route("GET", "/v1/key_sync/config", "read", [key_sync_config_json](const Request&) { return Response::ok(key_sync_config_json()); });
    s.add_route("PUT", "/v1/key_sync/config", "control", [this, key_sync_config_json](const Request& r) {
      const Json& b = r.body;
      if ((!b["mode"].is_null() && !b["mode"].is_number()) || (!b["fine"].is_null() && !b["fine"].is_bool()) ||
          (!b["limit"].is_null() && !b["limit"].is_number()))
        return Response::error(400, "mode and limit are numbers, fine is a boolean");
      if (b["mode"].is_number() && b["mode"].as_double() != 0 && b["mode"].as_double() != 1) return Response::error(400, "mode is 0 or 1");
      if (b["limit"].is_number() && (!(b["limit"].as_double() >= 0) || b["limit"].as_double() > 12)) return Response::error(400, "limit is 0..12 semitones");
      KeySyncSettings st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (b["mode"].is_number()) lib.set_setting("keysync.mode", b["mode"].as_double() != 0 ? "1" : "0");
        if (b["fine"].is_bool()) lib.set_setting("keysync.fine", b["fine"].as_bool() ? "1" : "0");
        if (b["limit"].is_number()) lib.set_setting("keysync.limit", std::to_string(b["limit"].as_double()));
        st = parse_key_sync_settings(lib.setting("keysync.mode"), lib.setting("keysync.fine"), lib.setting("keysync.limit"));
      }
      apply_key_sync_settings(host.api_engine(), st);
      return Response::ok(key_sync_config_json());
    });
    // Every deck control: POST /v1/decks/1/play, /v1/decks/2/hotcue.3, /v1/decks/1/seek {"value": 62.5}
    s.add_route("POST", "/v1/decks/{deck}/{control}", "control:deck:{deck}", [this](const Request& r) {
      return control("deck" + r.params.at("deck") + "." + r.params.at("control"), r.body);
    });
    s.add_route("GET", "/v1/decks/{deck}/grid", "read", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      return prep_state(static_cast<int>(deck - 1), "grid");
    });
    s.add_route("GET", "/v1/decks/{deck}/cues", "read", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      return prep_state(static_cast<int>(deck - 1), "cues");
    });
    s.add_route("POST", "/v1/decks/{deck}/grid/{action}", "control:deck:{deck}", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      return prep_response(static_cast<int>(deck - 1), "grid." + r.params.at("action"), 0, r.body);
    });
    s.add_route("POST", "/v1/decks/{deck}/cues/{action}", "control:deck:{deck}", [this](const Request& r) {
      std::int64_t deck = 0;
      if (!int_param(r, "deck", deck) || deck < 1 || deck > kMaxDecks) return Response::error(404, "no such deck");
      const std::string action = r.params.at("action");
      return prep_response(static_cast<int>(deck - 1), (action.compare(0, 5, "wave_") == 0 ? "wave." + action.substr(5) : "cue." + action), 0, r.body);
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
        std::string laction;
        if (parse_lighting_control(c.name, laction)) {
          if (host.api_lighting() && host.api_lighting()->read_control(laction, v)) j.set("value", v);
        } else if (read_control(host.api_engine(), c.name, v)) {
          j.set("value", v);
        }
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("GET", "/v1/controllers", "read", [this](const Request&) {
      std::lock_guard<std::mutex> lock(lib_mutex);
      return Response::ok(Json::parse(host.api_controllers_json(lib)));
    });

    // ---- lighting (DMX over Art-Net / sACN; off until enabled) ----
    s.add_route("GET", "/v1/lighting/status", "read", [this](const Request&) {
      LightingBridge* lb = host.api_lighting();
      return Response::ok(Json::parse(lb ? lb->status_json() : std::string("{\"available\":false,\"enabled\":false,\"running\":false}")));
    });
    s.add_route("GET", "/v1/lighting/config", "admin", [this](const Request&) {
      LightingBridge* lb = host.api_lighting();
      if (!lb || !LightingBridge::compiled()) return Response::error(404, "this build has no lighting module");
      return Response::ok(Json::parse(lb->config_json()));
    });
    s.add_route("PUT", "/v1/lighting/config", "admin", [this](const Request& r) {
      LightingBridge* lb = host.api_lighting();
      if (!lb || !LightingBridge::compiled()) return Response::error(404, "this build has no lighting module");
      std::string error;
      if (!lb->set_config_json(r.body.dump(), error)) return Response::error(400, error);
      return Response::ok(Json::parse(lb->config_json()));
    });
    s.add_route("POST", "/v1/lighting/command", "control", [this](const Request& r) {
      LightingBridge* lb = host.api_lighting();
      if (!lb || !LightingBridge::compiled()) return Response::error(404, "this build has no lighting module");
      std::string error;
      if (!lb->command_json(r.body.dump(), error)) return Response::error(400, error);
      return Response::ok(Json::parse(lb->status_json()));
    });
    s.add_route("GET", "/v1/lighting/preview", "read", [this](const Request&) {
      LightingBridge* lb = host.api_lighting();
      return Response::ok(Json::parse(lb ? lb->preview_json() : std::string("{\"running\":false,\"fixtures\":[],\"universes\":[]}")));
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
        j.set("when", c.when);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("DELETE", "/v1/schedule", "control", [this](const Request&) {
      std::lock_guard<std::mutex> lock(sched_mutex);
      scheduled.clear();
      host.api_engine().cancel_all();
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("DELETE", "/v1/schedule/{id}", "control", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such command");
      std::lock_guard<std::mutex> lock(sched_mutex);
      const auto it = std::find_if(scheduled.begin(), scheduled.end(), [&](const Scheduled& c) { return c.id == static_cast<std::uint64_t>(id); });
      if (it == scheduled.end()) return Response::error(404, "no such command");
      for (const auto eid : it->engine_ids) host.api_engine().cancel(eid);
      scheduled.erase(it);
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
        j.set("param2", e.param2_name);
        j.set("param3", e.param3_name);
        j.set("slug", e.slug);
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
    // ---- step sequencer and slicing (protocol 8) ----
    s.add_route("GET", "/v1/sequencer", "read", [this](const Request&) {
      const StepSeqView v = host.api_engine().step_seq();
      Json j = Json::object();
      j.set("running", v.running != 0);
      j.set("recording", v.recording != 0);
      j.set("pattern", static_cast<std::int64_t>(v.current) + 1);
      j.set("queued", v.queued == 0xFF ? static_cast<std::int64_t>(0) : static_cast<std::int64_t>(v.queued) + 1);
      j.set("step", v.step == 0xFF ? static_cast<std::int64_t>(0) : static_cast<std::int64_t>(v.step) + 1);
      j.set("follows_deck", v.locked != 0);
      j.set("bpm", static_cast<double>(v.bpm));
      j.set("swing", static_cast<double>(v.swing));
      Json pats = Json::array();
      for (int i = 0; i < kSeqPatterns; ++i) pats.push_back(Json(seq_pattern_to_text(v.pattern[i])));
      j.set("patterns", std::move(pats));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/sequencer/patterns/{pattern}", "read", [this](const Request& r) {
      std::int64_t n = 0;
      if (!int_param(r, "pattern", n) || n < 1 || n > kSeqPatterns) return Response::error(404, "no such pattern (1..8)");
      Json j = Json::object();
      j.set("pattern", n);
      j.set("text", host.api_engine().seq_pattern_text(static_cast<int>(n - 1)));
      return Response::ok(std::move(j));
    });
    s.add_route("PUT", "/v1/sequencer/patterns/{pattern}", "control", [this](const Request& r) {
      std::int64_t n = 0;
      if (!int_param(r, "pattern", n) || n < 1 || n > kSeqPatterns) return Response::error(404, "no such pattern (1..8)");
      if (!host.api_engine().seq_pattern_set(static_cast<int>(n - 1), r.body["text"].as_string())) return Response::error(400, "not a valid pattern text (see docs/SEQUENCER.md)");
      Json j = Json::object();
      j.set("pattern", n);
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/sequencer/step", "control", [this](const Request& r) {
      const std::int64_t pat = r.body["pattern"].is_number() ? r.body["pattern"].as_int() : host.api_engine().step_seq().current + 1;
      const std::int64_t step = r.body["step"].as_int(0), slot = r.body["slot"].as_int(0);
      if (pat < 1 || pat > kSeqPatterns || step < 1 || step > kSeqSteps || slot < 1 || slot > kSamplerSlots)
        return Response::error(400, "pattern 1..8, step 1..16 and slot 1..16 are needed");
      const double vel = r.body["velocity"].is_number() ? r.body["velocity"].as_double() : -1.0;
      if (vel > 1.0) return Response::error(400, "velocity is 0..1");
      host.api_engine().seq_step(static_cast<int>(pat - 1), static_cast<int>(step - 1), static_cast<int>(slot - 1), r.body["on"].as_bool(true),
                                 static_cast<float>(vel));
      return Response::ok();
    });
    s.add_route("POST", "/v1/sequencer/{action}", "control", [this](const Request& r) {
      const std::string& a = r.params.at("action");
      const Json& b = r.body;
      std::string name, error;
      if (a == "run" || a == "record") name = "seq." + a;
      else if (a == "bpm" || a == "swing") name = "seq." + a;
      else if (a == "select" || a == "queue") {
        const std::int64_t n = b["pattern"].as_int(0);
        if (n < 1 || n > kSeqPatterns) return Response::error(400, "pattern 1..8 is needed");
        name = "seq.pattern." + std::to_string(n) + (a == "queue" || b["at_next_bar"].as_bool(false) ? ".queue" : ".select");
      } else if (a == "clear") name = "seq.clear";
      else return Response::error(404, "use run, record, bpm, swing, select, queue or clear");
      bool has_value = false;
      double v = 0;
      if (a == "run" || a == "record") {
        const Json& on = b["on"];
        has_value = on.is_bool() || on.is_number();
        v = on.is_bool() ? (on.as_bool() ? 1.0 : 0.0) : on.as_double();
      } else if (a == "bpm" || a == "swing") {
        if (!b["value"].is_number()) return Response::error(400, "value is needed");
        has_value = true;
        v = b["value"].as_double();
      } else if (a == "clear") {  // none = the pattern being played; "all": true; "pattern": n
        if (b["all"].as_bool(false)) has_value = true, v = 0;
        else if (b["pattern"].is_number()) has_value = true, v = static_cast<double>(b["pattern"].as_int());
      }
      std::vector<Command> commands;
      if (!build_control(host.api_engine(), name, has_value, v, commands, error)) return Response::error(400, error);
      for (Command c : commands) {
        if (a == "clear" && b["slot"].is_number() && b["slot"].as_int() >= 1 && b["slot"].as_int() <= kSamplerSlots) c.d = static_cast<double>(b["slot"].as_int() - 1);
        host.api_engine().send(c);
      }
      Json j = Json::object();
      j.set("control", name);
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/sampler/slices", "read", [this](const Request&) {
      Json a = Json::array();
      for (int i = 0; i < kSamplerSlots; ++i) {
        const SamplerView sv = host.api_engine().sampler(i);
        Json j = Json::object();
        j.set("slot", static_cast<std::int64_t>(i) + 1);
        j.set("loaded", sv.loaded);
        j.set("slices", static_cast<std::int64_t>(sv.slices));
        j.set("last_slice", sv.slice == 0xFF ? static_cast<std::int64_t>(0) : static_cast<std::int64_t>(sv.slice) + 1);
        j.set("scratching", sv.scratching);
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/sampler/{slot}/slices", "control", [this](const Request& r) {
      std::int64_t slot = 0;
      if (!int_param(r, "slot", slot) || slot < 1 || slot > kSamplerSlots) return Response::error(404, "no such slot");
      return control("sampler.slot." + std::to_string(slot) + ".slices", [&] { Json b = Json::object(); b.set("value", r.body["count"]); return b; }());
    });
    s.add_route("POST", "/v1/sampler/{slot}/slices/{index}/trigger", "control", [this](const Request& r) {
      std::int64_t slot = 0, idx = 0;
      if (!int_param(r, "slot", slot) || slot < 1 || slot > kSamplerSlots) return Response::error(404, "no such slot");
      if (!int_param(r, "index", idx) || idx < 1 || idx > 16) return Response::error(404, "no such slice (1..16)");
      Json b = Json::object();
      if (r.body["velocity"].is_number()) b.set("value", r.body["velocity"]);
      return control("sampler.slot." + std::to_string(slot) + ".slice." + std::to_string(idx), b);
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
        if (c.color >= 0) cj.set("color", c.color);
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
      if (r.body["color"].is_number() && (r.body["color"].as_double() < 0 || r.body["color"].as_double() > 16777215)) return Response::error(400, "color must be 0 (none) or 0xRRGGBB as a number");
      Json out;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!int_param(r, "id", id) || !lib.track(id)) return Response::error(404, "no such track");
        EditScope api_edit("api");  // one journal batch for the whole request
        lib.edit_track(id, e);
        if (r.body["color"].is_number()) lib.set_track_color(id, static_cast<int>(r.body["color"].as_int()));
        if (r.body["grid_locked"].is_bool()) lib.set_grid_locked(id, r.body["grid_locked"].as_bool());
        out = track_json(*lib.track(id));
      }
      host.api_library_changed();
      return Response::ok(std::move(out));
    });
    s.add_route("GET", "/v1/library/tracks/{id}/history", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      const auto it = r.query.find("limit");
      const int limit = static_cast<int>(std::clamp<std::int64_t>(it == r.query.end() ? 50 : std::atoll(it->second.c_str()), 1, 200));
      Json items = Json::array();
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        for (const TrackEditRecord& e : lib.track_history(id, limit, r.query.count("include_analysis") && r.query.at("include_analysis") == "1")) items.push_back(edit_json(e));
      }
      Json j = Json::object();
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/edits", "library:read", [this](const Request& r) {
      const auto it = r.query.find("limit");
      const int limit = static_cast<int>(std::clamp<std::int64_t>(it == r.query.end() ? 50 : std::atoll(it->second.c_str()), 1, 500));
      Json items = Json::array();
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        for (const TrackEditRecord& e : lib.recent_edits(limit, r.query.count("include_analysis") && r.query.at("include_analysis") == "1")) items.push_back(edit_json(e));
      }
      Json j = Json::object();
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/edits/{id}/undo", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such edit");
      UndoResult u;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        u = lib.undo_edit(id);
      }
      if (u.status == UndoStatus::Ok) host.api_library_changed();
      return undo_response(u);
    });
    s.add_route("POST", "/v1/library/edit-batches/{id}/undo", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such edit batch");
      UndoResult u;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        u = lib.undo_batch(id);
      }
      if (u.status == UndoStatus::Ok) host.api_library_changed();
      return undo_response(u);
    });
    s.add_route("POST", "/v1/library/tracks/{id}/grid", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      const Json& secs = r.body["sections"];
      if (!secs.is_array() || secs.size() == 0 || secs.size() > 256) return Response::error(400, "sections must be an array of 1 to 256 tempo sections");
      std::vector<BeatSection> grid;
      for (std::size_t i = 0; i < secs.size(); ++i) {
        const Json& o = secs.at(i);
        const double bpm = o["bpm"].as_double(), first = o["first_beat_ms"].as_double(-1e18);
        const std::int64_t bar = o["bar_beat"].is_number() ? o["bar_beat"].as_int() : 1;
        if (!o["bpm"].is_number() || !(bpm >= 30 && bpm <= 300) || !(first > -1e9 && first < 1e9) || bar < 1 || bar > 4)
          return Response::error(400, "each section needs first_beat_ms, bpm 30 to 300 and bar_beat 1 to 4");
        grid.push_back({first, bpm, static_cast<int>(bar)});
      }
      std::optional<bool> lock;
      if (r.body["lock"].is_bool()) lock = r.body["lock"].as_bool();
      std::optional<double> bpm = grid.front().bpm;
      if (r.body["bpm"].is_number()) bpm = r.body["bpm"].as_double();
      const bool dry = opt_bool(r.body, "dry_run", false);
      Json j = Json::object();
      {
        std::lock_guard<std::mutex> lock_(lib_mutex);
        if (!lib.track(id)) return Response::error(404, "no such track");
        CueFollow mode = cue_follow_from(lib.setting("cues_follow_grid", "ask"));
        if (r.body["cues_follow"].is_string()) {
          const std::string m = r.body["cues_follow"].as_string();
          if (m != "ask" && m != "always" && m != "never") return Response::error(400, "cues_follow must be ask, always or never");
          mode = cue_follow_from(m);
        }
        EditScope api_edit("api");
        const GridCommit c = lib.commit_grid(id, grid, bpm, lock, mode, kCueGridToleranceMs, dry);
        j.set("dry_run", dry);
        j.set("batch_id", c.batch_id);
        j.set("grid_changed", c.grid_changed);
        j.set("cues_follow", cue_follow_name(c.mode));
        j.set("cues_applied", c.applied);
        j.set("mix_links_moved", c.mix_links_moved);
        j.set("cues", follow_json(c.proposal));
      }
      if (!dry) host.api_library_changed();
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/tracks/{id}/cues/follow-grid", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track");
      if (!r.body["batch_id"].is_number()) return Response::error(400, "batch_id (from the grid edit) is required");
      FollowResult f;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!lib.track(id)) return Response::error(404, "no such track");
        f = lib.apply_cue_follow(id, r.body["batch_id"].as_int());
      }
      if (f.status == FollowStatus::NoGridEdit) return Response::error(404, "that batch holds no grid edit of this track");
      if (f.status == FollowStatus::AlreadyApplied) return Response::error(409, "the cues of that grid edit were already moved");
      if (f.status == FollowStatus::Conflict) return Response::error(409, "the grid was edited again since; nothing was moved");
      host.api_library_changed();
      Json j = Json::object();
      j.set("batch_id", f.batch_id);
      j.set("mix_links_moved", f.mix_links_moved);
      j.set("cues", follow_json(f.proposal));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/analysis/strip", "library:write", [this](const Request& r) {
      StripKinds k;
      const Json& kinds = r.body["kinds"];
      if (!kinds.is_array() || kinds.size() == 0) return Response::error(400, "kinds must list what to clear: grid, key, loudness, waveform, first_sound, phrases, vocals, stems");
      for (std::size_t i = 0; i < kinds.size(); ++i) {
        const std::string n = kinds.at(i).as_string();
        if (n == "grid") k.grid = true;
        else if (n == "key") k.key = true;
        else if (n == "loudness") k.loudness = true;
        else if (n == "waveform") k.waveform = true;
        else if (n == "first_sound") k.first_sound = true;
        else if (n == "phrases") k.phrases = true;
        else if (n == "vocals") k.vocals = true;
        else if (n == "stems") k.stems = true;
        else return Response::error(400, "unknown kind: " + n);
      }
      std::vector<std::int64_t> ids;
      Json j;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (r.body["ids"].is_array()) {
          if (!id_list(r.body["ids"], ids)) return Response::error(400, "ids must be numbers");
        } else if (r.body["playlist_id"].is_number() || r.body["search"].is_string()) {
          ids = lib.track_ids(r.body["playlist_id"].as_int(), r.body["search"].as_string());
        } else if (opt_bool(r.body, "all", false)) {
          ids = lib.track_ids(0, "");
        } else {
          return Response::error(400, "select tracks with ids, playlist_id and/or search, or all: true");
        }
        EditScope api_edit("api");
        StripResult res = lib.strip_analysis(ids, k, opt_bool(r.body, "force", false), opt_bool(r.body, "dry_run", false));
        if (k.stems) {  // the separated stems of these tracks live in the stem cache, not in the library
          int n = 0;
          if (StemService* st = host.api_stems()) {
            for (const std::int64_t id : ids) {
              if (res.dry_run) n += st->track(id).cached ? 1 : 0;
              else n += st->evict_track(id) ? 1 : 0;
            }
          }
          res.notes.erase(std::remove_if(res.notes.begin(), res.notes.end(), [](const std::string& t) { return t.rfind("stems:", 0) == 0; }), res.notes.end());
          if (n > 0) res.cleared["stems"] = n;
          else res.notes.push_back("stems: no cached stems for these tracks");
        }
        j = strip_json(res);
        if (!res.dry_run) host.api_library_changed();
      }
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/library/duplicates", "library:read", [this](const Request& r) {
      DuplicateOptions o;
      const auto q = [&](const char* k) { return r.query.count(k) ? r.query.at(k) : std::string(); };
      const std::string mode = q("mode");
      if (mode == "exact") o.likely = false;
      else if (mode == "likely") o.exact = false;
      else if (!mode.empty() && mode != "all") return Response::error(400, "mode must be exact, likely or all");
      o.same_bpm = q("same_bpm") == "1";
      o.same_key = q("same_key") == "1";
      if (!q("duration_ms").empty()) o.duration_tolerance_ms = std::clamp<double>(std::atof(q("duration_ms").c_str()), 0, 60000);
      if (!q("limit").empty()) o.limit = static_cast<std::size_t>(std::clamp<long long>(std::atoll(q("limit").c_str()), 1, 5000));
      Json items = Json::array(), j = Json::object();
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        for (const DuplicateGroup& g : lib.find_duplicates(o)) items.push_back(duplicate_json(g));
        const HashProgress p = lib.hash_pending(0);  // limit 0 only counts what is still unhashed
        j.set("hash_pending", p.remaining);
      }
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/duplicates/hash", "library:write", [this](const Request& r) {
      const int limit = static_cast<int>(std::clamp<std::int64_t>(r.body["limit"].is_number() ? r.body["limit"].as_int() : 50, 1, 500));
      HashProgress p;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        p = lib.hash_pending(limit);
      }
      Json j = Json::object();
      j.set("sized", p.sized);
      j.set("hashed", p.hashed);
      j.set("missing", p.missing);
      j.set("remaining", p.remaining);
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/duplicates/merge", "library:write", [this](const Request& r) {
      if (!r.body["keep_id"].is_number()) return Response::error(400, "keep_id is required");
      std::vector<std::int64_t> remove;
      if (!id_list(r.body["remove_ids"], remove) || remove.empty()) return Response::error(400, "remove_ids must be a non-empty array of track ids");
      MergeOptions o;
      const Json& opt = r.body["options"];
      if (opt["copy_cues"].is_string()) {
        const std::string c = opt["copy_cues"].as_string();
        if (c == "missing") o.cues = MergeOptions::Cues::Missing;
        else if (c == "all") o.cues = MergeOptions::Cues::All;
        else if (c == "none") o.cues = MergeOptions::Cues::None;
        else return Response::error(400, "copy_cues must be missing, all or none");
      }
      o.fill_metadata = opt_bool(opt, "fill_metadata", true);
      o.combine_history = opt_bool(opt, "combine_history", true);
      o.dry_run = opt_bool(opt, "dry_run", false);
      MergeResult m;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        EditScope api_edit("api");
        m = lib.merge_duplicates(r.body["keep_id"].as_int(), remove, o);
      }
      if (!m.ok) return Response::error(400, m.error);
      if (!o.dry_run) host.api_library_changed();
      return Response::ok(merge_json(m));
    });
    s.add_route("GET", "/v1/library/trash", "library:read", [this](const Request&) {
      Json items = Json::array();
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        for (const DeletedTrack& d : lib.deleted_tracks()) {
          Json t = Json::object();
          t.set("id", d.id);
          t.set("title", d.title);
          t.set("artist", d.artist);
          t.set("path", d.path);
          t.set("deleted_at", d.deleted_at);
          items.push_back(std::move(t));
        }
      }
      Json j = Json::object();
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/library/trash/{id}/restore", "library:write", [this](const Request& r) {
      std::int64_t id = 0, restored = 0;
      if (!int_param(r, "id", id)) return Response::error(404, "no such track in the trash");
      bool known = false, ok = false;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        for (const DeletedTrack& d : lib.deleted_tracks()) known = known || d.id == id;
        ok = known && lib.restore_track(id, &restored);
      }
      if (!known) return Response::error(404, "no such track in the trash");
      if (!ok) return Response::error(409, "the file is in the library again under another entry");
      host.api_library_changed();
      Json j = Json::object();
      j.set("id", restored);
      return Response::ok(std::move(j));
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
    // ---- smart playlists: saved rule sets, evaluated when read ----
    // ---- shared playlists: one playlist shared with another person's library through a folder both replicate ----
    // (docs/SYNC.md). `folder` is the replicated folder (absolute); `label` is the name the other side sees.
    const auto shared_config = [this](const Json& b, const std::string& folder_from_query, sync::SharedConfig& cfg, std::string& error) {
      cfg.folder = b["folder"].is_string() ? b["folder"].as_string() : folder_from_query;
      if (cfg.folder.empty() || cfg.folder[0] != '/') {
        error = "folder must be the absolute path of the shared folder";
        return false;
      }
      std::error_code ec;
      if (!std::filesystem::is_directory(cfg.folder, ec)) {
        error = "the shared folder does not exist";
        return false;
      }
      cfg.label = b["label"].is_string() ? b["label"].as_string() : std::string();
      if (cfg.label.empty()) {
        std::lock_guard<std::mutex> lock(lib_mutex);
        cfg.label = lib.setting("sync.device_name");
      }
      if (cfg.label.empty()) cfg.label = "DeckHand";
      return true;
    };
    const auto shared_result_json = [](const sync::SharedResult& res) {
      Json j = Json::object();
      j.set("shared_id", res.shared_id);
      j.set("playlist_id", static_cast<double>(res.playlist_id));
      Json sum = Json::object();  // the merge summary
      sum.set("sent_added", res.added);
      sum.set("sent_removed", res.removed);
      sum.set("sent_moved", res.moved);
      sum.set("received", res.received);
      sum.set("missing", res.missing);
      j.set("summary", std::move(sum));
      return j;
    };
    s.add_route("GET", "/v1/library/shared-playlists", "library:read", [this, shared_config](const Request& r) {
      sync::SharedConfig cfg;
      std::string error;
      if (!shared_config(Json::object(), r.query.count("folder") ? r.query.at("folder") : std::string(), cfg, error)) return Response::error(400, error);
      Json a = Json::array();
      for (const sync::SharedPlaylistInfo& p : sync::list_shared_playlists(cfg)) {
        Json j = Json::object();
        j.set("shared_id", p.shared_id);
        j.set("name", p.name);
        Json who = Json::array();
        for (const std::string& l : p.participants) who.push_back(l);
        j.set("participants", std::move(who));
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/library/playlists/{id}/share", "library:write", [this, shared_config, shared_result_json](const Request& r) {
      std::int64_t id = 0;
      sync::SharedConfig cfg;
      std::string error;
      if (!int_param(r, "id", id)) return Response::error(400, "bad playlist id");
      if (!shared_config(r.body, std::string(), cfg, error)) return Response::error(400, error);
      std::lock_guard<std::mutex> lock(lib_mutex);
      const sync::SharedResult res = sync::share_playlist(lib, id, cfg);
      if (!res.ok) return Response::error(409, res.error);
      return Response::ok(shared_result_json(res));
    });
    s.add_route("POST", "/v1/library/shared-playlists/{shared_id}/join", "library:write", [this, shared_config, shared_result_json](const Request& r) {
      sync::SharedConfig cfg;
      std::string error;
      if (!shared_config(r.body, std::string(), cfg, error)) return Response::error(400, error);
      sync::SharedResult res;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        res = sync::join_shared_playlist(lib, r.params.at("shared_id"), cfg, r.body["parent_id"].as_int(0));
      }
      if (!res.ok) return Response::error(409, res.error);
      host.api_library_changed();
      return Response::ok(shared_result_json(res));
    });
    s.add_route("POST", "/v1/library/playlists/{id}/shared-sync", "library:write", [this, shared_config, shared_result_json](const Request& r) {
      std::int64_t id = 0;
      sync::SharedConfig cfg;
      std::string error;
      if (!int_param(r, "id", id)) return Response::error(400, "bad playlist id");
      if (!shared_config(r.body, std::string(), cfg, error)) return Response::error(400, error);
      sync::SharedResult res;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        res = sync::sync_shared_playlist(lib, id, cfg);
      }
      if (!res.ok) return Response::error(409, res.error);
      if (res.received > 0) host.api_library_changed();
      return Response::ok(shared_result_json(res));
    });
    s.add_route("DELETE", "/v1/library/playlists/{id}/share", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      if (!int_param(r, "id", id)) return Response::error(400, "bad playlist id");
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        sync::unlink_shared_playlist(lib, id);  // the playlist stays; the files in the folder are left alone
      }
      Response ok;
      ok.status = 204;
      return ok;
    });

    s.add_route("GET", "/v1/library/smart-playlists", "library:read", [this](const Request&) {
      Json a = Json::array();
      std::lock_guard<std::mutex> lock(lib_mutex);
      for (const SmartPlaylist& p : lib.smart_playlists()) {
        Json j = smart_json(p);
        j.set("track_count", static_cast<std::int64_t>(lib.query_ids(p.rules, p.match_all).size()));
        a.push_back(std::move(j));
      }
      return Response::ok(std::move(a));
    });
    s.add_route("POST", "/v1/library/smart-playlists", "library:write", [this](const Request& r) {
      SmartPlaylist p;
      std::string error;
      if (!smart_from_json(r.body, p, error)) return Response::error(400, error);
      if (p.name.empty()) return Response::error(400, "name is required");
      Json out;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        p.id = lib.create_smart_playlist(p.name, p.match_all, p.rules);
        out = smart_json(*lib.smart_playlist(p.id));
      }
      host.api_library_changed();
      Response ok = Response::ok(std::move(out));
      ok.status = 201;
      return ok;
    });
    s.add_route("GET", "/v1/library/smart-playlists/{id}", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      std::lock_guard<std::mutex> lock(lib_mutex);
      const auto p = int_param(r, "id", id) ? lib.smart_playlist(id) : std::nullopt;
      if (!p) return Response::error(404, "no such smart playlist");
      Json j = smart_json(*p);
      j.set("track_count", static_cast<std::int64_t>(lib.query_ids(p->rules, p->match_all).size()));
      return Response::ok(std::move(j));
    });
    s.add_route("PATCH", "/v1/library/smart-playlists/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      Json out;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        auto cur = int_param(r, "id", id) ? lib.smart_playlist(id) : std::nullopt;
        if (!cur) return Response::error(404, "no such smart playlist");
        std::string error;
        if (!smart_from_json(r.body, *cur, error)) return Response::error(400, error);
        if (cur->name.empty()) return Response::error(400, "name must not be empty");
        lib.update_smart_playlist(*cur);
        out = smart_json(*lib.smart_playlist(id));
      }
      host.api_library_changed();
      return Response::ok(std::move(out));
    });
    s.add_route("DELETE", "/v1/library/smart-playlists/{id}", "library:write", [this](const Request& r) {
      std::int64_t id = 0;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!int_param(r, "id", id) || !lib.smart_playlist(id)) return Response::error(404, "no such smart playlist");
        lib.delete_smart_playlist(id);
      }
      host.api_library_changed();
      Response ok;
      ok.status = 204;
      return ok;
    });
    s.add_route("GET", "/v1/library/smart-playlists/{id}/tracks", "library:read", [this](const Request& r) {
      std::int64_t id = 0;
      std::vector<TrackRow> rows;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        if (!int_param(r, "id", id) || !lib.smart_playlist(id)) return Response::error(404, "no such smart playlist");
        for (const std::int64_t tid : lib.smart_track_ids(id))
          if (const auto row = lib.track(tid)) rows.push_back(*row);
      }
      Json items = Json::array();
      for (const TrackRow& t : rows) items.push_back(track_json(t));
      Json j = Json::object();
      j.set("total", static_cast<std::int64_t>(rows.size()));
      j.set("items", std::move(items));
      return Response::ok(std::move(j));
    });

    // ---- pre-listen: the headphone-only preview player ----
    s.add_route("GET", "/v1/preview", "read", [this](const Request&) {
      const PreviewView v = host.api_engine().preview();
      Json j = Json::object();
      j.set("loaded", v.loaded != 0);
      j.set("playing", v.playing != 0);
      j.set("on_master", v.on_master != 0);
      j.set("level", static_cast<double>(v.level));
      j.set("track_id", v.loaded ? Json(v.track_id) : Json());
      j.set("title", v.loaded ? Json(host.api_preview_title()) : Json());
      j.set("position_seconds", v.sample_rate > 0 ? v.position / v.sample_rate : 0.0);
      j.set("length_seconds", v.sample_rate > 0 ? v.length / v.sample_rate : 0.0);
      j.set("headphone_output", host.api_engine().output_channels() >= 4);
      return Response::ok(std::move(j));
    });
    s.add_route("POST", "/v1/preview/load", "control", [this](const Request& r) {
      if (!r.body["track_id"].is_number()) return Response::error(400, "give track_id");
      std::string error;
      if (!host.api_preview_load(r.body["track_id"].as_int(), r.body["play"].as_bool(false), error)) return Response::error(409, error);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });
    s.add_route("POST", "/v1/preview/{control}", "control", [this](const Request& r) {
      return control("preview." + r.params.at("control"), r.body);
    });

    // ---- debug: windows and hit testing (admin). Shows what a click on a shaped, transparent window would do. ----
    s.add_route("GET", "/v1/debug/windows", "admin", [this](const Request&) {
      const std::string text = host.api_debug_windows();
      if (text.empty()) return Response::error(503, "the UI did not answer (no frame is being drawn)");
      return Response::ok(Json::parse(text));
    });
    s.add_route("GET", "/v1/debug/hit", "admin", [this](const Request& r) {
      const auto num = [&](const char* k, float& v) {
        const auto it = r.query.find(k);
        if (it == r.query.end() || it->second.empty()) return false;
        char* end = nullptr;
        const double d = std::strtod(it->second.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(d) || std::fabs(d) > 1e6) return false;
        v = static_cast<float>(d);
        return true;
      };
      float x = 0, y = 0;
      const auto w = r.query.find("window");
      if (w == r.query.end() || !num("x", x) || !num("y", y)) return Response::error(400, "give window, x and y (window points)");
      const std::string text = host.api_debug_hit(w->second, x, y);
      if (text.empty()) return Response::error(503, "the UI did not answer (no frame is being drawn)");
      return Response::ok(Json::parse(text));
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
    // ---- interchange files: paths are on the machine running DeckHand ----
    s.add_route("POST", "/v1/library/export/xml", "export", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (!absolute_path(path)) return Response::error(400, "path must be an absolute file path");
      InterchangeExportOptions o;
      o.playlist_id = r.body["playlist_id"].as_int(0);
      o.include_playlists = opt_bool(r.body, "include_playlists", true);
      o.include_cues = opt_bool(r.body, "include_cues", true);
      o.include_grid = opt_bool(r.body, "include_grid", true);
      InterchangeExportStats st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = export_library_xml(lib, path, o);
      }
      if (!st.ok) return Response::error(409, st.error);
      return Response::ok(export_stats_json(st));
    });
    s.add_route("POST", "/v1/library/import/xml", "library:write", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (!absolute_path(path)) return Response::error(400, "path must be an absolute file path");
      InterchangeImportOptions o;
      o.merge = opt_bool(r.body, "merge", o.merge);
      o.overwrite_cues = opt_bool(r.body, "overwrite_cues", o.overwrite_cues);
      o.overwrite_grid = opt_bool(r.body, "overwrite_grid", o.overwrite_grid);
      o.overwrite_metadata = opt_bool(r.body, "overwrite_metadata", o.overwrite_metadata);
      o.match_by_name = opt_bool(r.body, "match_by_name", o.match_by_name);
      o.import_playlists = opt_bool(r.body, "import_playlists", o.import_playlists);
      o.import_play_counts = opt_bool(r.body, "import_play_counts", o.import_play_counts);
      o.dry_run = opt_bool(r.body, "dry_run", false);
      InterchangeStats st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = import_library_xml(lib, path, o);
      }
      if (!st.ok) return Response::error(409, st.error);
      if (!o.dry_run) host.api_library_changed();
      return Response::ok(import_stats_json(st));
    });
    s.add_route("POST", "/v1/library/playlists/{id}/export/m3u8", "export", [this](const Request& r) {
      std::int64_t id = 0;
      const std::string path = r.body["path"].as_string();
      if (!int_param(r, "id", id)) return Response::error(404, "no such playlist");
      if (!absolute_path(path)) return Response::error(400, "path must be an absolute file path");
      InterchangeExportStats st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = export_playlist_m3u8(lib, id, path, opt_bool(r.body, "relative_paths", false));
      }
      if (!st.ok) return Response::error(409, st.error);
      return Response::ok(export_stats_json(st));
    });
    s.add_route("POST", "/v1/library/export/m3u8", "export", [this](const Request& r) {
      const std::string dir = r.body["directory"].as_string();
      if (!absolute_path(dir)) return Response::error(400, "directory must be an absolute folder path");
      InterchangeExportStats st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = export_all_playlists_m3u8(lib, dir, opt_bool(r.body, "relative_paths", true));
      }
      if (!st.ok) return Response::error(409, st.error);
      return Response::ok(export_stats_json(st));
    });
    s.add_route("POST", "/v1/library/import/m3u8", "library:write", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (!absolute_path(path)) return Response::error(400, "path must be an absolute file path");
      M3uImportOptions o;
      o.playlist_name = r.body["playlist_name"].as_string();
      o.parent = r.body["parent"].as_int(0);
      o.merge = opt_bool(r.body, "merge", o.merge);
      o.add_missing_tracks = opt_bool(r.body, "add_missing_tracks", o.add_missing_tracks);
      o.dry_run = opt_bool(r.body, "dry_run", false);
      InterchangeStats st;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        st = import_playlist_m3u8(lib, path, o);
      }
      if (!st.ok) return Response::error(409, st.error);
      if (!o.dry_run) host.api_library_changed();
      return Response::ok(import_stats_json(st));
    });
    s.add_route("POST", "/v1/export/preflight", "export", [this](const Request& r) {
      const std::string id = r.body["profile"].is_string() ? r.body["profile"].as_string() : std::string("club");
      const usbexport::PlayerProfile* profile = usbexport::find_player_profile(id);
      if (!profile) return Response::error(400, "unknown player profile");
      std::vector<usbexport::ExportTrack> tracks;
      {
        std::lock_guard<std::mutex> lock(lib_mutex);
        std::vector<std::int64_t> ids;
        const Json& pl = r.body["playlist_ids"];
        const Json& tr = r.body["track_ids"];
        for (std::size_t i = 0; i < pl.size(); ++i)
          for (const auto t : lib.playlist_track_ids(pl.at(i).as_int(0))) ids.push_back(t);
        for (std::size_t i = 0; i < tr.size(); ++i) ids.push_back(tr.at(i).as_int(0));
        if (pl.size() == 0 && tr.size() == 0) ids = lib.track_ids(0, "");
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const auto t : ids) {
          const auto row = lib.track(t);
          if (!row) continue;
          usbexport::ExportTrack e;
          e.id = static_cast<int>(t);
          e.source_path = row->path;
          tracks.push_back(std::move(e));
        }
      }
      const usbexport::PreflightReport report = usbexport::preflight_export(tracks, *profile);
      return Response::ok(Json::parse(report.to_json()));
    });
    s.add_route("GET", "/v1/export/report", "library:read", [this](const Request&) {
      const std::string text = host.api_export_report();
      if (text.empty()) return Response::error(404, "no export has run yet");
      return Response::ok(Json::parse(text));
    });
    s.add_route("POST", "/v1/export", "export", [this](const Request& r) {
      const std::string dest = r.body["destination"].as_string();
      if (dest.empty() || dest[0] != '/') return Response::error(400, "destination must be an absolute folder");
      std::string error;
      const std::int64_t smart = r.body["smart_playlist_id"].as_int(0);  // exported as an ordinary playlist (passed on negative)
      const std::int64_t plain = r.body["playlist_id"].as_int(0);
      if (smart != 0 && plain != 0) return Response::error(400, "give playlist_id or smart_playlist_id, not both");
      const std::string profile = r.body["player_profile"].is_string() ? r.body["player_profile"].as_string() : std::string();
      if (!profile.empty() && !usbexport::find_player_profile(profile)) return Response::error(400, "unknown player profile");
      ApiExportRequest req;
      req.destination = dest;
      req.smart_playlist_id = smart;
      if (plain != 0) req.playlist_ids.push_back(plain);
      if (r.body["playlist_ids"].size() > 0) {
        if (plain != 0 || smart != 0) return Response::error(400, "give playlist_id, playlist_ids or smart_playlist_id, not several");
        for (std::size_t i = 0; i < r.body["playlist_ids"].size(); ++i) req.playlist_ids.push_back(r.body["playlist_ids"].at(i).as_int(0));
      }
      req.player_profile = profile;
      req.convert_incompatible = opt_bool(r.body, "convert_incompatible", false);
      // Stems on the stick: stem_mixes = ["acapella", "instrumental", "drums", "bass", "other"] (or a bit set number),
      // stem_mixes_by_playlist = {"<playlist id>": [...]}, stem_bits 16 | 24, stems_inline, stems_folder.
      const auto mixes_of = [](const Json& j, unsigned& out) {
        out = 0;
        if (j.is_number()) {
          out = static_cast<unsigned>(j.as_int(0)) & 31u;
          return true;
        }
        if (j.is_string() && j.as_string() == "all") {
          out = usbexport::kMixesFourStems;
          return true;
        }
        if (!j.is_array()) return j.is_null();
        for (std::size_t i = 0; i < j.size(); ++i) {
          const std::string n = j.at(i).as_string();
          if (n == "acapella") out |= usbexport::kMixAcapella;
          else if (n == "instrumental") out |= usbexport::kMixInstrumental;
          else if (n == "drums") out |= usbexport::kMixDrums;
          else if (n == "bass") out |= usbexport::kMixBass;
          else if (n == "other") out |= usbexport::kMixOther;
          else return false;
        }
        return true;
      };
      if (r.body.contains("stem_mixes") || r.body.contains("stem_mixes_by_playlist")) {
        req.stems_given = true;
        if (!mixes_of(r.body["stem_mixes"], req.stem_mixes)) return Response::error(400, "stem_mixes: acapella, instrumental, drums, bass, other, or \"all\"");
        const Json& by = r.body["stem_mixes_by_playlist"];
        if (by.is_object()) {
          for (std::size_t bi = 0; bi < by.size(); ++bi) {
            unsigned m = 0;
            const std::int64_t id = std::atoll(by.key_at(bi).c_str());
            if (id <= 0 || !mixes_of(by.at(bi), m)) return Response::error(400, "stem_mixes_by_playlist: playlist id -> list of mixes");
            req.stem_mixes_by_playlist[id] = m;
          }
        }
        req.stem_bits = static_cast<int>(r.body["stem_bits"].as_int(24));
        if (req.stem_bits != 16 && req.stem_bits != 24) return Response::error(400, "stem_bits is 16 or 24");
        req.stems_inline = opt_bool(r.body, "stems_inline", false);
        if (r.body["stems_folder"].is_string()) req.stems_folder = r.body["stems_folder"].as_string();
      }
      if (!host.api_export_ex(req, error)) return Response::error(409, error);
      Response ok = Response::ok();
      ok.status = 202;
      return ok;
    });

    // ---- the stick itself: pre-gig check, dated backup of its library, restore (docs/EXPORT.md) ----
    s.add_route("POST", "/v1/export/verify_device", "export", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (path.empty() || path[0] != '/') return Response::error(400, "path must be the absolute path of the stick");
      usbexport::VerifyOptions vo;
      vo.check_audio = opt_bool(r.body, "check_audio", true);
      usbexport::DeviceReport rep;
      usbexport::verify_device(path, rep, vo);
      return Response::ok(Json::parse(rep.to_json()));
    });
    s.add_route("POST", "/v1/export/backup_device", "export", [this](const Request& r) {
      const std::string path = r.body["path"].as_string();
      if (path.empty() || path[0] != '/') return Response::error(400, "path must be the absolute path of the stick");
      const std::string dir = host.api_device_backup_dir();
      if (dir.empty()) return Response::error(404, "device backups are not available");
      usbexport::BackupOptions bo;
      bo.include_audio = opt_bool(r.body, "include_audio", false);
      std::string backup, error;
      if (!usbexport::backup_device(path, dir, bo, backup, error)) return Response::error(409, error);
      Json j = Json::object();
      j.set("backup", backup);
      return Response::ok(std::move(j));
    });
    s.add_route("GET", "/v1/export/device_backups", "export", [this](const Request&) {
      Json a = Json::array();
      const std::string dir = host.api_device_backup_dir();
      if (!dir.empty()) {
        for (const usbexport::BackupInfo& b : usbexport::list_device_backups(dir)) {
          Json j = Json::object();
          j.set("backup", b.path);
          j.set("name", b.name);
          j.set("created", b.created);
          j.set("files", static_cast<double>(b.files));
          j.set("bytes", static_cast<double>(b.bytes));
          j.set("has_audio", b.has_audio);
          j.set("valid", b.valid);
          a.push_back(std::move(j));
        }
      }
      return Response::ok(std::move(a));
    });
    // Writes into the stick: admin scope, a backup folder from the list above, and `force` to replace an existing library.
    s.add_route("POST", "/v1/export/restore_device", "admin", [this](const Request& r) {
      const std::string path = r.body["path"].as_string(), backup = r.body["backup"].as_string();
      if (path.empty() || path[0] != '/' || backup.empty() || backup[0] != '/')
        return Response::error(400, "path (the stick) and backup (a folder from GET /v1/export/device_backups) are absolute paths");
      const std::string dir = host.api_device_backup_dir();
      bool known = false;
      for (const usbexport::BackupInfo& b : usbexport::list_device_backups(dir)) known = known || b.path == backup;
      if (!known) return Response::error(404, "no such backup");
      usbexport::RestoreOptions ro;
      ro.force = opt_bool(r.body, "force", false);
      ro.restore_audio = opt_bool(r.body, "restore_audio", true);
      usbexport::RestoreResult rr;
      std::string error;
      if (!usbexport::restore_device(backup, path, ro, rr, error)) return Response::error(409, error);
      Json j = Json::object();
      j.set("files", static_cast<double>(rr.files));
      j.set("bytes", static_cast<double>(rr.bytes));
      j.set("removed_stale", static_cast<double>(rr.removed_stale));
      return Response::ok(std::move(j));
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
    if (AutomixService* a = host.api_automix()) {
      a->set_listener([this](const AutomixEvent& ev) {
        if (!server) return;
        Json e = Json::object();
        e.set("type", ev.type);
        for (const auto& kv : ev.text) e.set(kv.first, kv.second);
        for (const auto& kv : ev.num) e.set(kv.first, kv.second);
        server->publish("events", e);
      });
    }
    if (StemService* st = host.api_stems()) {
      st->set_event_sink([this](const std::string& type, const std::string& id, const std::string& state, float progress, const std::string& error) {
        if (!server) return;
        Json e = Json::object();
        e.set("type", type);
        e.set(type == "stems.install" ? "model" : "job", id);
        e.set("state", state);
        e.set("progress", static_cast<double>(progress));
        if (!error.empty()) e.set("error", error);
        server->publish("events", e);
      });
    }
    add_chain_routes();
    add_queue_routes();
    add_keymap_stem_routes();
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
  if (StemService* st = impl_->host.api_stems()) st->set_event_sink(nullptr);
  if (AutomixService* a = impl_->host.api_automix()) a->set_listener(nullptr);
  impl_->quit = true;
  if (impl_->ticker.joinable()) impl_->ticker.join();
  if (impl_->server) impl_->server->stop();
}

bool ApiBridge::running() const { return impl_->server && impl_->server->running(); }
std::string ApiBridge::describe() const { return impl_->description; }
void ApiBridge::engine_event(const EngineEvent& e) { impl_->engine_event(e); }

}  // namespace deckhand

#else  // built without the API server (no OpenSSL on this platform)

namespace deckhand {
struct ApiBridge::Impl {};
ApiBridge::ApiBridge(ApiHost&, const std::string&) : impl_(std::make_unique<Impl>()) {}
ApiBridge::~ApiBridge() = default;
bool ApiBridge::running() const { return false; }
std::string ApiBridge::describe() const { return "API off"; }
void ApiBridge::engine_event(const EngineEvent&) {}
}  // namespace deckhand

#endif
