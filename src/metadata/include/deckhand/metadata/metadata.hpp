#pragma once
// Opt-in metadata fill from an open music database (MusicBrainz).
//
// Off by default. Nothing leaves the machine unless the setting kSettingEnabled is "1" and the user selected tracks to look up. What
// is sent for each track: its artist and title, in the query string of one HTTPS request (and the id of the chosen release in a
// second one). Album and duration stay on this machine and are used to pick between the answers. File paths and the library
// are never sent. Nothing is written to the library until the caller passes accepted proposals to Service::apply().
//
// The database holds no musical key: key stays with local analysis.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand::metadata {

constexpr const char* kSettingEnabled = "metadata.musicbrainz.enabled";  // "1" = lookups allowed; anything else = off (the default)
constexpr const char* kSource = "metadata";                               // journal source of the edits apply() makes
constexpr const char* kBaseUrl = "https://musicbrainz.org/ws/2/";

// True when this build can make requests (DECKHAND_WITH_METADATA and a TLS library); false in the stub build.
bool network_built();
// Exactly "DeckHand/<version>": no URL, no contact details.
std::string user_agent();
bool enabled(Library& lib);
void set_enabled(Library& lib, bool on);

// ---- text normalisation and similarity (normalize.cpp) ---------------------------------------------------------------
struct Normalized {
  std::string base;               // lower case, no diacritics or punctuation, no "the", no feat./remaster suffixes
  std::set<std::string> variant;  // remix / edit / live / dub ... qualifiers that make a different recording
};
Normalized normalize_title(const std::string& title);
// Artist names: "&", "and", "feat." and "," split the credit into a sorted token set; "the" is dropped.
std::string normalize_artist(const std::string& artist);
double title_similarity(const std::string& a, const std::string& b);   // 0..1; differing variants cost 25 %
double artist_similarity(const std::string& a, const std::string& b);  // 0..1
std::string percent_encode(const std::string& s);

// ---- transport and clock (injected in tests) -----------------------------------------------------------------------------
struct HttpRequest {
  std::string url;  // https only
  std::string user_agent;
  int timeout_ms = 15000;
  std::size_t max_bytes = 2u << 20;  // response size cap
};
struct HttpResponse {
  int status = 0;            // 0 = no response (see error)
  std::string body;
  std::string retry_after;   // header value, seconds
  std::string error;         // transport-level failure text
};
class Transport {
 public:
  virtual ~Transport() = default;
  virtual HttpResponse get(const HttpRequest& r) = 0;
};
// HTTPS GET over OpenSSL: certificates and host name are verified against the system trust store, at most 3 redirects (https, same
// host), response capped. Returns a transport that always fails with "not built" in the stub build.
std::shared_ptr<Transport> make_https_transport(const std::string& ca_file = {});

class Clock {
 public:
  virtual ~Clock() = default;
  virtual std::int64_t now_ms() = 0;  // monotonic
  virtual void sleep_ms(std::int64_t ms) = 0;
};
std::shared_ptr<Clock> system_clock();

// ---- rate-limited client -------------------------------------------------------------------------------------------------
struct RatePolicy {
  std::int64_t min_interval_ms = 1000;  // one request per second
  int max_retries = 3;                  // after a 503 or 429
  std::int64_t backoff_base_ms = 2000;  // doubles per retry, or Retry-After when longer
  std::int64_t backoff_cap_ms = 30000;
};
class RateLimitedClient {
 public:
  RateLimitedClient(std::shared_ptr<Transport> t, std::shared_ptr<Clock> c, std::string user_agent, RatePolicy p = {});
  HttpResponse get(const std::string& url, const std::atomic<bool>* cancel = nullptr);
  int requests_made() const { return requests_; }

 private:
  std::shared_ptr<Transport> t_;
  std::shared_ptr<Clock> c_;
  std::string ua_;
  RatePolicy p_;
  std::int64_t last_start_ = 0;
  bool started_ = false;
  int requests_ = 0;
};

// ---- provider --------------------------------------------------------------------------------------------------------------
// The only data about a track that a provider may be given.
struct TrackQuery {
  std::string artist, title;
  std::string album;             // used locally to choose a release; never sent
  std::int64_t duration_ms = 0;  // used locally; never sent
};
// One recording on one release.
struct Candidate {
  std::string recording_id, release_id;
  std::string title, artist, album, label, isrc;
  std::string status;                      // Official, Promotion, Bootleg ...
  std::string primary_type;                // Album, Single, EP ...
  std::set<std::string> secondary_types;   // Compilation, Live, Remix ...
  int year = 0, track_number = 0;
  std::int64_t duration_ms = 0;
  std::vector<std::string> genres;         // most voted first
  std::string source_url;                  // page of the recording on the database
};
struct SearchResult {
  bool ok = false;
  std::string error;
  std::vector<Candidate> candidates;  // one per (recording, release); a recording without releases yields one with no album
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  virtual SearchResult search(const TrackQuery& q, const std::atomic<bool>* cancel) = 0;
  // Fills album-level detail (label, genres, track number, year) of one candidate. False on failure; the candidate is kept as it was.
  virtual bool detail(Candidate& c, const std::atomic<bool>* cancel, std::string* error) = 0;
};

