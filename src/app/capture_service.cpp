#include "deckhand/app/capture_service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>

#include "deckhand/audio/track.hpp"

namespace deckhand {

namespace fs = std::filesystem;
using namespace capture;

namespace {
float to_db(float lin) { return lin > 1e-10f ? 20.0f * std::log10(lin) : -200.0f; }

std::string stamp_name() {
  const std::time_t t = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char b[48];
  std::strftime(b, sizeof b, "capture-%Y%m%d-%H%M%S", &tmv);
  return b;
}
}  // namespace

CaptureService::CaptureService(IEngine& engine, std::string folder) : CaptureService(engine, std::move(folder), Options{}) {}
CaptureService::CaptureService(IEngine& engine, std::string folder, Options options) : engine_(engine), folder_(std::move(folder)), options_(options) {}

CaptureService::~CaptureService() {
  std::string e;
  stop(e);
  if (analysis_thread_.joinable()) analysis_thread_.join();
}

std::vector<std::string> CaptureService::inputs() { return engine_.input_devices(); }

bool CaptureService::start(const StartArgs& a, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == State::Recording || state_ == State::Paused) {
    error = "already recording";
    return false;
  }
  if (analysing_.load()) {
    error = "the last side is still being analysed";
    return false;
  }
  SampleFormat fmt = SampleFormat::Pcm24;
  if (a.format == "pcm16") fmt = SampleFormat::Pcm16;
  else if (a.format == "float32") fmt = SampleFormat::Float32;
  else if (a.format != "pcm24" && !a.format.empty()) {
    error = "format is pcm16, pcm24 or float32";
    return false;
  }
  we_opened_input_ = false;
  if (options_.open_device) {
    const bool was_open = engine_.input_status().open;
    InputConfig c;
    c.device_hint = a.device;
    c.channel = std::clamp(a.channel, 0, 62);
    c.stereo = a.stereo;
    if (!engine_.open_input(c, error)) return false;
    we_opened_input_ = !was_open;
  }
  const InputStatus st = engine_.input_status();
  if (!st.open || st.sample_rate == 0) {
    error = "no input is open";
    return false;
  }
  std::string name = a.name.empty() ? stamp_name() : a.name;
  for (char& ch : name)
    if (ch == '/' || ch == '\\' || ch == ':') ch = '-';
  std::error_code ec;
  fs::create_directories(fs::u8path(folder_), ec);
  fs::path file = fs::u8path(folder_) / fs::u8path(name + ".wav");
  for (int n = 2; fs::exists(file, ec) && n < 1000; ++n) file = fs::u8path(folder_) / fs::u8path(name + "-" + std::to_string(n) + ".wav");
  auto session = std::make_unique<CaptureSession>();
  CaptureConfig cfg;
  cfg.path = file.string();
  cfg.sample_rate = st.sample_rate;
  cfg.channels = 2;
  cfg.format = fmt;
  if (!session->start(cfg, error)) {
    if (we_opened_input_) engine_.close_input();
    return false;
  }
  session_ = std::move(session);
  path_ = cfg.path;
  rate_ = st.sample_rate;
  input_name_ = st.name;
  error_.clear();
  peak_db_ = hold_db_ = rms_db_ = -200;
  tap_dropped_ = 0;
  analysed_ = false;
  plan_ = SplitPlan{};
  analysis_ = SplitAnalysis{};
  titles_.clear();
  engine_.input_tap_enable(true);
  state_ = State::Recording;
  pump_run_ = true;
  pump_ = std::thread([this] { pump(); });
  return true;
}

void CaptureService::pump() {
  std::vector<float> buf(2 * 2048);
  while (pump_run_.load()) {
    bool got = false;
    for (;;) {
      const std::size_t n = engine_.input_tap_read(buf.data(), 2048);
      if (n == 0) break;
      got = true;
      std::lock_guard<std::mutex> lock(mu_);
      if (session_) session_->push(buf.data(), n);  // a paused session discards it itself
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (session_) {
        const CaptureMeter m = session_->take_meter();
        float pk = 0;
        for (const float p : m.peak) pk = std::max(pk, p);
        if (got || state_ == State::Recording) {
          peak_db_ = to_db(pk);
          rms_db_ = to_db(m.rms);
        }
        hold_db_ = std::max(peak_db_, hold_db_ - 1.0f);  // falls about 60 dB a second at the 15 ms pace
      }
      tap_dropped_ = engine_.input_tap_dropped();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

void CaptureService::stop_pump() {
  pump_run_ = false;
  if (pump_.joinable()) pump_.join();
}

bool CaptureService::set_paused(bool paused, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ != State::Recording && state_ != State::Paused) {
    error = "not recording";
    return false;
  }
  session_->set_paused(paused);
  state_ = paused ? State::Paused : State::Recording;
  return true;
}

bool CaptureService::stop(std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::Recording && state_ != State::Paused) {
      error = "not recording";
      return false;
    }
  }
  engine_.input_tap_enable(false);
  stop_pump();
  std::lock_guard<std::mutex> lock(mu_);
  std::string e;
  const bool ok = session_->stop(e);
  if (!ok) error_ = e;
  state_ = State::Stopped;
  peak_db_ = hold_db_ = rms_db_ = -200;
  if (we_opened_input_) engine_.close_input();  // the input was not open before: leave it as it was
  we_opened_input_ = false;
  if (!ok) error = e;
  return ok;
}

