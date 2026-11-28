// MusicBrainz web service (JSON): recording search and release lookup. Requests go through RateLimitedClient.
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "deckhand/api/json.hpp"
#include "deckhand/metadata/metadata.hpp"

namespace deckhand::metadata {
namespace {

using api::Json;

std::string credit(const Json& arr) {
  std::string out;
  if (!arr.is_array()) return out;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const Json& c = arr.at(i);
    std::string n = c["name"].as_string();
    if (n.empty()) n = c["artist"]["name"].as_string();
    out += n + c["joinphrase"].as_string();
  }
  return out;
}

int year_of(const std::string& date) {
  if (date.size() < 4) return 0;
  for (int i = 0; i < 4; ++i)
    if (!std::isdigit(static_cast<unsigned char>(date[static_cast<std::size_t>(i)]))) return 0;
  return std::atoi(date.substr(0, 4).c_str());
}

int leading_int(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  return i == 0 ? 0 : std::atoi(s.substr(0, i).c_str());
}

// genres (curated) or tags, most voted first; names as given by the service.
std::vector<std::string> votes(const Json& genres, const Json& tags) {
  std::vector<std::pair<std::int64_t, std::string>> v;
  const Json& src = genres.is_array() && genres.size() ? genres : tags;
  if (src.is_array())
    for (std::size_t i = 0; i < src.size(); ++i) {
      const std::string n = src.at(i)["name"].as_string();
      if (!n.empty()) v.emplace_back(src.at(i)["count"].as_int(), n);
    }
  std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::string> out;
  for (auto& p : v) out.push_back(std::move(p.second));
  return out;
}

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    o += c;
  }
  return o + "\"";
}

// The title with bracketed groups removed (feat., remaster ...), unless it carries a remix/edit/live qualifier that picks the recording.
std::string query_title(const std::string& title) {
  if (!normalize_title(title).variant.empty()) return title;
  std::string out;
  int depth = 0;
  for (char c : title) {
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    else if (depth == 0) out += c;
  }
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out.empty() ? title : out;
}

}  // namespace

MusicBrainz::MusicBrainz(std::shared_ptr<RateLimitedClient> http, std::string base_url, int limit)
    : http_(std::move(http)), base_(std::move(base_url)), limit_(limit) {
  if (!base_.empty() && base_.back() != '/') base_ += '/';
}

std::string MusicBrainz::search_url(const TrackQuery& q) const {
  const std::string query = "recording:" + quote(query_title(q.title)) + " AND artist:" + quote(q.artist);
  return base_ + "recording?query=" + percent_encode(query) + "&fmt=json&limit=" + std::to_string(limit_);
}

std::string MusicBrainz::release_url(const std::string& id) const {
  return base_ + "release/" + percent_encode(id) + "?inc=labels+genres+recordings&fmt=json";
}

