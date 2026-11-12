#pragma once
// RFC 6455 framing (client side with masking; the parser also reads masked frames so a test server can use it).
#include <cstdint>
#include <string>
#include <string_view>

namespace deckhand::broadcast {

enum class WsOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct WsMessage {
  WsOpcode opcode = WsOpcode::Text;
  std::string payload;  // fully reassembled for data messages
};

// One final frame. mask=true is what a client must do (random masking key per frame).
std::string ws_encode(WsOpcode op, std::string_view payload, bool mask);
// Base64 of 16 random bytes.
std::string ws_random_key();
// The client's HTTP upgrade request.
std::string ws_client_request(const std::string& host_header, const std::string& path, const std::string& key,
                              const std::string& subprotocol);

class WsParser {
 public:
  explicit WsParser(std::size_t max_message = 8u << 20) : max_(max_message) {}
  void feed(const char* data, std::size_t n) { buf_.append(data, n); }
  // Next complete message (control frames are returned as they arrive, fragments are reassembled).
  // False when more bytes are needed or after a protocol error (check failed()).
  bool next(WsMessage* out);
  bool failed() const { return failed_; }

 private:
  std::string buf_;
  std::string frag_;
  WsOpcode frag_op_ = WsOpcode::Text;
  bool in_frag_ = false;
  bool failed_ = false;
  std::size_t max_;
};

}  // namespace deckhand::broadcast
