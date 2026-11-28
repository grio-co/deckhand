#pragma once
// The application side of the opt-in metadata fill (docs/METADATA.md): one lookup at a time on a worker thread with progress and
// cancel, the proposals kept for review, accepting or rejecting each, applying them as one journal batch and undoing the last batch.
// The panel (ui.metadata) and the REST routes drive the same object. Nothing is looked up unless metadata::enabled() is true.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "deckhand/metadata/metadata.hpp"

namespace deckhand {

class MetadataJob {
 public:
  struct Status {
    bool enabled = false;       // the opt-in setting
    bool network_built = false; // this build can make requests
    bool running = false;
    bool cancelling = false;
    std::size_t done = 0, total = 0;
    std::size_t proposals = 0;  // proposals waiting for review (applicable ones)
    std::int64_t last_batch = 0;
    std::string note;           // last message ("" = none)
  };
  struct Row {                  // one proposal with the review state
    metadata::Proposal p;
    bool accepted = true;
  };
  struct Track {
    std::int64_t track_id = 0;
    std::string label;          // "artist - title" as stored
    metadata::Status status = metadata::Status::Skipped;
    std::string message, matched;
    double confidence = 0;
    std::vector<Row> rows;
  };

  // db_path: the library file (the job opens its own connection). Tests inject the canned transport and clock.
  explicit MetadataJob(const std::string& db_path, std::shared_ptr<metadata::Transport> t = nullptr, std::shared_ptr<metadata::Clock> c = nullptr,
                       metadata::RatePolicy rate = {});
  ~MetadataJob();
  MetadataJob(const MetadataJob&) = delete;
  MetadataJob& operator=(const MetadataJob&) = delete;

  bool enabled();
  void set_enabled(bool on);
  // Starts a lookup of these tracks. False with error when the opt-in is off, a lookup is running or there is nothing to look up.
  bool start(const std::vector<std::int64_t>& track_ids, const metadata::FillOptions& o, std::string& error);
  void cancel();
  void wait();  // blocks until the worker is idle
  Status status();
  std::vector<Track> results();
  // Review: accept or reject one proposal (track id and field) or all of them.
  bool set_accepted(std::int64_t track_id, const std::string& field, bool accepted);
  void set_all_accepted(bool accepted);
  // Writes the accepted, applicable proposals as one journal batch (source "metadata"). Applied proposals leave the list.
  bool apply(bool allow_overwrite, metadata::ApplyResult& out, std::string& error);
  // Reverts the last applied batch.
  bool undo_last(UndoResult& out, std::string& error);
  void clear();

 private:
  void run(std::vector<std::int64_t> ids, metadata::FillOptions o);
  std::string db_path_;
  std::shared_ptr<metadata::Transport> transport_;
  std::shared_ptr<metadata::Clock> clock_;
  metadata::RatePolicy rate_;
  std::mutex m_;
  std::unique_ptr<Library> lib_;  // guarded by lib_m_ (the worker and the callers share it)
  std::mutex lib_m_;
  std::thread worker_;
  std::atomic<bool> cancel_{false}, running_{false};
  std::size_t done_ = 0, total_ = 0;
  std::vector<Track> tracks_;
  std::int64_t last_batch_ = 0;
  std::string note_;
};

}  // namespace deckhand
