// Metadata fill: normalisation, rate limiting and back-off (fake clock), response parsing, matching, proposals, apply and undo.
// Nothing here touches the network: the transport is a fake that returns small hand-written JSON about made-up artists.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/metadata/metadata.hpp"

using namespace deckhand;
using namespace deckhand::metadata;

namespace {

struct FakeClock : Clock {
  std::int64_t t = 100000;
  std::int64_t now_ms() override { return t; }
  void sleep_ms(std::int64_t ms) override { t += ms; }
};

struct FakeTransport : Transport {
  struct Hit { std::string url, ua; std::int64_t at; };
  std::shared_ptr<FakeClock> clock;
  std::vector<Hit> hits;
  std::vector<HttpResponse> script;  // consumed in order when non-empty
  std::string search_body, release_body;
  int search_status = 200;
  HttpResponse get(const HttpRequest& r) override {
    hits.push_back({r.url, r.user_agent, clock ? clock->t : 0});
    if (!script.empty()) {
      HttpResponse x = script.front();
      script.erase(script.begin());
      return x;
    }
    HttpResponse x;
    if (r.url.find("/recording?") != std::string::npos) {
      x.status = search_status;
      x.body = search_body;
    } else {
      x.status = 200;
      x.body = release_body;
    }
    return x;
  }
};

const char* kSearch = R"J({"created":"2020-01-01T00:00:00Z","count":3,"offset":0,"recordings":[
 {"id":"aaaaaaaa-0000-0000-0000-000000000001","score":100,"title":"Nightfall","length":241000,
  "artist-credit":[{"name":"Aurora Vale","artist":{"id":"x","name":"Aurora Vale"}}],
  "isrcs":["XX1AB1600001"],"tags":[{"count":2,"name":"house"},{"count":1,"name":"downtempo"}],
  "releases":[
   {"id":"rel-comp","title":"Club Hits Vol. 3","status":"Official","date":"2019-05-01","release-group":{"primary-type":"Album","secondary-types":["Compilation"]}},
   {"id":"rel-orig","title":"Dusk Tapes","status":"Official","date":"2016-03-09","release-group":{"primary-type":"Album","secondary-types":[]},
    "media":[{"position":1,"track-count":9,"track":[{"id":"t","number":"3","title":"Nightfall"}]}]}]},
 {"id":"aaaaaaaa-0000-0000-0000-000000000002","score":90,"title":"Nightfall (Extended Mix)","length":421000,
  "artist-credit":[{"name":"Aurora Vale","artist":{"name":"Aurora Vale"}}],
  "releases":[{"id":"rel-ext","title":"Nightfall","status":"Official","date":"2016-04-01","release-group":{"primary-type":"Single"}}]},
 {"id":"aaaaaaaa-0000-0000-0000-000000000003","score":60,"title":"Night Falls","length":200000,
  "artist-credit":[{"name":"Somebody Else"}],"releases":[]}]})J";

const char* kRelease = R"J({"id":"rel-orig","title":"Dusk Tapes","status":"Official","date":"2016-03-09",
 "label-info":[{"catalog-number":"MR001","label":{"id":"l","name":"Moth Records"}}],
 "genres":[{"count":5,"name":"house"},{"count":3,"name":"deep house"}],
 "media":[{"position":1,"tracks":[{"number":"3","position":3,"title":"Nightfall","recording":{"id":"aaaaaaaa-0000-0000-0000-000000000001"}}]}]})J";

TrackEdit edit_album(const char* v) { TrackEdit t; t.album = v; return t; }
TrackEdit edit_genre(const char* v) { TrackEdit t; t.genre = v; return t; }

