// Matching of database candidates to library tracks, proposals, and applying accepted ones through the library's edit journal.
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "deckhand/metadata/metadata.hpp"

#ifndef DH_METADATA_APP_VERSION
#define DH_METADATA_APP_VERSION "0"
#endif

namespace deckhand::metadata {

std::string user_agent() { return std::string("DeckHand/") + DH_METADATA_APP_VERSION; }

bool enabled(Library& lib) { return lib.setting(kSettingEnabled, "0") == "1"; }
void set_enabled(Library& lib, bool on) { lib.set_setting(kSettingEnabled, on ? "1" : "0"); }

const char* status_name(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Disabled: return "disabled";
    case Status::Skipped: return "skipped";
    case Status::NoMatch: return "no_match";
    case Status::LowConfidence: return "low_confidence";
    case Status::Error: return "error";
    case Status::Cancelled: return "cancelled";
  }
  return "error";
}

std::vector<Scored> score_candidates(const TrackQuery& q, const std::vector<Candidate>& cands, const MatchOptions& o) {
  std::vector<Scored> out;
  for (const Candidate& c : cands) {
    Scored s;
    s.c = c;
    s.title = title_similarity(q.title, c.title);
    s.artist = artist_similarity(q.artist, c.artist);
    if (s.title < o.min_title || s.artist < o.min_artist) continue;
    if (q.duration_ms > 0 && c.duration_ms > 0) {
      const std::int64_t diff = std::llabs(q.duration_ms - c.duration_ms);
      if (diff > o.duration_tolerance_ms) continue;  // a different version (extended mix, edit ...)
      const std::int64_t flat = std::min<std::int64_t>(2000, o.duration_tolerance_ms);
      s.duration = diff <= flat ? 1.0 : 1.0 - 0.5 * static_cast<double>(diff - flat) / static_cast<double>(o.duration_tolerance_ms - flat);
    } else {
      s.duration = 0.7;  // unknown on one side: neither for nor against
    }
    s.album = !q.album.empty() && !c.album.empty() ? title_similarity(q.album, c.album) : 0.5;
    s.confidence = 0.45 * s.title + 0.30 * s.artist + 0.15 * s.duration + 0.10 * s.album;
    double penalty = 0;
    if (c.secondary_types.count("Compilation") || c.primary_type == "Compilation") penalty += 0.06;
    if (c.secondary_types.count("Live")) penalty += 0.05;
    if (c.secondary_types.count("Remix") || c.secondary_types.count("DJ-mix") || c.secondary_types.count("Mixtape/Street")) penalty += 0.03;
    if (!c.status.empty() && c.status != "Official") penalty += 0.04;
    if (c.album.empty()) penalty += 0.02;
    s.rank = s.confidence - penalty;
    out.push_back(std::move(s));
  }
  std::stable_sort(out.begin(), out.end(), [](const Scored& a, const Scored& b) {
    if (std::abs(a.rank - b.rank) > 1e-9) return a.rank > b.rank;
    const int ya = a.c.year ? a.c.year : 9999, yb = b.c.year ? b.c.year : 9999;  // earlier release first
    if (ya != yb) return ya < yb;
    return a.c.release_id < b.c.release_id;
  });
  return out;
}

