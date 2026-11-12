#include "deckhand/broadcast/websocket.hpp"

#include <random>

#include "deckhand/broadcast/crypto.hpp"

namespace deckhand::broadcast {
namespace {

std::mt19937_64& rng() {
  thread_local std::mt19937_64 g{std::random_device{}()};
  return g;
}

}  // namespace

std::string ws_random_key() {
  std::string raw(16, '\0');
  for (auto& c : raw) c = static_cast<char>(rng()() & 0xff);
  return base64_encode(raw);
}

std::string ws_encode(WsOpcode op, std::string_view payload, bool mask) {
  std::string out;
  out.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(op)));
  const std::uint8_t mbit = mask ? 0x80 : 0;
  const std::size_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<char>(mbit | n));
  } else if (n <= 0xffff) {
    out.push_back(static_cast<char>(mbit | 126));
    out.push_back(static_cast<char>((n >> 8) & 0xff));
    out.push_back(static_cast<char>(n & 0xff));
  } else {
    out.push_back(static_cast<char>(mbit | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xff));
  }
  if (!mask) {
    out.append(payload);
    return out;
  }
  char key[4];
  for (auto& c : key) c = static_cast<char>(rng()() & 0xff);
  out.append(key, 4);
  for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<char>(payload[i] ^ key[i & 3]));
  return out;
}

std::string ws_client_request(const std::string& host_header, const std::string& path, const std::string& key,
                              const std::string& subprotocol) {
  std::string r = "GET " + path + " HTTP/1.1\r\nHost: " + host_header +
                  "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                  "\r\nSec-WebSocket-Version: 13\r\n";
  if (!subprotocol.empty()) r += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
  r += "\r\n";
  return r;
}

bool WsParser::next(WsMessage* out) {
  while (!failed_) {
    if (buf_.size() < 2) return false;
    const auto b0 = static_cast<std::uint8_t>(buf_[0]);
    const auto b1 = static_cast<std::uint8_t>(buf_[1]);
    const bool fin = (b0 & 0x80) != 0;
    const auto op = static_cast<WsOpcode>(b0 & 0x0f);
    const bool masked = (b1 & 0x80) != 0;
    std::uint64_t len = b1 & 0x7f;
    std::size_t pos = 2;
    if ((b0 & 0x70) != 0) { failed_ = true; return false; }  // no extensions negotiated
    if (len == 126) {
      if (buf_.size() < 4) return false;
      len = (std::uint64_t(static_cast<std::uint8_t>(buf_[2])) << 8) | static_cast<std::uint8_t>(buf_[3]);
      pos = 4;
    } else if (len == 127) {
      if (buf_.size() < 10) return false;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<std::uint8_t>(buf_[2 + i]);
      pos = 10;
    }
    const bool control = (b0 & 0x08) != 0;
    if (len > max_ || (control && (len > 125 || !fin))) { failed_ = true; return false; }
    const std::size_t need = pos + (masked ? 4 : 0) + static_cast<std::size_t>(len);
    if (buf_.size() < need) return false;
    std::string payload = buf_.substr(pos + (masked ? 4 : 0), static_cast<std::size_t>(len));
    if (masked) {
      const char* key = buf_.data() + pos;
      for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ key[i & 3]);
    }
    buf_.erase(0, need);

    if (control) {
      if (op != WsOpcode::Close && op != WsOpcode::Ping && op != WsOpcode::Pong) { failed_ = true; return false; }
      out->opcode = op;
      out->payload = std::move(payload);
      return true;
    }
    if (op == WsOpcode::Continuation) {
      if (!in_frag_ || frag_.size() + payload.size() > max_) { failed_ = true; return false; }
      frag_ += payload;
    } else if (op == WsOpcode::Text || op == WsOpcode::Binary) {
      if (in_frag_) { failed_ = true; return false; }
      frag_ = std::move(payload);
      frag_op_ = op;
      in_frag_ = true;
    } else {
      failed_ = true;
      return false;
    }
    if (fin) {
      out->opcode = frag_op_;
      out->payload = std::move(frag_);
      frag_.clear();
      in_frag_ = false;
      return true;
    }
  }
  return false;
}

}  // namespace deckhand::broadcast
