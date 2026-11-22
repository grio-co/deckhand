// Self-signed certificate generation and fingerprinting (OpenSSL).
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <cstdio>
#include <memory>

#include "deckhand/api/server.hpp"
#include "fileio.hpp"
#include "sockport.hpp"

namespace deckhand::api {
namespace {

template <class T, void (*F)(T*)>
struct Free {
  void operator()(T* p) const { if (p) F(p); }
};
using PKey = std::unique_ptr<EVP_PKEY, Free<EVP_PKEY, EVP_PKEY_free>>;
using Cert = std::unique_ptr<X509, Free<X509, X509_free>>;

bool valid_host(const std::string& h) {
  if (h.empty() || h.size() > 253) return false;
  for (char c : h)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '_' || c == '*')) return false;
  return true;
}

bool is_ip(const std::string& h) {
  unsigned char buf[16];
  return inet_pton(AF_INET, h.c_str(), buf) == 1 || inet_pton(AF_INET6, h.c_str(), buf) == 1;
}

bool write_file(const std::string& path, const std::string& data, bool owner_only) {
  return write_file_atomic(path, data, owner_only);
}

std::string bio_to_string(BIO* b) {
  char* p = nullptr;
  long n = BIO_get_mem_data(b, &p);
  return n > 0 ? std::string(p, static_cast<std::size_t>(n)) : std::string();
}
}  // namespace

bool generate_self_signed(const std::string& cert_path, const std::string& key_path,
                          const std::vector<std::string>& hostnames, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  try {
    std::string san = "DNS:localhost,IP:127.0.0.1,IP:::1";
    for (const auto& h : hostnames) {
      if (!valid_host(h)) return fail("invalid hostname");
      san += (is_ip(h) ? ",IP:" : ",DNS:") + h;
    }
    PKey key(EVP_EC_gen("P-256"));
    if (!key) return fail("key generation failed");
    Cert x(X509_new());
    if (!x) return fail("X509_new failed");
    unsigned char serial[16];
    if (RAND_bytes(serial, sizeof serial) != 1) return fail("random failed");
    serial[0] &= 0x7F;
    std::unique_ptr<BIGNUM, Free<BIGNUM, BN_free>> bn(BN_bin2bn(serial, sizeof serial, nullptr));
    X509_set_version(x.get(), 2);
    if (!bn || !BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(x.get()))) return fail("serial failed");
    X509_gmtime_adj(X509_getm_notBefore(x.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x.get()), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x.get(), key.get());
    std::unique_ptr<X509_NAME, Free<X509_NAME, X509_NAME_free>> nm(X509_NAME_new());
    X509_NAME* name = nm.get();
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DeckHand"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DeckHand API"), -1, -1, 0);
    X509_set_subject_name(x.get(), name);
    X509_set_issuer_name(x.get(), name);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x.get(), x.get(), nullptr, nullptr, 0);
    const struct {
      int nid;
      const char* value;
    } exts[] = {{NID_basic_constraints, "critical,CA:FALSE"},
                {NID_key_usage, "critical,digitalSignature"},
                {NID_ext_key_usage, "serverAuth"},
                {NID_subject_alt_name, san.c_str()}};
    for (const auto& e : exts) {
      X509_EXTENSION* ex = X509V3_EXT_nconf_nid(nullptr, &ctx, e.nid, e.value);
      if (!ex) return fail("certificate extension failed");
      X509_add_ext(x.get(), ex, -1);
      X509_EXTENSION_free(ex);
    }
    if (X509_sign(x.get(), key.get(), EVP_sha256()) <= 0) return fail("signing failed");

    std::unique_ptr<BIO, Free<BIO, BIO_vfree>> kb(BIO_new(BIO_s_mem())), cb(BIO_new(BIO_s_mem()));
    if (!kb || !cb || !PEM_write_bio_PrivateKey(kb.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ||
        !PEM_write_bio_X509(cb.get(), x.get()))
      return fail("PEM encoding failed");
    if (!write_file(key_path, bio_to_string(kb.get()), true)) return fail("cannot write key file");
    if (!write_file(cert_path, bio_to_string(cb.get()), false)) return fail("cannot write certificate file");
    return true;
  } catch (...) {
    return fail("internal error");
  }
}

bool certificate_fingerprint(const std::string& cert_path, std::string* fingerprint, std::string* error) {
  FILE* f = std::fopen(cert_path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open certificate";
    return false;
  }
  Cert x(PEM_read_X509(f, nullptr, nullptr, nullptr));
  std::fclose(f);
  if (!x) {
    if (error) *error = "not a PEM certificate";
    return false;
  }
  unsigned char der[8192];
  unsigned char* p = der;
  int n = i2d_X509(x.get(), nullptr);
  if (n <= 0 || n > static_cast<int>(sizeof der) || i2d_X509(x.get(), &p) != n) {
    if (error) *error = "cannot encode certificate";
    return false;
  }
  if (fingerprint) *fingerprint = sha256_hex(std::string_view(reinterpret_cast<char*>(der), static_cast<std::size_t>(n)));
  return true;
}

}  // namespace deckhand::api