CaptureService::Status CaptureService::status() {
  std::lock_guard<std::mutex> lock(mu_);
  Status s;
  s.state = state_;
  s.path = path_;
  s.input = input_name_;
  s.error = error_;
  s.sample_rate = rate_;
  s.peak_db = peak_db_;
  s.hold_db = hold_db_;
  s.rms_db = rms_db_;
  if (session_) {
    const CaptureStats st = session_->stats();
    s.frames = st.frames;
    s.seconds = st.seconds;
    s.clips = st.clip_samples;
    s.dropped = st.dropped_frames + tap_dropped_;
    if (st.write_error && s.error.empty()) s.error = "the disk write failed";
  }
  s.analysed = analysed_;
  s.analysing = analysing_.load();
  s.points = static_cast<int>(plan_.points.size());
  return s;
}

bool CaptureService::load(const std::string& path, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == State::Recording || state_ == State::Paused) {
    error = "stop the recording first";
    return false;
  }
  WavSource src;
  if (!src.open(path, error)) return false;
  session_.reset();
  path_ = path;
  rate_ = src.sample_rate();
  state_ = State::Stopped;
  analysed_ = false;
  plan_ = SplitPlan{};
  analysis_ = SplitAnalysis{};
  titles_.clear();
  error_.clear();
  return true;
}

bool CaptureService::side_source_open(std::string& error) {
  if (state_ != State::Stopped || path_.empty()) {
    error = "there is no finished recording (stop the recording or load a file first)";
    return false;
  }
  return true;
}

bool CaptureService::analyse(const SplitSettings& settings, std::string& error) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!side_source_open(error)) return false;
    path = path_;
  }
  analysing_ = true;
  WavSource src;
  SplitAnalysis an;
  const bool ok = src.open(path, error);
  if (ok) an = analyse_splits(src, settings);
  std::lock_guard<std::mutex> lock(mu_);
  analysing_ = false;
  if (!ok) return false;
  analysis_ = std::move(an);
  settings_ = settings;
  plan_.sample_rate = analysis_.sample_rate;
  plan_.set_auto(find_splits(analysis_, settings));
  analysed_ = true;
  return true;
}

void CaptureService::analyse_async(const SplitSettings& settings) {
  if (analysing_.exchange(true)) return;
  if (analysis_thread_.joinable()) analysis_thread_.join();
  analysis_thread_ = std::thread([this, settings] {
    std::string e;
    analysing_ = false;  // analyse() raises it again; it is only the guard against a second launch
    if (!analyse(settings, e)) {
      std::lock_guard<std::mutex> lock(mu_);
      error_ = e;
    }
  });
}

SplitSettings CaptureService::settings() const {
  std::lock_guard<std::mutex> lock(mu_);
  return settings_;
}

bool CaptureService::rerun(const SplitSettings& settings, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!analysed_) {
    error = "analyse the side first";
    return false;
  }
  settings_ = settings;
  plan_.set_auto(find_splits(analysis_, settings));
  return true;
}

std::vector<CaptureService::SplitPointInfo> CaptureService::points() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<SplitPointInfo> out;
  for (const SplitPoint& p : plan_.points) {
    SplitPointInfo i;
    i.frame = p.frame;
    i.seconds = analysis_.sample_rate ? static_cast<double>(p.frame) / analysis_.sample_rate : 0;
    i.confidence = p.confidence;
    i.user = p.origin == SplitPoint::Origin::User;
    out.push_back(i);
  }
  return out;
}

bool CaptureService::add_point(std::uint64_t frame, std::size_t* index, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!analysed_) {
    error = "analyse the side first";
    return false;
  }
  if (frame == 0 || frame >= analysis_.frames) {
    error = "the point is outside the recording";
    return false;
  }
  const std::size_t at = plan_.add(frame);
  if (index) *index = at;
  return true;
}

