#include "deckhand/app/metadata_job.hpp"

#include <algorithm>

namespace deckhand {

MetadataJob::MetadataJob(const std::string& db_path, std::shared_ptr<metadata::Transport> t, std::shared_ptr<metadata::Clock> c,
                         metadata::RatePolicy rate)
    : db_path_(db_path), transport_(std::move(t)), clock_(std::move(c)), rate_(rate), lib_(new Library(db_path)) {}

MetadataJob::~MetadataJob() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
}

bool MetadataJob::enabled() {
  std::lock_guard<std::mutex> lock(lib_m_);
  return metadata::enabled(*lib_);
}

void MetadataJob::set_enabled(bool on) {
  if (!on) cancel();
  std::lock_guard<std::mutex> lock(lib_m_);
  metadata::set_enabled(*lib_, on);
}

bool MetadataJob::start(const std::vector<std::int64_t>& ids, const metadata::FillOptions& o, std::string& error) {
  if (!enabled()) {
    error = "metadata lookup is off: enable it in the settings first (it sends artist and title of the selected tracks to MusicBrainz)";
    return false;
  }
  if (ids.empty()) {
    error = "no tracks to look up";
    return false;
  }
  if (running_) {
    error = "a lookup is already running";
    return false;
  }
  if (worker_.joinable()) worker_.join();
  {
    std::lock_guard<std::mutex> lock(m_);
    tracks_.clear();
    done_ = 0;
    total_ = ids.size();
    note_.clear();
  }
  cancel_ = false;
  running_ = true;
  worker_ = std::thread([this, ids, o] { run(ids, o); });
  return true;
}

void MetadataJob::run(std::vector<std::int64_t> ids, metadata::FillOptions o) {
  std::vector<metadata::TrackProposals> out;
  {
    // One connection for the whole lookup; apply() and undo() wait for it (the lookup is the only long part and can be cancelled).
    std::lock_guard<std::mutex> lock(lib_m_);
    metadata::Service svc = transport_ || clock_ ? metadata::Service(*lib_, transport_, clock_, rate_) : metadata::Service(*lib_);
    out = svc.propose(ids, o, [this](std::size_t d, std::size_t t) {
      std::lock_guard<std::mutex> l(m_);
      done_ = d;
      total_ = t;
    }, &cancel_);
    std::vector<Track> tracks;
    for (const auto& tp : out) {
      Track t;
      t.track_id = tp.track_id;
      t.status = tp.status;
      t.message = tp.message;
      t.matched = tp.matched;
      t.confidence = tp.confidence;
      if (const auto row = lib_->track(tp.track_id)) t.label = row->artist + " - " + row->title;
      for (const auto& p : tp.proposals) {
        Row r;
        r.p = p;
        r.accepted = !p.info_only && !p.overwrites;  // a change of an existing value is never accepted by default
        t.rows.push_back(std::move(r));
      }
      tracks.push_back(std::move(t));
    }
    std::lock_guard<std::mutex> l(m_);
    tracks_ = std::move(tracks);
    done_ = ids.size();
    note_ = cancel_ ? "cancelled" : "";
  }
  running_ = false;
}

void MetadataJob::cancel() { cancel_ = true; }

void MetadataJob::wait() {
  if (worker_.joinable()) worker_.join();
}

MetadataJob::Status MetadataJob::status() {
  Status s;
  s.network_built = metadata::network_built();
  s.running = running_;
  s.cancelling = running_ && cancel_;
  {
    std::lock_guard<std::mutex> lock(m_);
    s.done = done_;
    s.total = total_;
    s.last_batch = last_batch_;
    s.note = note_;
    for (const Track& t : tracks_)
      for (const Row& r : t.rows) s.proposals += r.p.info_only ? 0 : 1;
  }
  // The setting is read without blocking on a running lookup: the panel calls this every frame.
  if (lib_m_.try_lock()) {
    s.enabled = metadata::enabled(*lib_);
    lib_m_.unlock();
  } else {
    s.enabled = true;  // a lookup is running, so it was on
  }
  return s;
}

std::vector<MetadataJob::Track> MetadataJob::results() {
  std::lock_guard<std::mutex> lock(m_);
  return tracks_;
}

bool MetadataJob::set_accepted(std::int64_t track_id, const std::string& field, bool accepted) {
  std::lock_guard<std::mutex> lock(m_);
  for (Track& t : tracks_)
    if (t.track_id == track_id)
      for (Row& r : t.rows)
        if (r.p.field == field && !r.p.info_only) {
          r.accepted = accepted;
          return true;
        }
  return false;
}

void MetadataJob::set_all_accepted(bool accepted) {
  std::lock_guard<std::mutex> lock(m_);
  for (Track& t : tracks_)
    for (Row& r : t.rows)
      if (!r.p.info_only) r.accepted = accepted;
}

bool MetadataJob::apply(bool allow_overwrite, metadata::ApplyResult& out, std::string& error) {
  if (running_) {
    error = "wait for the lookup to finish or cancel it";
    return false;
  }
  std::vector<metadata::Proposal> accepted;
  {
    std::lock_guard<std::mutex> lock(m_);
    for (const Track& t : tracks_)
      for (const Row& r : t.rows)
        if (r.accepted && !r.p.info_only) accepted.push_back(r.p);
  }
  if (accepted.empty()) {
    error = "nothing is accepted";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(lib_m_);
    metadata::Service svc(*lib_, std::shared_ptr<metadata::Provider>());  // apply() never uses the provider
    metadata::ApplyOptions ao;
    ao.allow_overwrite = allow_overwrite;
    out = svc.apply(accepted, ao);
  }
  std::lock_guard<std::mutex> lock(m_);
  if (out.batch_id > 0) last_batch_ = out.batch_id;
  // The applied rows leave the list; skipped ones (overwrites without permission) stay for review.
  for (Track& t : tracks_)
    t.rows.erase(std::remove_if(t.rows.begin(), t.rows.end(),
                                [&](const Row& r) { return r.accepted && !r.p.info_only && (allow_overwrite || !r.p.overwrites); }),
                 t.rows.end());
  note_.clear();
  return true;
}

bool MetadataJob::undo_last(UndoResult& out, std::string& error) {
  std::int64_t batch;
  {
    std::lock_guard<std::mutex> lock(m_);
    batch = last_batch_;
  }
  if (batch <= 0) {
    error = "nothing to undo";
    return false;
  }
  std::lock_guard<std::mutex> lock(lib_m_);
  metadata::Service svc(*lib_, std::shared_ptr<metadata::Provider>());
  out = svc.undo(batch);
  if (out.status == UndoStatus::Ok) {
    std::lock_guard<std::mutex> l(m_);
    last_batch_ = 0;
  }
  return true;
}

void MetadataJob::clear() {
  if (running_) return;
  std::lock_guard<std::mutex> lock(m_);
  tracks_.clear();
  done_ = total_ = 0;
  note_.clear();
}

}  // namespace deckhand
