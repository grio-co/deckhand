#include "deckhand/stems/model_convert.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "deckhand/stems/checkpoint.hpp"
#include "internal.hpp"

namespace deckhand::stems {
namespace fs = std::filesystem;
using detail::Json;

bool convert_checkpoint(const fs::path& ckpt, const fs::path& layout_json, const fs::path& out, const std::function<void(float)>& progress, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  std::ifstream lf(layout_json, std::ios::binary);
  if (!lf) return fail("cannot open layout file");
  std::ostringstream ss;
  ss << lf.rdbuf();
  Json j;
  std::string e;
  if (!detail::parse_json(ss.str(), j, &e)) return fail("layout file: " + e);
  const Json* fmt = j.get("format");
  const Json* tensors = j.get("tensors");
  const Json* total = j.get("total_bytes");
  if (!fmt || fmt->s != "deckhand-stems-layout/1" || !tensors || tensors->t != Json::T::Arr || !total || !total->is_num() || total->n < 0 || total->n > 8e9)
    return fail("not a deckhand-stems-layout/1 file");
  Checkpoint ck;
  if (!read_checkpoint(ckpt, ck, &e)) return fail(e);
  std::map<std::string, const CkptTensor*> by_name;
  for (auto& t : ck.tensors) by_name[t.name] = &t;
  if (by_name.size() != tensors->a.size()) return fail("checkpoint has " + std::to_string(by_name.size()) + " tensors, the model expects " + std::to_string(tensors->a.size()) + " (other version?)");
  fs::path tmp = out;
  tmp += ".part";
  std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) return fail("cannot create " + tmp.string());
  bool ok = true;
  std::uint64_t written = 0;
  const std::uint64_t want = std::uint64_t(total->n);
  std::vector<float> data;
  std::size_t n = 0;
  for (auto& t : tensors->a) {
    const Json* nm = t.get("name");
    const Json* off = t.get("offset");
    const Json* len = t.get("length");
    const Json* shp = t.get("shape");
    if (!nm || !nm->is_str() || !off || !off->is_num() || !len || !len->is_num() || !shp || shp->t != Json::T::Arr) { ok = false; e = "bad layout entry"; break; }
    auto it = by_name.find(nm->s);
    if (it == by_name.end()) { ok = false; e = "tensor missing from checkpoint: " + nm->s; break; }
    const CkptTensor& ct = *it->second;
    bool same = ct.shape.size() == shp->a.size();
    for (std::size_t k = 0; same && k < ct.shape.size(); ++k) same = shp->a[k].is_num() && double(ct.shape[k]) == shp->a[k].n;
    std::uint64_t o = std::uint64_t(off->n), l = std::uint64_t(len->n);
    if (!same || l != ct.numel * 4 || o + l > want || o < written) { ok = false; e = "tensor shape or placement mismatch: " + nm->s; break; }
    if (!read_tensor_f32(ckpt, ct, data, &e)) { ok = false; break; }
    // zero-fill alignment gaps
    static const char zeros[64] = {0};
    for (std::uint64_t gap = o - written; gap > 0;) {
      std::size_t g = std::size_t(std::min<std::uint64_t>(gap, sizeof zeros));
      if (std::fwrite(zeros, 1, g, f) != g) { ok = false; break; }
      gap -= g; written += g;
    }
    if (!ok) { e = "write failed"; break; }
    std::vector<std::uint8_t> bytes(data.size() * 4);
    for (std::size_t i = 0; i < data.size(); ++i) { std::uint32_t u; std::memcpy(&u, &data[i], 4); for (int b = 0; b < 4; ++b) bytes[4 * i + std::size_t(b)] = std::uint8_t(u >> (8 * b)); }
    if (std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) { ok = false; e = "write failed"; break; }
    written += bytes.size();
    if (progress) progress(float(++n) / float(tensors->a.size()));
  }
  if (ok) {
    static const char zeros[64] = {0};
    while (written < want) {
      std::size_t g = std::size_t(std::min<std::uint64_t>(want - written, sizeof zeros));
      if (std::fwrite(zeros, 1, g, f) != g) { ok = false; e = "write failed"; break; }
      written += g;
    }
  }
  ok = (std::fclose(f) == 0) && ok;
  std::error_code ec;
  if (!ok) { fs::remove(tmp, ec); return fail(e.empty() ? "write failed" : e); }
  fs::rename(tmp, out, ec);
  if (ec) { fs::remove(tmp, ec); return fail("rename failed"); }
  return true;
}

}  // namespace deckhand::stems