namespace {

std::string title_case(const std::string& s) {
  std::string out = s;
  bool start = true;
  for (char& c : out) {
    if (start && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    start = c == ' ' || c == '-' || c == '/';
  }
  return out;
}

int year_of_track(Library& lib, std::int64_t id) {
  Stmt s(lib.db(), "SELECT year FROM tracks WHERE id = ?1");
  s.bind(1, id);
  return s.step() && !s.is_null(0) ? static_cast<int>(s.i64(0)) : 0;
}
std::string label_of_track(Library& lib, std::int64_t id) {
  Stmt s(lib.db(), "SELECT label FROM tracks WHERE id = ?1");
  s.bind(1, id);
  return s.step() && !s.is_null(0) ? s.text(0) : std::string();
}

std::string current_value(Library& lib, const TrackRow& row, const std::string& field) {
  if (field == "title") return row.title;
  if (field == "artist") return row.artist;
  if (field == "album") return row.album;
  if (field == "genre") return row.genre;
  if (field == "year") {
    const int y = year_of_track(lib, row.id);
    return y > 0 ? std::to_string(y) : std::string();
  }
  return {};
}

// Equal apart from case, diacritics and punctuation: nothing to propose.
bool same_text(const std::string& a, const std::string& b) { return !a.empty() && normalize_artist(a) == normalize_artist(b); }

}  // namespace

Service::Service(Library& lib, std::shared_ptr<Transport> t, std::shared_ptr<Clock> c, RatePolicy rate) : lib_(lib) {
  if (!t) t = make_https_transport();
  if (!c) c = system_clock();
  provider_ = std::make_shared<MusicBrainz>(std::make_shared<RateLimitedClient>(std::move(t), std::move(c), user_agent(), rate));
}
Service::Service(Library& lib, std::shared_ptr<Provider> provider) : lib_(lib), provider_(std::move(provider)) {}

bool Service::enabled() const { return metadata::enabled(lib_); }

std::vector<TrackProposals> Service::propose(const std::vector<std::int64_t>& ids, const FillOptions& o, const ProgressFn& progress,
                                             const std::atomic<bool>* cancel) {
  std::vector<TrackProposals> out;
  for (std::size_t n = 0; n < ids.size(); ++n) {
    TrackProposals tp;
    tp.track_id = ids[n];
    if (!enabled()) {
      tp.status = Status::Disabled;
      tp.message = "metadata lookup is off (setting " + std::string(kSettingEnabled) + ")";
    } else if (cancel && cancel->load()) {
      tp.status = Status::Cancelled;
    } else {
      const auto row = lib_.track(ids[n]);
      if (!row) {
        tp.status = Status::Skipped;
        tp.message = "no such track";
      } else if (row->title.empty() || row->artist.empty()) {
        tp.status = Status::Skipped;
        tp.message = "needs an artist and a title";
      }
      if (row && tp.message.empty()) {
        TrackQuery q;
        q.artist = row->artist;
        q.title = row->title;
        q.album = row->album;
        q.duration_ms = row->duration_ms;
        const SearchResult sr = provider_->search(q, cancel);
        if (!sr.ok) {
          tp.status = cancel && cancel->load() ? Status::Cancelled : Status::Error;
          tp.message = sr.error;
        } else {
          auto scored = score_candidates(q, sr.candidates, o.match);
          if (scored.empty()) {
            tp.status = Status::NoMatch;
            tp.message = "no recording in the database matches closely enough";
          } else if (scored.front().confidence < o.match.min_confidence) {
            tp.status = Status::LowConfidence;
            tp.confidence = scored.front().confidence;
            tp.matched = scored.front().c.artist + " - " + scored.front().c.title;
            tp.message = "best match is below the confidence threshold";
          } else {
            Scored best = scored.front();
            if (o.lookup_release && !best.c.release_id.empty()) {
              std::string err;
              if (!provider_->detail(best.c, cancel, &err)) tp.message = "release details unavailable: " + err;
            }
            tp.status = Status::Ok;
            tp.confidence = best.confidence;
            tp.matched = best.c.artist + " - " + best.c.title;
            const std::set<std::string> edited = lib_.protected_fields(row->id);
            const auto add = [&](const std::string& field, std::string proposed, bool info) {
              if (proposed.empty()) return;
              Proposal p;
              p.track_id = row->id;
              p.field = field;
              p.proposed = std::move(proposed);
              p.confidence = best.confidence;
              p.source_url = best.c.source_url;
              p.provider = provider_->name();
              p.info_only = info;
              if (info) {
                if (field == "label") p.current = label_of_track(lib_, row->id);
                if (!p.current.empty() && same_text(p.current, p.proposed)) return;
                tp.proposals.push_back(std::move(p));
                return;
              }
              if (!o.fields.count(field)) return;
              p.current = current_value(lib_, *row, field);
              if (p.current == p.proposed) return;
              if (field != "year" && same_text(p.current, p.proposed)) return;
              p.overwrites = !p.current.empty();
              p.edited_by_hand = edited.count(field) > 0;
              if (p.overwrites && !o.overwrite_nonempty) return;
              if (p.edited_by_hand && !o.overwrite_edited) return;
              tp.proposals.push_back(std::move(p));
            };
            add("album", best.c.album, false);
            add("year", best.c.year > 0 ? std::to_string(best.c.year) : std::string(), false);
            add("genre", best.c.genres.empty() ? std::string() : title_case(best.c.genres.front()), false);
            add("title", best.c.title, false);
            add("artist", best.c.artist, false);
            add("label", best.c.label, true);
            add("isrc", best.c.isrc, true);
            add("track_number", best.c.track_number > 0 ? std::to_string(best.c.track_number) : std::string(), true);
          }
        }
      }
    }
    out.push_back(std::move(tp));
    if (progress) progress(n + 1, ids.size());
  }
  return out;
}

ApplyResult Service::apply(const std::vector<Proposal>& accepted, const ApplyOptions& o) {
  ApplyResult r;
  const auto skip = [&](const Proposal& p, const std::string& why) {
    ++r.skipped;
    r.skipped_reasons.push_back("track " + std::to_string(p.track_id) + " " + p.field + ": " + why);
  };
  std::map<std::int64_t, std::vector<const Proposal*>> by_track;
  for (const Proposal& p : accepted) by_track[p.track_id].push_back(&p);
  EditScope scope(kSource);
  for (const auto& [id, list] : by_track) {
    const auto row = lib_.track(id);
    if (!row) {
      for (const Proposal* p : list) skip(*p, "no such track");
      continue;
    }
    const std::set<std::string> edited = lib_.protected_fields(id);
    TrackEdit e;
    int n = 0;
    for (const Proposal* p : list) {
      if (p->info_only) {
        skip(*p, "the library has no field for it");
        continue;
      }
      if (p->field != "title" && p->field != "artist" && p->field != "album" && p->field != "genre" && p->field != "year") {
        skip(*p, "unknown field");
        continue;
      }
      if (p->proposed.empty()) {
        skip(*p, "empty value");
        continue;
      }
      const std::string now = current_value(lib_, *row, p->field);
      if (now != p->current) {
        skip(*p, "changed since the lookup");
        continue;
      }
      if (!now.empty() && !o.allow_overwrite) {
        skip(*p, "would replace a value");
        continue;
      }
      if (edited.count(p->field) && !o.overwrite_edited) {
        skip(*p, "edited by hand");
        continue;
      }
      if (p->field == "title") e.title = p->proposed;
      else if (p->field == "artist") e.artist = p->proposed;
      else if (p->field == "album") e.album = p->proposed;
      else if (p->field == "genre") e.genre = p->proposed;
      else e.year = std::atoi(p->proposed.c_str());
      ++n;
    }
    if (n > 0) {
      lib_.edit_track(id, e);
      r.applied += n;
    }
  }
  r.batch_id = scope.batch();
  return r;
}

UndoResult Service::undo(std::int64_t batch_id) { return lib_.undo_batch(batch_id); }

}  // namespace deckhand::metadata
