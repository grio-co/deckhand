#include "crypto.hpp"

#ifdef DECKHAND_SYNC_CRYPTO
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#endif

namespace deckhand::sync::detail {

#ifdef DECKHAND_SYNC_CRYPTO
namespace {

const unsigned char* bytes(const std::string& s) { return reinterpret_cast<const unsigned char*>(s.data()); }
unsigned char* bytes(std::string& s) { return reinterpret_cast<unsigned char*>(&s[0]); }
using Ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

}  // namespace

bool crypto_available() { return true; }

bool random_bytes(std::string& out, std::size_t n) {
  out.assign(n, '\0');
  return n == 0 || RAND_bytes(bytes(out), static_cast<int>(n)) == 1;
}

bool derive_key(const std::string& passphrase, const std::string& salt, std::string& key) {
  key.assign(kKeyLen, '\0');
  return EVP_PBE_scrypt(passphrase.data(), passphrase.size(), bytes(salt), salt.size(), 1u << 15, 8, 1, 64u << 20,
                        bytes(key), key.size()) == 1;
}

bool seal(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& plain,
          std::string& sealed) {
  if (key.size() != kKeyLen || nonce.size() != kNonceLen) return false;
  Ctx c(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!c || EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, bytes(key), bytes(nonce)) != 1) return false;
  sealed.assign(plain.size() + kTagLen, '\0');
  int n = 0, tail = 0;
  if (EVP_EncryptUpdate(c.get(), nullptr, &n, bytes(aad), static_cast<int>(aad.size())) != 1) return false;
  n = 0;
  if (!plain.empty() &&
      EVP_EncryptUpdate(c.get(), bytes(sealed), &n, bytes(plain), static_cast<int>(plain.size())) != 1)
    return false;
  if (EVP_EncryptFinal_ex(c.get(), bytes(sealed) + n, &tail) != 1) return false;
  return EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), bytes(sealed) + plain.size()) == 1;
}

bool unseal(const std::string& key, const std::string& nonce, const std::string& aad, const std::string& sealed,
            std::string& plain) {
  if (key.size() != kKeyLen || nonce.size() != kNonceLen || sealed.size() < kTagLen) return false;
  Ctx c(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!c || EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, bytes(key), bytes(nonce)) != 1) return false;
  const std::size_t len = sealed.size() - kTagLen;
  std::string out(len + 1, '\0'), tag = sealed.substr(len);
  int n = 0, tail = 0;
  if (EVP_DecryptUpdate(c.get(), nullptr, &n, bytes(aad), static_cast<int>(aad.size())) != 1) return false;
  n = 0;
  if (len > 0 && EVP_DecryptUpdate(c.get(), bytes(out), &n, bytes(sealed), static_cast<int>(len)) != 1) return false;
  if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen), bytes(tag)) != 1) return false;
  if (EVP_DecryptFinal_ex(c.get(), bytes(out) + n, &tail) != 1) return false;  // wrong key or altered data
  out.resize(len);
  plain.swap(out);
  return true;
}

std::string key_mark(const std::string& key) {
  const std::string in = "DeckHand sync key mark" + key;
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  if (EVP_Digest(in.data(), in.size(), md, &n, EVP_sha256(), nullptr) != 1 || n < 8) return {};
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 8; ++i) {
    out.push_back(hex[md[i] >> 4]);
    out.push_back(hex[md[i] & 15]);
  }
  return out;
}

#else

bool crypto_available() { return false; }
bool random_bytes(std::string&, std::size_t) { return false; }
bool derive_key(const std::string&, const std::string&, std::string&) { return false; }
bool seal(const std::string&, const std::string&, const std::string&, const std::string&, std::string&) { return false; }
bool unseal(const std::string&, const std::string&, const std::string&, const std::string&, std::string&) { return false; }
std::string key_mark(const std::string&) { return {}; }

#endif

}  // namespace deckhand::sync::detail