bool CaptureService::move_point(std::size_t index, std::uint64_t frame, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!analysed_ || index >= plan_.points.size()) {
    error = "no such split point";
    return false;
  }
  if (frame == 0 || frame >= analysis_.frames) {
    error = "the point is outside the recording";
    return false;
  }
  return plan_.move(index, frame);
}

bool CaptureService::remove_point(std::size_t index, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!analysed_ || index >= plan_.points.size()) {
    error = "no such split point";
    return false;
  }
  return plan_.remove(index);
}

std::vector<CaptureService::PieceInfo> CaptureService::pieces(bool contiguous) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PieceInfo> out;
  if (!analysed_) return out;
  const std::vector<Piece> ps = plan_pieces(analysis_, settings_, plan_.points, contiguous);
  std::size_t n = 0;
  for (const Piece& p : ps) {
    PieceInfo i;
    i.start = p.start;
    i.end = p.end;
    i.start_s = static_cast<double>(p.start) / analysis_.sample_rate;
    i.end_s = static_cast<double>(p.end) / analysis_.sample_rate;
    i.empty = p.empty;
    if (!p.empty) {
      if (n < titles_.size()) i.title = titles_[n];
      ++n;
    }
    out.push_back(std::move(i));
  }
  return out;
}

std::vector<float> CaptureService::envelope(int buckets, double* seconds) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<float> out;
  if (seconds) *seconds = analysis_.sample_rate ? static_cast<double>(analysis_.frames) / analysis_.sample_rate : 0;
  if (!analysed_ || analysis_.env_db.empty() || buckets <= 0) return out;
  const std::size_t n = analysis_.env_db.size();
  const std::size_t b = static_cast<std::size_t>(std::min<std::size_t>(static_cast<std::size_t>(buckets), n));
  out.assign(b, -200.0f);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t k = std::min(b - 1, i * b / n);
    out[k] = std::max(out[k], analysis_.env_db[i]);
  }
  return out;
}

double CaptureService::side_seconds() const {
  std::lock_guard<std::mutex> lock(mu_);
  return analysis_.sample_rate ? static_cast<double>(analysis_.frames) / analysis_.sample_rate : 0;
}
std::uint32_t CaptureService::side_rate() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rate_;
}

void CaptureService::set_album(const AlbumTags& a) {
  std::lock_guard<std::mutex> lock(mu_);
  album_ = a;
}
CaptureService::AlbumTags CaptureService::album() const {
  std::lock_guard<std::mutex> lock(mu_);
  return album_;
}
void CaptureService::set_titles(const std::vector<std::string>& titles) {
  std::lock_guard<std::mutex> lock(mu_);
  titles_ = titles;
}
std::vector<std::string> CaptureService::titles() const {
  std::lock_guard<std::mutex> lock(mu_);
  return titles_;
}

std::vector<ExportResult> CaptureService::export_pieces(const std::string& dir, const ExportSettings& s, bool contiguous, std::string& error) {
  std::string path;
  std::vector<Piece> ps;
  std::vector<TrackTags> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!side_source_open(error)) return {};
    if (!analysed_) {
      error = "analyse the side first";
      return {};
    }
    path = path_;
    ps = plan_pieces(analysis_, settings_, plan_.points, contiguous);
    std::size_t n = 0;
    for (const Piece& p : ps) {
      if (p.empty) continue;
      TrackTags t;
      t.artist = album_.artist;
      t.album = album_.album;
      t.genre = album_.genre;
      t.year = album_.year;
      t.comment = album_.comment;
      if (n < titles_.size()) t.title = titles_[n];
      tags.push_back(std::move(t));
      ++n;
    }
  }
  WavSource src;
  if (!src.open(path, error)) return {};
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  return capture::export_pieces(src, ps, tags, dir, s);
}

ExportResult CaptureService::export_edit(const std::string& source_path, double start_s, double end_s, const std::string& out_path, const ExportSettings& s,
                                         const TrackTags* tags, std::string& error) {
  ExportResult bad;
  std::shared_ptr<TrackAudio> t = decode_file(source_path, error);
  if (!t) {
    bad.error = error;
    return bad;
  }
  MemorySource src(t->data(), t->frames, 2, t->sample_rate);
  ExportResult r = capture::export_edit(src, source_path, start_s, end_s, out_path, s, tags);
  if (!r.ok) error = r.error;
  return r;
}

}  // namespace deckhand
