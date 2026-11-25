#pragma once
// Internal: the primitives behind sync folder encryption (see docs/SYNC.md, format appendix). All of them come from
// OpenSSL's libcrypto; a build without it (DECKHAND_SYNC_CRYPTO unset) reports encryption as unavailable.
#include <cstddef>
#include <string>

namespace deckhand::sync::detail {

constexpr std::size_t kSaltLen = 16, kNonceLen = 12, kTagLen = 16, kKeyLen = 32;

bool crypto_available();
bool random_bytes(std::string& out, std::size_t n);
// scrypt, N = 2^15, r = 8, p = 1 (32 MiB). The parameters are part of the format.
bool derive_key(const std::string& passphrase, const std::string& salt, std::string& key);
// AES-256-GCM. `sealed` is the ciphertext followed by the 16 byte tag; `aad` is authenticated but not encrypted.
bool seal(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& plain,
          std::string& sealed);
bool unseal(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed,
            std::string& plain);
// Short digest used to notice locally that the passphrase changed; "" if unavailable.
std::string key_mark(const std::string& key);

}  // namespace deckhand::sync::detail