class MusicBrainz : public Provider {
 public:
  MusicBrainz(std::shared_ptr<RateLimitedClient> http, std::string base_url = kBaseUrl, int limit = 10);
  std::string name() const override { return "MusicBrainz"; }
  SearchResult search(const TrackQuery& q, const std::atomic<bool>* cancel) override;
  bool detail(Candidate& c, const std::atomic<bool>* cancel, std::string* error) override;
  // Pure parts, exposed for tests.
  std::string search_url(const TrackQuery& q) const;
  std::string release_url(const std::string& release_id) const;
  static SearchResult parse_search(const std::string& json);
  static bool parse_release(const std::string& json, Candidate& c, std::string* error);

 private:
  std::shared_ptr<RateLimitedClient> http_;
  std::string base_;
  int limit_;
};

// ---- matching --------------------------------------------------------------------------------------------------------------
struct MatchOptions {
  double min_confidence = 0.80;
  std::int64_t duration_tolerance_ms = 5000;  // a candidate further off is a different version and is dropped (unless duration is unknown)
  double min_title = 0.85, min_artist = 0.70;
};
struct Scored {
  Candidate c;
  double confidence = 0;  // 0..1
  double rank = 0;        // confidence minus penalties for compilations, live and unofficial releases
  double title = 0, artist = 0, duration = 0, album = 0;
};
// Every candidate that passes the gates, best first.
std::vector<Scored> score_candidates(const TrackQuery& q, const std::vector<Candidate>& c, const MatchOptions& o);

// ---- proposals -------------------------------------------------------------------------------------------------------------
struct Proposal {
  std::int64_t track_id = 0;
  std::string field;     // album, year, genre, title, artist (applicable); label, isrc, track_number (information only)
  std::string current;   // as stored ("" = empty)
  std::string proposed;
  double confidence = 0;
  std::string source_url;
  std::string provider;
  bool info_only = false;      // the library has no column for it: shown, never applied
  bool overwrites = false;     // current is not empty
  bool edited_by_hand = false; // the journal shows the user (or the API) set this field
};
enum class Status { Ok, Disabled, Skipped, NoMatch, LowConfidence, Error, Cancelled };
struct TrackProposals {
  std::int64_t track_id = 0;
  Status status = Status::Skipped;
  std::string message;
  double confidence = 0;
  std::string matched;  // "artist - title" the database holds
  std::vector<Proposal> proposals;
};
struct FillOptions {
  MatchOptions match;
  bool overwrite_nonempty = false;  // also propose a change for a field that already has a value
  bool overwrite_edited = false;    // ... even one the user edited by hand
  std::set<std::string> fields = {"album", "year", "genre"};  // title and artist are only proposed when listed here
  bool lookup_release = true;       // second request for label, genre, track number and year
};
struct ApplyOptions {
  bool allow_overwrite = false;     // proposals with overwrites = true are skipped unless set
  bool overwrite_edited = false;
};
struct ApplyResult {
  int applied = 0, skipped = 0;
  std::int64_t batch_id = 0;                 // journal batch: Service::undo(batch_id) restores everything applied
  std::vector<std::string> skipped_reasons;  // "<track> <field>: why"
};

using ProgressFn = std::function<void(std::size_t done, std::size_t total)>;

class Service {
 public:
  // Default transport and clock are the real ones; tests inject fakes. The provider is built on them (MusicBrainz).
  explicit Service(Library& lib, std::shared_ptr<Transport> t = nullptr, std::shared_ptr<Clock> c = nullptr, RatePolicy rate = {});
  Service(Library& lib, std::shared_ptr<Provider> provider);

  bool enabled() const;
  // Looks the tracks up (one request per second) and returns proposals; changes nothing. Disabled when the opt-in setting is not "1".
  std::vector<TrackProposals> propose(const std::vector<std::int64_t>& track_ids, const FillOptions& o = {},
                                      const ProgressFn& progress = {}, const std::atomic<bool>* cancel = nullptr);
  // Writes the accepted proposals through Library::edit_track under one journal batch.
  ApplyResult apply(const std::vector<Proposal>& accepted, const ApplyOptions& o = {});
  UndoResult undo(std::int64_t batch_id);

 private:
  Library& lib_;
  std::shared_ptr<Provider> provider_;
};

const char* status_name(Status s);

}  // namespace deckhand::metadata
