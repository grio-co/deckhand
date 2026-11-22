#pragma once
// Exposes the application over the REST + WebSocket API (see docs/API.md).
#include <cstdint>
#include <memory>
#include <string>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

// What the bridge needs from the application. Methods are called from API threads.
class ApiHost {
 public:
  virtual ~ApiHost() = default;
  virtual IEngine& api_engine() = 0;
  virtual bool api_load(int deck, std::int64_t track_id, std::string& error) = 0;
  virtual void api_add_file(const std::string& path, int deck) = 0;  // deck -1 = just add
  virtual bool api_export(const std::string& destination, std::int64_t playlist_id, std::string& error) = 0;
  virtual void api_library_changed() = 0;
  // start = true begins recording the master output; path receives the file in both cases.
  virtual bool api_record(bool start, std::string& path, std::string& error) = 0;
  virtual void api_load_sample(int slot, const std::string& path) = 0;
  virtual std::string api_status() = 0;
  virtual std::string api_audio_device() = 0;
};

class ApiBridge {
 public:
  // Settings are read from the library ("api.enabled", "api.port", "api.bind", "api.tls").
  ApiBridge(ApiHost& host, const std::string& data_dir);
  ~ApiBridge();
  bool running() const;
  std::string describe() const;
  // Forward engine events here (scheduled commands that fired or were dropped). Any thread.
  void engine_event(const EngineEvent& e);  // e.g. "API http://127.0.0.1:7373"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
