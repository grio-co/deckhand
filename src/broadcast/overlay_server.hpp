#pragma once
// Private: tiny loopback HTTP server for the overlay page (GET only, strict limits, SSE for live updates).
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "sock.hpp"

namespace deckhand::broadcast {

class OverlayServer {
 public:
  OverlayServer();
  ~OverlayServer();
  OverlayServer(const OverlayServer&) = delete;
  OverlayServer& operator=(const OverlayServer&) = delete;

  bool start(std::uint16_t port);  // 127.0.0.1; 0 = ephemeral
  void stop();
  std::uint16_t port() const { return port_; }
  // New now-playing JSON (single line) and artwork file; pushed to connected event streams.
  void publish(const std::string& json, const std::string& artwork_path);

 private:
  struct Client;
  void run();

  net::Wake wake_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::string json_ = "{\"on_air\":false}";
  std::string artwork_;
  std::uint64_t version_ = 1;
};

const char* overlay_html();

}  // namespace deckhand::broadcast
