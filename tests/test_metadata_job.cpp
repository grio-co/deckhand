// The application side of the metadata fill: the opt-in gate, the worker thread with progress and cancel, review, apply as one journal
// batch with source "metadata", and undo. The transport and clock are canned: nothing here can reach the network.
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/app/metadata_job.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

namespace {

struct FakeClock : metadata::Clock {
  std::int64_t t = 100000;
  std::int64_t now_ms() override { return t; }
  void sleep_ms(std::int64_t ms) override { t += ms; }
};

struct FakeTransport : metadata::Transport {
  std::mutex m;
  std::vector<std::string> urls, agents;
  std::function<void()> on_first;  // runs inside the first request (to cancel mid-lookup)
  metadata::HttpResponse get(const metadata::HttpRequest& r) override {
    {
      std::lock_guard<std::mutex> lock(m);
      urls.push_back(r.url);
      agents.push_back(r.user_agent);
    }
    if (on_first) {
      auto f = std::move(on_first);
      on_first = nullptr;
      f();
    }
    metadata::HttpResponse x;
    x.status = 200;
    if (r.url.find("/recording?") != std::string::npos)
      x.body = R"J({"count":1,"offset":0,"recordings":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","score":100,"title":"Nightfall","length":241000,
  "artist-credit":[{"name":"Aurora Vale","artist":{"name":"Aurora Vale"}}],"tags":[{"count":2,"name":"house"}],
  "releases":[{"id":"rel-orig","title":"Dusk Tapes","status":"Official","date":"2016-03-09","release-group":{"primary-type":"Album","secondary-types":[]}}]}]})J";
    else
      x.body = R"J({"id":"rel-orig","title":"Dusk Tapes","status":"Official","date":"2016-03-09","genres":[{"count":5,"name":"house"}]})J";
    return x;
  }
};

struct Env {
  std::filesystem::path dir;
  std::string db;
  std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
  std::shared_ptr<FakeTransport> tr = std::make_shared<FakeTransport>();
  std::unique_ptr<Library> lib;
  explicit Env(const char* name) {
    dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    db = (dir / "library.db").string();
    lib = std::make_unique<Library>(db);
  }
  ~Env() {
    lib.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::int64_t add(const char* file, const char* title, const char* artist) {
    NewTrack t;
    t.path = (dir / file).string();
    t.title = title;
    t.artist = artist;
    t.duration_ms = 241000;
    return lib->upsert_track(t);
  }
};

}  // namespace

int main() {
  {
    Env e("deckhand_test_metadata_job");
    const auto a = e.add("a.mp3", "Nightfall", "Aurora Vale");
    const auto b = e.add("b.mp3", "Nightfall", "Aurora Vale");
    MetadataJob job(e.db, e.tr, e.clock);
    // off by default: refused, no request
    CHECK(!job.enabled());
    std::string err;
    CHECK(!job.start({a}, {}, err) && !err.empty() && e.tr->urls.empty());
    CHECK(!job.status().enabled);
    // on: a lookup runs on the worker thread and ends with proposals; nothing is written yet
    job.set_enabled(true);
    CHECK(job.enabled() && job.status().enabled);
    CHECK(job.start({a, b}, {}, err));
    job.wait();
    const auto st = job.status();
    CHECK(!st.running && st.done == 2 && st.total == 2);
    const auto res = job.results();
    CHECK(res.size() == 2 && res[0].status == metadata::Status::Ok && !res[0].rows.empty());
    for (const auto& ag : e.tr->agents) CHECK(ag.rfind("DeckHand/", 0) == 0 && ag.find(' ') == std::string::npos && ag.find("http") == std::string::npos);
    CHECK(e.lib->track(a)->album.empty());
    // review: reject the first track's album, accept the rest
    CHECK(job.set_accepted(a, "album", false));
    CHECK(!job.set_accepted(a, "nonsense", true));
    metadata::ApplyResult ar;
    CHECK(job.apply(false, ar, err));
    CHECK(ar.applied > 0 && ar.batch_id > 0);
    CHECK(e.lib->track(a)->album.empty() && e.lib->track(b)->album == "Dusk Tapes");
    bool seen = false;
    for (const auto& r : e.lib->recent_edits(20)) seen = seen || r.source == "metadata";
    CHECK(seen);
    CHECK(job.status().last_batch == ar.batch_id);
    // undo restores the batch
    UndoResult ur;
    CHECK(job.undo_last(ur, err) && ur.status == UndoStatus::Ok);
    CHECK(e.lib->track(b)->album.empty());
    CHECK(!job.undo_last(ur, err));  // nothing more to undo
    // turning it off refuses a new lookup
    job.set_enabled(false);
    CHECK(!job.start({a}, {}, err));
  }
  {
    // cancel in the middle: the track being looked up finishes, the rest are marked cancelled
    Env e("deckhand_test_metadata_job_cancel");
    std::vector<std::int64_t> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(e.add(("t" + std::to_string(i) + ".mp3").c_str(), "Nightfall", "Aurora Vale"));
    MetadataJob job(e.db, e.tr, e.clock);
    job.set_enabled(true);
    e.tr->on_first = [&] { job.cancel(); };
    std::string err;
    CHECK(job.start(ids, {}, err));
    job.wait();
    const auto res = job.results();
    CHECK(res.size() == 3 && res[2].status == metadata::Status::Cancelled);
    CHECK(!job.status().running);
    metadata::ApplyResult ar;
    CHECK(job.apply(false, ar, err));  // what was found can still be applied
  }
  std::puts("metadata job ok");
  return 0;
}
