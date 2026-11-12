#pragma once
// OBS WebSocket protocol v5 client (plain ws://). All network I/O happens on the client's own thread.
// Public methods are thread-safe and never block for network I/O. Every callback (responses, events, connection
// changes) is delivered on the client thread, never under an internal lock, so a callback may call request()
// and the typed helpers again. A callback must not block for long, and must not destroy the client.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "deckhand/broadcast/json.hpp"

namespace deckhand::broadcast::net {
class Wake;
}

namespace deckhand::broadcast {

struct WsMessage;

struct ObsConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 4455;
  std::string password;                // empty when OBS has authentication disabled
  double request_timeout = 5.0;        // seconds until a request is answered with error "timeout"
  double reconnect_min = 1.0;          // backoff after a failed or dropped connection, doubling up to reconnect_max
  double reconnect_max = 15.0;
  std::uint32_t event_subscriptions = 2047;  // OBS EventSubscription bitmask (2047 = all but high-volume events)
};

enum class ObsState { Stopped, Connecting, Connected, Disconnected };

struct ObsResponse {
  bool ok = false;     // OBS reported success
  int code = 0;        // OBS request status code (100 = success); 0 when OBS never answered
  std::string error;   // local failure ("not connected", "timeout", "disconnected", "stopped", "bad request json",
                       // "not started") or OBS's status comment
  Json data;           // responseData (null when absent)
};

using ObsCallback = std::function<void(const ObsResponse&)>;
using ObsEventCallback = std::function<void(const std::string& event_type, const Json& event_data)>;
using ObsConnectionCallback = std::function<void(bool connected)>;
using ObsScenesCallback =
    std::function<void(const ObsResponse&, const std::vector<std::string>& scenes, const std::string& current)>;
using ObsStringCallback = std::function<void(const ObsResponse&, const std::string& value)>;
using ObsBoolCallback = std::function<void(const ObsResponse&, bool value)>;

class ObsClient {
 public:
  explicit ObsClient(ObsConfig cfg = {});
  ~ObsClient();
  ObsClient(const ObsClient&) = delete;
  ObsClient& operator=(const ObsClient&) = delete;

  // Replaces the settings; a running client reconnects with them.
  void configure(const ObsConfig& cfg);
  void start();  // connects (and keeps reconnecting with backoff) in the background
  void stop();   // disconnects; pending requests complete with error "stopped"
  bool connected() const;
  ObsState state() const;
  std::string last_error() const;

  void on_connection(ObsConnectionCallback cb);  // after identify succeeded / after the link dropped
  void on_event(ObsEventCallback cb);            // all OBS events within event_subscriptions

  // Generic request. json_text is the requestData object ("" for none). The callback always fires exactly once.
  void request(const std::string& type, const std::string& json_text, ObsCallback cb = nullptr);

  // Typed helpers.
  void get_version(ObsCallback cb);
  void list_scenes(ObsScenesCallback cb);                    // names as OBS returns them, plus the program scene
  void get_current_scene(ObsStringCallback cb);              // program scene name
  void set_current_scene(const std::string& scene, ObsCallback cb = nullptr);
  void set_text(const std::string& input_name, const std::string& text, ObsCallback cb = nullptr);  // text source
  void get_scene_item_id(const std::string& scene, const std::string& source,
                         std::function<void(const ObsResponse&, long long id)> cb);
  void set_scene_item_enabled(const std::string& scene, long long item_id, bool enabled, ObsCallback cb = nullptr);
  // Looks the item up by source name, then shows/hides it.
  void set_source_visible(const std::string& scene, const std::string& source, bool visible, ObsCallback cb = nullptr);
  void start_stream(ObsCallback cb = nullptr);
  void stop_stream(ObsCallback cb = nullptr);
  void toggle_stream(ObsCallback cb = nullptr);
  void stream_status(ObsBoolCallback cb);  // value = outputActive; full status in ObsResponse::data
  void start_record(ObsCallback cb = nullptr);
  void stop_record(ObsCallback cb = nullptr);  // data.outputPath holds the file
  void toggle_record(ObsCallback cb = nullptr);
  void record_status(ObsBoolCallback cb);

 private:
  struct Queued {
    std::string type;
    Json data;
    bool bad = false;
    ObsCallback cb;
    double deadline = 0;
  };
  struct Pending {
    ObsCallback cb;
    double deadline = 0;
    std::string type;
  };
  struct Conn;

  void run();
  bool open_session(Conn& c, std::string* err);
  void serve(Conn& c);
  bool handle_message(Conn& c, const WsMessage& m);
  void finish(const ObsCallback& cb, ObsResponse r);
  bool flush_queue(Conn* c, const char* fail_reason);
  void fail_pending(const char* reason);
  void set_state(ObsState s);

  mutable std::mutex m_;
  ObsConfig cfg_;
  ObsState state_ = ObsState::Stopped;
  std::string last_error_;
  ObsConnectionCallback on_conn_;
  ObsEventCallback on_event_;
  std::deque<Queued> queue_;
  std::map<std::string, Pending> pending_;  // client thread only
  std::uint64_t next_id_ = 1;
  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<bool> stop_{false};
  bool accepting_ = false;
  std::unique_ptr<net::Wake> wake_;   // new request queued
  std::unique_ptr<net::Wake> abort_;  // stop requested
};

}  // namespace deckhand::broadcast