struct Env {
  std::filesystem::path dir;
  std::unique_ptr<Library> lib;
  std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
  std::shared_ptr<FakeTransport> tr = std::make_shared<FakeTransport>();
  explicit Env(const char* name) {
    dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    lib = std::make_unique<Library>((dir / "library.db").string());
    tr->clock = clock;
    tr->search_body = kSearch;
    tr->release_body = kRelease;
  }
  ~Env() {
    lib.reset();  // close the database before deleting its folder
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::int64_t add(const char* file, const char* title, const char* artist, std::int64_t dur, const char* album = "") {
    NewTrack t;
    t.path = (dir / file).string();
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.duration_ms = dur;
    return lib->upsert_track(t);
  }
  Service service() { return Service(*lib, tr, clock); }
};

const Proposal* find(const TrackProposals& t, const char* field) {
  for (const auto& p : t.proposals)
    if (p.field == field) return &p;
  return nullptr;
}

void test_normalise() {
  CHECK(title_similarity("Café del Mar (Remastered 2011)", "Cafe Del Mar") > 0.99);
  CHECK(title_similarity("Song feat. Someone", "Song") > 0.99);
  CHECK(title_similarity("Song (Original Mix)", "SONG") > 0.99);
  CHECK(title_similarity("Song - Radio Edit", "Song") > 0.99);
  CHECK(title_similarity("Song (Club Remix)", "Song") < 0.8);                   // a different recording
  CHECK(title_similarity("Song (Club Remix)", "Song [Club Remix]") > 0.99);
  CHECK(title_similarity("Don't Stop", "Dont Stop!") > 0.99);
  CHECK(title_similarity("Night Fall", "Fall Night") > 0.9);                    // word order
  CHECK(title_similarity("Nightfall", "Sunrise") < 0.5);
  CHECK(artist_similarity("The Midnight Choir", "Midnight Choir") > 0.99);
  CHECK(artist_similarity("Zoë Ångström", "Zoe Angstrom") > 0.99);
  CHECK(artist_similarity("Aurora Vale & Kit", "Aurora Vale") >= 0.9);
  CHECK(artist_similarity("Aurora Vale feat. Kit", "Aurora Vale") >= 0.9);
  CHECK(artist_similarity("Kit and Aurora Vale", "Aurora Vale, Kit") > 0.99);
  CHECK(artist_similarity("Aurora Vale", "Someone Else") < 0.5);
  CHECK(artist_similarity("", "x") == 0);
  CHECK(percent_encode("a b&\"é") == "a%20b%26%22%C3%A9");
}

void test_rate_and_backoff() {
  auto clock = std::make_shared<FakeClock>();
  auto tr = std::make_shared<FakeTransport>();
  tr->clock = clock;
  RateLimitedClient c(tr, clock, user_agent());
  HttpResponse ok;
  ok.status = 200;
  for (int i = 0; i < 3; ++i) c.get("https://example.invalid/x");  // fake transport: no network involved
  CHECK(tr->hits.size() == 3);
  CHECK(tr->hits[1].at - tr->hits[0].at >= 1000);
  CHECK(tr->hits[2].at - tr->hits[1].at >= 1000);
  // 503 twice, then success: waits grow, Retry-After is honoured
  tr->hits.clear();
  HttpResponse busy;
  busy.status = 503;
  HttpResponse busy10 = busy;
  busy10.retry_after = "10";
  HttpResponse good;
  good.status = 200;
  good.body = "{}";
  tr->script = {busy, busy10, good};
  const auto r = c.get("https://example.invalid/y");
  CHECK(r.status == 200 && tr->hits.size() == 3);
  CHECK(tr->hits[1].at - tr->hits[0].at >= 2000);
  CHECK(tr->hits[2].at - tr->hits[1].at >= 10000);
  // permanent 503: gives up after max_retries + 1 attempts with a message
  tr->hits.clear();
  tr->script = {busy, busy, busy, busy, busy, busy};
  const auto g = c.get("https://example.invalid/z");
  CHECK(g.status == 503 && !g.error.empty() && tr->hits.size() == 4);
  // cancel: no request
  tr->hits.clear();
  std::atomic<bool> cancel{true};
  CHECK(c.get("https://example.invalid/c", &cancel).error == "cancelled" && tr->hits.empty());
  {
    const std::string ua = user_agent();
    CHECK(ua.rfind("DeckHand/", 0) == 0);
    CHECK(ua.find_first_of(" @:()/", 9) == std::string::npos);  // version only: no URL, no contact, no comment
    CHECK(ua.find("http") == std::string::npos && ua.find("github") == std::string::npos);
  }
}

void test_parse() {
  const auto r = MusicBrainz::parse_search(kSearch);
  CHECK(r.ok && r.candidates.size() == 4);  // 2 releases + 1 + 1 without releases
  CHECK(r.candidates[0].album == "Club Hits Vol. 3" && r.candidates[0].secondary_types.count("Compilation"));
  CHECK(r.candidates[1].year == 2016 && r.candidates[1].track_number == 3 && r.candidates[1].isrc == "XX1AB1600001");
  CHECK(r.candidates[1].genres.size() == 2 && r.candidates[1].genres[0] == "house");
  CHECK(!MusicBrainz::parse_search("not json").ok);
  CHECK(!MusicBrainz::parse_search("{\"error\":\"nope\"}").ok);
  CHECK(!MusicBrainz::parse_search("[1,2]").ok);
  CHECK(!MusicBrainz::parse_search("{\"recordings\":5}").ok);
  CHECK(MusicBrainz::parse_search("{\"recordings\":[]}").ok);
  CHECK(MusicBrainz::parse_search("{\"recordings\":[5,null,{\"title\":\"x\"}]}").candidates.empty());
  Candidate c = r.candidates[1];
  std::string err;
  CHECK(MusicBrainz::parse_release(kRelease, c, &err) && c.label == "Moth Records" && c.genres[0] == "house" && c.genres[1] == "deep house");
  CHECK(!MusicBrainz::parse_release("{\"oops\":1}", c, &err));
  auto clock = std::make_shared<FakeClock>();
  MusicBrainz mb(std::make_shared<RateLimitedClient>(std::make_shared<FakeTransport>(), clock, "x"));
  TrackQuery q{"Aurora \"Vale\"", "Night\\fall", "Secret Album", 241000};
  const std::string u = mb.search_url(q);
  CHECK(u.rfind("https://musicbrainz.org/ws/2/recording?query=", 0) == 0 && u.find("fmt=json") != std::string::npos);
  CHECK(u.find("Secret") == std::string::npos && u.find("241000") == std::string::npos);  // album and duration are not sent
  CHECK(mb.release_url("a/b").find("release/a%2Fb?") != std::string::npos);
}

void test_matching() {
  TrackQuery q{"Aurora Vale", "Nightfall", "", 241500};
  const auto s = score_candidates(q, MusicBrainz::parse_search(kSearch).candidates, MatchOptions{});
  CHECK(!s.empty());
  CHECK(s.front().c.release_id == "rel-orig");  // original release before the compilation
  CHECK(s.front().confidence > 0.8);
  for (const auto& x : s) CHECK(x.c.title == "Nightfall");  // extended mix (different length and qualifier) and the other artist are dropped
  q.duration_ms = 0;  // unknown duration: still matches, neutral
  CHECK(!score_candidates(q, MusicBrainz::parse_search(kSearch).candidates, MatchOptions{}).empty());
}

void test_proposals_apply_undo() {
  Env e("deckhand_test_metadata_fill");
  const auto id = e.add("a.mp3", "Nightfall", "Aurora Vale", 241500);
  Service svc = e.service();
  CHECK(!svc.enabled());
  auto off = svc.propose({id});  // opt-in is off: no request at all
  CHECK(off.size() == 1 && off[0].status == Status::Disabled && e.tr->hits.empty());
  set_enabled(*e.lib, true);
  CHECK(svc.enabled());

  const auto res = svc.propose({id});
  CHECK(res.size() == 1 && res[0].status == Status::Ok);
  CHECK(e.tr->hits.size() == 2);
  CHECK(e.clock->t - 100000 >= 1000);  // second request waited for the rate limit
  for (const auto& h : e.tr->hits) {
    CHECK(h.url.rfind("https://musicbrainz.org/ws/2/", 0) == 0);
    CHECK(h.url.find(e.dir.string()) == std::string::npos && h.url.find("mp3") == std::string::npos);  // no path
    CHECK(h.ua.rfind("DeckHand/", 0) == 0);
  }
  const auto& t = res[0];
  CHECK(find(t, "album") && find(t, "album")->proposed == "Dusk Tapes" && find(t, "album")->current.empty());
  CHECK(find(t, "year") && find(t, "year")->proposed == "2016");
  CHECK(find(t, "genre") && find(t, "genre")->proposed == "House");
  CHECK(find(t, "label") && find(t, "label")->info_only && find(t, "label")->proposed == "Moth Records");
  CHECK(find(t, "track_number") && find(t, "track_number")->proposed == "3");
  CHECK(find(t, "isrc") && find(t, "isrc")->proposed == "XX1AB1600001");
  CHECK(!find(t, "title") && !find(t, "artist") && find(t, "album")->confidence > 0.8);
  CHECK(find(t, "album")->source_url == "https://musicbrainz.org/recording/aaaaaaaa-0000-0000-0000-000000000001");
  // nothing written yet
  CHECK(e.lib->track(id)->album.empty());

  // accept album and year only (plus an info-only one, which is refused)
  std::vector<Proposal> accepted = {*find(t, "album"), *find(t, "year"), *find(t, "label")};
  auto stale = *find(t, "genre");
  stale.current = "Something";  // the field changed after the lookup
  accepted.push_back(stale);
  const auto ar = svc.apply(accepted);
  CHECK(ar.applied == 2 && ar.skipped == 2 && ar.batch_id > 0);
  CHECK(e.lib->track(id)->album == "Dusk Tapes" && e.lib->track(id)->genre.empty());
  const auto hist = e.lib->track_history(id);
  CHECK(!hist.empty() && hist[0].source == kSource && hist[0].batch_id == ar.batch_id);
  const auto ur = svc.undo(ar.batch_id);
  CHECK(ur.status == UndoStatus::Ok && e.lib->track(id)->album.empty());

  // a second lookup after accepting: non-empty fields are not proposed again
  svc.apply({*find(t, "album")});
  const auto again = svc.propose({id});
  CHECK(!find(again[0], "album"));
  FillOptions ow;
  ow.overwrite_nonempty = true;
  e.lib->edit_track(id, edit_album("My Own Album"));
  const auto over = svc.propose({id}, ow);
  CHECK(!find(over[0], "album"));  // hand-edited: left alone
  ow.overwrite_edited = true;
  const auto over2 = svc.propose({id}, ow);
  CHECK(find(over2[0], "album") && find(over2[0], "album")->overwrites && find(over2[0], "album")->edited_by_hand);
  CHECK(svc.apply({*find(over2[0], "album")}).applied == 0);  // caller did not allow overwriting
  ApplyOptions allow;
  allow.allow_overwrite = true;
  allow.overwrite_edited = true;
  CHECK(svc.apply({*find(over2[0], "album")}, allow).applied == 1 && e.lib->track(id)->album == "Dusk Tapes");
}

void test_edge_cases() {
  Env e("deckhand_test_metadata_edges");
  set_enabled(*e.lib, true);
  Service svc = e.service();
  const auto a = e.add("a.mp3", "Nightfall", "Aurora Vale", 400000);    // far longer than the database recording
  const auto b = e.add("b.mp3", "Nightfall", "Someone Else", 241000);   // other artist
  const auto c = e.add("c.mp3", "", "Aurora Vale", 241000);             // no title
  const auto d = e.add("d.mp3", "Nightfall", "Aurora Vale", 241000);
  auto r = svc.propose({a, b, c, 9999});
  CHECK(r[0].status == Status::NoMatch && r[1].status == Status::NoMatch && r[2].status == Status::Skipped && r[3].status == Status::Skipped);
  CHECK(e.tr->hits.size() == 2);  // skipped tracks cost no request
  // threshold: a very high minimum refuses to propose
  FillOptions strict;
  strict.match.min_confidence = 0.99;
  r = svc.propose({d}, strict);
  CHECK(r[0].status == Status::LowConfidence && r[0].proposals.empty());
  // service errors and bad JSON become Error, never an exception
  e.tr->search_status = 500;
  CHECK(svc.propose({d})[0].status == Status::Error);
  e.tr->search_status = 200;
  e.tr->search_body = "{garbage";
  CHECK(svc.propose({d})[0].status == Status::Error);
  e.tr->search_body = kSearch;
  e.tr->release_body = "nope";  // release lookup fails: proposals still come, from the search result
  const auto soft = svc.propose({d});
  CHECK(soft[0].status == Status::Ok && find(soft[0], "album") && !soft[0].message.empty());
  // progress and cancel
  std::size_t last = 0;
  std::atomic<bool> cancel{false};
  e.tr->release_body = kRelease;
  const auto pr = svc.propose({d, d}, {}, [&](std::size_t done, std::size_t) { last = done; cancel = true; }, &cancel);
  CHECK(last == 2 && pr[1].status == Status::Cancelled);
  // a hand-cleared field stays untouched
  e.lib->edit_track(d, edit_genre("Techno"));
  e.lib->edit_track(d, edit_genre(""));
  CHECK(!find(svc.propose({d})[0], "genre"));
}

void test_stub_or_built() {
  auto t = make_https_transport();
  CHECK(t != nullptr);
  if (!network_built()) {
    HttpRequest rq;
    rq.url = "https://musicbrainz.org/ws/2/recording?query=x";
    const auto r = t->get(rq);
    CHECK(r.status == 0 && !r.error.empty());
    Env e("deckhand_test_metadata_stub");
    set_enabled(*e.lib, true);
    const auto id = e.add("a.mp3", "Nightfall", "Aurora Vale", 241000);
    Service svc(*e.lib);  // default transport: the stub
    CHECK(svc.propose({id})[0].status == Status::Error);
  }
}

}  // namespace

int main() {
  test_normalise();
  test_rate_and_backoff();
  test_parse();
  test_matching();
  test_proposals_apply_undo();
  test_edge_cases();
  test_stub_or_built();
  std::printf("metadata fill tests passed\n");
  return 0;
}
