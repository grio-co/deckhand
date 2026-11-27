#include "audio_store.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

#include "crypto.hpp"

namespace fs = std::filesystem;

namespace deckhand::sync::detail {
namespace {

constexpr char kMagic[4] = {'D', 'H', 'A', 'U'};
constexpr std::uint8_t kAudioVersion = 1, kAudioEncrypted = 1;
constexpr std::size_t kFixedHeader = 16;  // magic, version, flags, 2 reserved, u64 size

void put_u64(std::string& s, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
std::uint64_t get_u64(const unsigned char* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
std::uint32_t get_u32(const unsigned char* p) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

std::string make_header(std::uint64_t size, const std::string& salt) {
  std::string h(kMagic, 4);
  h.push_back(static_cast<char>(kAudioVersion));
  h.push_back(static_cast<char>(salt.empty() ? 0 : kAudioEncrypted));
  h.push_back(0);
  h.push_back(0);
  put_u64(h, size);
  if (!salt.empty()) h += salt;
  return h;
}

std::string chunk_aad(const std::string& header, std::uint32_t index, bool last) {
  std::string a = header;
  put_u32(a, index);
  a.push_back(last ? 1 : 0);
  return a;
}

}  // namespace

std::string hash_whole_file(const fs::path& path, std::uint64_t& size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  Hash128 h;
  std::vector<char> buf(256 * 1024);
  size = 0;
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (!n) break;
    h.add(buf.data(), n);
    size += n;
  }
  if (in.bad()) return {};
  return h.hex();
}

fs::path audio_object_path(const fs::path& store, const std::string& hash) {
  return store / hash.substr(0, 2) / (hash + ".dhau");
}

std::uint64_t audio_object_size(const fs::path& obj) {
  std::error_code ec;
  const auto n = fs::file_size(obj, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

PutResult audio_put(const fs::path& store, const fs::path& src, const std::string& hash, std::uint64_t size, const Sealer* sealer,
                    const std::string& tag, std::string& error) {
  const fs::path obj = audio_object_path(store, hash);
  std::error_code ec;
  fs::create_directories(obj.parent_path(), ec);
  if (ec) {
    error = "cannot create " + obj.parent_path().string() + ": " + ec.message();
    return PutResult::Failed;
  }
  const fs::path tmp = obj.parent_path() / ("." + hash + "." + tag + ".tmp");
  const bool enc = sealer && !sealer->key.empty();
  PutResult result = PutResult::Failed;
  {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
      error = "cannot open " + (in ? tmp : src).string();
      return PutResult::Failed;
    }
    const std::string header = make_header(size, enc ? sealer->salt : std::string());
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    Hash128 h;
    std::uint64_t done = 0;
    std::uint32_t index = 0;
    std::string chunk;
    bool ok = true;
    do {
      chunk.assign(static_cast<std::size_t>(std::min<std::uint64_t>(kAudioChunk, size - done)), '\0');
      in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (static_cast<std::size_t>(in.gcount()) != chunk.size()) {
        ok = false;  // shorter than when it was hashed
        break;
      }
      h.add(chunk.data(), chunk.size());
      done += chunk.size();
      const bool last = done >= size;
      if (enc) {
        std::string nonce, sealed;
        if (!random_bytes(nonce, kNonceLen) || !seal(sealer->key, nonce, chunk_aad(header, index, last), chunk, sealed)) {
          error = "encryption failed";
          ok = false;
          result = PutResult::Failed;
          break;
        }
        std::string rec;
        put_u32(rec, static_cast<std::uint32_t>(chunk.size()));
        rec += nonce;
        rec += sealed;
        out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
      } else {
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      }
      ++index;
    } while (done < size);
    if (ok) {
      char extra;
      in.read(&extra, 1);
      if (in.gcount() != 0 || h.hex() != hash) ok = false;  // grew or changed
      else result = PutResult::Ok;
    } else if (result != PutResult::Failed || error.empty()) {
      result = PutResult::SourceChanged;
    }
    out.flush();
    if (!out) {
      error = "cannot write " + tmp.string();
      result = PutResult::Failed;
    }
  }
  if (result == PutResult::Ok) {
    fs::rename(tmp, obj, ec);
    if (ec && audio_object_size(obj) > 0) {  // another device stored the same audio first (Windows will not replace)
      ec.clear();
    }
    if (ec) {
      error = "cannot move " + tmp.string() + " into place: " + ec.message();
      result = PutResult::Failed;
    }
  }
  if (result != PutResult::Ok) fs::remove(tmp, ec);
  return result;
}

GetResult audio_get(const fs::path& obj, const std::string& hash, std::uint64_t size, const KeyFor& key_for, bool require_encrypted,
                    const fs::path& dest, std::string& error) {
  std::error_code ec;
  if (!fs::is_regular_file(obj, ec)) return GetResult::Missing;
  std::ifstream in(obj, std::ios::binary);
  unsigned char fixed[kFixedHeader];
  if (!in.read(reinterpret_cast<char*>(fixed), kFixedHeader)) {
    error = "object is cut off";
    return GetResult::Corrupt;  // still being uploaded, or damaged: the caller retries later
  }
  if (std::string(reinterpret_cast<char*>(fixed), 4) != std::string(kMagic, 4) || fixed[4] > kAudioVersion) {
    error = "not an audio object of this version";
    return GetResult::Corrupt;
  }
  const bool enc = (fixed[5] & kAudioEncrypted) != 0;
  const std::uint64_t stored = get_u64(fixed + 8);
  if (stored != size) {
    error = "size differs from what the library says";
    return GetResult::Corrupt;
  }
  if (!enc && require_encrypted) return GetResult::PlainRefused;
  std::string header(reinterpret_cast<char*>(fixed), kFixedHeader);
  const std::string* key = nullptr;
  if (enc) {
    std::string salt(kSaltLen, '\0');
    if (!in.read(salt.data(), static_cast<std::streamsize>(kSaltLen))) {
      error = "object is cut off";
      return GetResult::Corrupt;
    }
    header += salt;
    key = key_for ? key_for(salt) : nullptr;
    if (!key) return GetResult::Locked;
  }
  fs::create_directories(dest.parent_path(), ec);
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + dest.string();
    return GetResult::Corrupt;
  }
  Hash128 h;
  std::uint64_t done = 0;
  std::uint32_t index = 0;
  bool bad = false;
  GetResult res = GetResult::Ok;
  do {
    std::string plain;
    if (enc) {
      unsigned char lenb[4];
      if (!in.read(reinterpret_cast<char*>(lenb), 4)) {
        bad = true;
        break;
      }
      const std::uint32_t len = get_u32(lenb);
      std::string nonce(kNonceLen, '\0');
      std::string sealed(static_cast<std::size_t>(len) + kTagLen, '\0');
      if (len > kAudioChunk || done + len > size || !in.read(nonce.data(), static_cast<std::streamsize>(kNonceLen)) ||
          !in.read(sealed.data(), static_cast<std::streamsize>(sealed.size()))) {
        bad = true;
        break;
      }
      const bool last = done + len >= size;
      if (!unseal(*key, nonce, chunk_aad(header, index, last), sealed, plain)) {
        error = "wrong passphrase or altered data";
        res = GetResult::Locked;
        bad = true;
        break;
      }
    } else {
      plain.assign(static_cast<std::size_t>(std::min<std::uint64_t>(kAudioChunk, size - done)), '\0');
      if (!in.read(plain.data(), static_cast<std::streamsize>(plain.size()))) {
        bad = true;
        break;
      }
    }
    h.add(plain.data(), plain.size());
    out.write(plain.data(), static_cast<std::streamsize>(plain.size()));
    done += plain.size();
    ++index;
  } while (done < size);
  out.flush();
  const bool wrote = static_cast<bool>(out);
  out.close();
  if (!bad && wrote && h.hex() == hash && done == size) return GetResult::Ok;
  if (res == GetResult::Ok) {
    res = GetResult::Corrupt;
    error = bad ? "object is cut off" : wrote ? "content does not match its hash" : "cannot write the copy";
  }
  fs::remove(dest, ec);
  return res;
}

}  // namespace deckhand::sync::detail