SearchResult MusicBrainz::parse_search(const std::string& json) {
  SearchResult r;
  Json doc;
  std::string err;
  if (!Json::try_parse(json, &doc, &err) || !doc.is_object()) {
    r.error = "the service answered with something that is not JSON";
    return r;
  }
  if (doc.contains("error") && !doc.contains("recordings")) {
    r.error = "the service reported: " + doc["error"].as_string();
    return r;
  }
  const Json& recs = doc["recordings"];
  if (!recs.is_array()) {
    r.error = "unexpected response (no recordings list)";
    return r;
  }
  for (std::size_t i = 0; i < recs.size(); ++i) {
    const Json& rec = recs.at(i);
    if (!rec.is_object() || rec["id"].as_string().empty()) continue;
    Candidate base;
    base.recording_id = rec["id"].as_string();
    base.title = rec["title"].as_string();
    base.artist = credit(rec["artist-credit"]);
    base.duration_ms = rec["length"].as_int();
    if (rec["isrcs"].is_array() && rec["isrcs"].size()) base.isrc = rec["isrcs"].at(0).as_string();
    base.genres = votes(rec["genres"], rec["tags"]);
    base.source_url = "https://musicbrainz.org/recording/" + base.recording_id;
    const Json& rels = rec["releases"];
    bool any = false;
    if (rels.is_array())
      for (std::size_t k = 0; k < rels.size(); ++k) {
        const Json& rel = rels.at(k);
        if (!rel.is_object()) continue;
        Candidate c = base;
        c.release_id = rel["id"].as_string();
        c.album = rel["title"].as_string();
        c.status = rel["status"].as_string();
        c.year = year_of(rel["date"].as_string());
        const Json& rg = rel["release-group"];
        c.primary_type = rg["primary-type"].as_string();
        if (rg["secondary-types"].is_array())
          for (std::size_t s = 0; s < rg["secondary-types"].size(); ++s) c.secondary_types.insert(rg["secondary-types"].at(s).as_string());
        const Json& media = rel["media"];
        if (media.is_array() && media.size()) {
          const Json& tr = media.at(0)["track"];
          if (tr.is_array() && tr.size()) {
            c.track_number = leading_int(tr.at(0)["number"].as_string());
            if (c.track_number == 0 && media.at(0).contains("track-offset")) c.track_number = static_cast<int>(media.at(0)["track-offset"].as_int()) + 1;
          }
        }
        r.candidates.push_back(std::move(c));
        any = true;
      }
    if (!any) r.candidates.push_back(std::move(base));
  }
  r.ok = true;
  return r;
}

bool MusicBrainz::parse_release(const std::string& json, Candidate& c, std::string* error) {
  Json doc;
  if (!Json::try_parse(json, &doc, nullptr) || !doc.is_object() || doc["id"].as_string().empty()) {
    if (error) *error = "the release answer is not usable";
    return false;
  }
  const Json& li = doc["label-info"];
  if (li.is_array())
    for (std::size_t i = 0; i < li.size(); ++i) {
      const std::string n = li.at(i)["label"]["name"].as_string();
      if (!n.empty()) {
        c.label = n;
        break;
      }
    }
  auto g = votes(doc["genres"], Json());
  if (!g.empty()) {
    for (const auto& old : c.genres)
      if (std::find(g.begin(), g.end(), old) == g.end()) g.push_back(old);
    c.genres = std::move(g);
  }
  if (const int y = year_of(doc["date"].as_string())) c.year = y;
  if (c.status.empty()) c.status = doc["status"].as_string();
  const Json& media = doc["media"];
  if (media.is_array())
    for (std::size_t m = 0; m < media.size(); ++m) {
      const Json& tracks = media.at(m)["tracks"];
      if (!tracks.is_array()) continue;
      for (std::size_t t = 0; t < tracks.size(); ++t)
        if (tracks.at(t)["recording"]["id"].as_string() == c.recording_id) {
          const int n = static_cast<int>(tracks.at(t)["position"].as_int());
          c.track_number = n > 0 ? n : leading_int(tracks.at(t)["number"].as_string());
          return true;
        }
    }
  return true;
}

SearchResult MusicBrainz::search(const TrackQuery& q, const std::atomic<bool>* cancel) {
  SearchResult r;
  if (q.title.empty() || q.artist.empty()) {
    r.error = "artist and title are needed";
    return r;
  }
  const HttpResponse h = http_->get(search_url(q), cancel);
  if (h.status != 200) {
    r.error = !h.error.empty() ? h.error : "HTTP " + std::to_string(h.status);
    return r;
  }
  return parse_search(h.body);
}

bool MusicBrainz::detail(Candidate& c, const std::atomic<bool>* cancel, std::string* error) {
  if (c.release_id.empty()) {
    if (error) *error = "no release";
    return false;
  }
  const HttpResponse h = http_->get(release_url(c.release_id), cancel);
  if (h.status != 200) {
    if (error) *error = !h.error.empty() ? h.error : "HTTP " + std::to_string(h.status);
    return false;
  }
  Candidate copy = c;
  if (!parse_release(h.body, copy, error)) return false;
  c = std::move(copy);
  return true;
}

}  // namespace deckhand::metadata
