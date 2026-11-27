#pragma once
// Recording from an analogue input in the application: takes the engine's raw input tap, writes it with a CaptureSession,
// and after the stop holds the side's level envelope, the editable split points, the track names and the export. The
// panel and the REST routes are thin shells over this class. It opens no device unless start() is told to (Options), so
// tests drive it with an offline engine and a fake input (Engine::configure_offline_input / feed_input).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deckhand/audio/engine.hpp"
#include "deckhand/capture/export.hpp"
#include "deckhand/capture/session.hpp"
#include "deckhand/capture/split.hpp"

namespace deckhand {

class CaptureService {
 public:
  struct Options {
    bool open_device = true;  // start() opens the input the caller picked; false: use the input that is already open
  };
  enum class State { Idle, Recording, Paused, Stopped };

  struct StartArgs {
    std::string device;       // case-insensitive part of a device name from inputs(); "" = the system default
    int channel = 0;
    bool stereo = true;
    std::string format = "pcm24";  // pcm16 | pcm24 | float32
    std::string name;         // file name without extension; "" = capture-<date>-<time>
  };

  struct Status {
    State state = State::Idle;
    std::string path, input, error;
    double seconds = 0;
    std::uint64_t frames = 0;
    std::uint32_t sample_rate = 0;
    float peak_db = -200, hold_db = -200, rms_db = -200;  // the meter: latest block, decaying peak hold, latest block RMS
    std::uint64_t clips = 0, dropped = 0;
    bool analysed = false, analysing = false;
    int points = 0;
  };

  struct SplitPointInfo {
    std::uint64_t frame = 0;
    double seconds = 0, confidence = 1;
    bool user = false;
  };
  struct PieceInfo {
    std::uint64_t start = 0, end = 0;
    double start_s = 0, end_s = 0;
    bool empty = false;
    std::string title;
  };
  struct AlbumTags {
    std::string artist, album, genre, year, comment;
  };

  CaptureService(IEngine& engine, std::string folder);
  CaptureService(IEngine& engine, std::string folder, Options options);
  ~CaptureService();
  CaptureService(const CaptureService&) = delete;
  CaptureService& operator=(const CaptureService&) = delete;

  std::vector<std::string> inputs();  // capture devices present
  const std::string& folder() const { return folder_; }

  // ---- recording ----
  bool start(const StartArgs& args, std::string& error);
  bool set_paused(bool paused, std::string& error);
  bool stop(std::string& error);  // drains and closes the file; the side can then be analysed and exported
  Status status();
  // Opens an existing recording (a WAV) as the current side, for splitting and export without recording.
  bool load(const std::string& path, std::string& error);

  // ---- splitting (after stop or load) ----
  bool analyse(const capture::SplitSettings& settings, std::string& error);  // reads the whole file: blocks
  void analyse_async(const capture::SplitSettings& settings);                 // the same on a thread (the panel)
  capture::SplitSettings settings() const;
  bool rerun(const capture::SplitSettings& settings, std::string& error);     // new settings, keeps the user's edits
  std::vector<SplitPointInfo> points() const;
  bool add_point(std::uint64_t frame, std::size_t* index, std::string& error);
  bool move_point(std::size_t index, std::uint64_t frame, std::string& error);
  bool remove_point(std::size_t index, std::string& error);
  std::vector<PieceInfo> pieces(bool contiguous) const;
  // The side's level envelope reduced to `buckets` values (dB, the loudest hop of each bucket) and its length.
  std::vector<float> envelope(int buckets, double* seconds = nullptr) const;
  double side_seconds() const;
  std::uint32_t side_rate() const;

  // ---- names and export ----
  void set_album(const AlbumTags& a);
  AlbumTags album() const;
  void set_titles(const std::vector<std::string>& titles);  // one per non-empty piece, in order
  std::vector<std::string> titles() const;
  std::vector<capture::ExportResult> export_pieces(const std::string& dir, const capture::ExportSettings& s, bool contiguous, std::string& error);

  // An edited copy of a decoded track (trim, fades, normalise). The original is never written to.
  static capture::ExportResult export_edit(const std::string& source_path, double start_s, double end_s, const std::string& out_path,
                                           const capture::ExportSettings& s, const capture::TrackTags* tags, std::string& error);

 private:
  void pump();
  void stop_pump();
  bool side_source_open(std::string& error);

  IEngine& engine_;
  std::string folder_;
  Options options_;
  mutable std::mutex mu_;
  std::unique_ptr<capture::CaptureSession> session_;
  State state_ = State::Idle;
  std::string path_, input_name_, error_;
  std::uint32_t rate_ = 0;
  bool we_opened_input_ = false;
  std::thread pump_;
  std::atomic<bool> pump_run_{false};
  float peak_db_ = -200, hold_db_ = -200, rms_db_ = -200;
  std::uint64_t tap_dropped_ = 0;
  // the side
  capture::SplitAnalysis analysis_;
  capture::SplitSettings settings_;
  capture::SplitPlan plan_;
  bool analysed_ = false;
  std::atomic<bool> analysing_{false};
  std::thread analysis_thread_;
  AlbumTags album_;
  std::vector<std::string> titles_;
};

}  // namespace deckhand
