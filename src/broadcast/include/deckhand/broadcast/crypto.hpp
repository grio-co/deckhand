#pragma once
// Small original SHA-256 / SHA-1 / base64 implementations (OBS authentication and the WebSocket handshake).
#include <string>
#include <string_view>

namespace deckhand::broadcast {

std::string sha256(std::string_view data);  // 32 raw bytes
std::string sha1(std::string_view data);    // 20 raw bytes
std::string to_hex(std::string_view bytes);
std::string base64_encode(std::string_view bytes);
bool base64_decode(std::string_view text, std::string* out);

// OBS WebSocket v5: base64(sha256(base64(sha256(password + salt)) + challenge)).
std::string obs_auth_string(const std::string& password, const std::string& salt, const std::string& challenge);
// RFC 6455 Sec-WebSocket-Accept for a client key.
std::string websocket_accept_key(const std::string& client_key);

}  // namespace deckhand::broadcast
