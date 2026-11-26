#include "deckhand/stems/checkpoint.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace deckhand::stems {
namespace fs = std::filesystem;
namespace {

bool fail(std::string* e, const std::string& m) { if (e) *e = m; return false; }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint64_t le64(const std::uint8_t* p) { return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32); }

// ---- zip (stored members only, as torch writes them) -------------------------------------------------------------
struct ZipEntry { std::uint64_t offset = 0, size = 0; };
struct Zip {
  std::map<std::string, ZipEntry> files;
  std::uint64_t total = 0;
};

bool read_at(std::FILE* f, std::uint64_t off, void* buf, std::size_t n) {
#if defined(_WIN32)
  if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) return false;
#else
  if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) return false;
#endif
  return std::fread(buf, 1, n, f) == n;
}

bool open_zip(std::FILE* f, Zip& z, const CkptLimits& lim, std::string* err) {
#if defined(_WIN32)
  _fseeki64(f, 0, SEEK_END);
  std::uint64_t size = static_cast<std::uint64_t>(_ftelli64(f));
#else
  fseeko(f, 0, SEEK_END);
  std::uint64_t size = static_cast<std::uint64_t>(ftello(f));
#endif
  z.total = size;
  if (size > lim.max_file_bytes) return fail(err, "checkpoint file too large");
  if (size < 22) return fail(err, "not a zip archive");
  std::vector<std::uint8_t> tail(std::size_t(std::min<std::uint64_t>(size, 65557)));
  if (!read_at(f, size - tail.size(), tail.data(), tail.size())) return fail(err, "read error");
  std::size_t e = tail.size() >= 22 ? tail.size() - 22 + 1 : 0;
  bool found = false;
  while (e-- > 0) if (le32(&tail[e]) == 0x06054b50) { found = true; break; }
  if (!found) return fail(err, "not a zip archive (no end-of-central-directory)");
  std::uint64_t entries = le16(&tail[e + 10]), cd_size = le32(&tail[e + 12]), cd_off = le32(&tail[e + 16]);
  if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
    // zip64 end record locator sits just before the EOCD
    if (e < 20 || le32(&tail[e - 20]) != 0x07064b50) return fail(err, "unsupported zip64 layout");
    std::uint64_t off64 = le64(&tail[e - 20 + 8]);
    std::uint8_t r[56];
    if (!read_at(f, off64, r, 56) || le32(r) != 0x06064b50) return fail(err, "bad zip64 record");
    entries = le64(r + 32); cd_size = le64(r + 40); cd_off = le64(r + 48);
  }
  if (cd_off + cd_size > size || entries > 1000000 || cd_size > (256u << 20)) return fail(err, "bad zip directory");
  std::vector<std::uint8_t> cd;
  cd.resize(static_cast<std::size_t>(cd_size));
  if (!read_at(f, cd_off, cd.data(), cd.size())) return fail(err, "read error");
  std::size_t p = 0;
  for (std::uint64_t i = 0; i < entries; ++i) {
    if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) return fail(err, "bad zip directory entry");
    std::uint16_t method = le16(&cd[p + 10]);
    std::uint64_t csize = le32(&cd[p + 20]), usize = le32(&cd[p + 24]), lho = le32(&cd[p + 42]);
    std::size_t nl = le16(&cd[p + 28]), xl = le16(&cd[p + 30]), cl = le16(&cd[p + 32]);
    if (p + 46 + nl + xl + cl > cd.size()) return fail(err, "bad zip directory entry");
    std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    // zip64 extra field
    for (std::size_t x = p + 46 + nl, xe = x + xl; x + 4 <= xe;) {
      std::uint16_t id = le16(&cd[x]), len = le16(&cd[x + 2]);
      if (id == 1) {
        std::size_t q = x + 4;
        if (usize == 0xFFFFFFFFu && q + 8 <= xe) { usize = le64(&cd[q]); q += 8; }
        if (csize == 0xFFFFFFFFu && q + 8 <= xe) { csize = le64(&cd[q]); q += 8; }
        if (lho == 0xFFFFFFFFu && q + 8 <= xe) { lho = le64(&cd[q]); q += 8; }
      }
      x += 4 + len;
    }
    if (method != 0 || csize != usize) return fail(err, "compressed zip member (not a PyTorch checkpoint?)");
    if (name.find("..") != std::string::npos || (!name.empty() && name[0] == '/')) return fail(err, "suspicious zip member name");
    std::uint8_t lh[30];
    if (lho + 30 > size || !read_at(f, lho, lh, 30) || le32(lh) != 0x04034b50) return fail(err, "bad local header");
    std::uint64_t data = lho + 30 + le16(lh + 26) + le16(lh + 28);
    if (data + usize > size) return fail(err, "zip member out of range");
    z.files[name] = ZipEntry{data, usize};
    p += 46 + nl + xl + cl;
  }
  return true;
}

// ---- restricted pickle interpreter --------------------------------------------------------------------------------
struct Val;
using VP = std::shared_ptr<Val>;
struct Val {
  enum K { None, Bool, Int, Float, Str, Bytes, Tuple, List, Dict, Global, Storage, Tensor, OrderedDict, Opaque } k = None;
  std::int64_t i = 0;
  double d = 0;
  std::string s;              // Str / Bytes / Global "module name" / Storage key
  std::vector<VP> v;          // Tuple / List items
  std::vector<std::pair<VP, VP>> kv;  // Dict / OrderedDict
  CkptTensor t;               // Tensor
  std::string dtype;          // Storage dtype
  std::uint64_t numel = 0;    // Storage elements
};
VP mk(Val::K k) { auto p = std::make_shared<Val>(); p->k = k; return p; }

const char* storage_dtype(const std::string& cls) {
  static const std::pair<const char*, const char*> m[] = {
      {"FloatStorage", "float32"}, {"HalfStorage", "float16"}, {"BFloat16Storage", "bfloat16"}, {"DoubleStorage", "float64"},
      {"LongStorage", "int64"}, {"IntStorage", "int32"}, {"ShortStorage", "int16"}, {"CharStorage", "int8"},
      {"ByteStorage", "uint8"}, {"BoolStorage", "bool"}};
  for (auto& e : m) if (cls == e.first) return e.second;
  return nullptr;
}

struct Interp {
  const std::vector<std::uint8_t>& d;
  const CkptLimits& lim;
  std::string err;
  std::vector<VP> st;
  std::vector<std::size_t> marks;
  std::map<std::uint64_t, VP> memo;
  explicit Interp(const std::vector<std::uint8_t>& b, const CkptLimits& l) : d(b), lim(l) {}

  bool bad(const std::string& m) { if (err.empty()) err = m; return false; }
  bool need(std::size_t n) { return st.size() >= n ? true : bad("pickle stack underflow"); }
  VP pop() { VP v = st.back(); st.pop_back(); return v; }
  bool push(VP v) { if (st.size() > 2000000) return bad("pickle stack too deep"); st.push_back(std::move(v)); return true; }

  // reads items above the last mark
  bool pop_mark(std::vector<VP>& items) {
    if (marks.empty()) return bad("pickle MARK missing");
    std::size_t m = marks.back();
    marks.pop_back();
    if (m > st.size()) return bad("pickle mark out of range");
    items.assign(st.begin() + long(m), st.end());
    st.resize(m);
    return true;
  }
  static bool is_int(const VP& v) { return v->k == Val::Int || v->k == Val::Bool; }

  bool call(const VP& fn, const VP& args, VP& out) {
    if (fn->k != Val::Global || args->k != Val::Tuple) return bad("call of a non-allow-listed object");
    const std::string& g = fn->s;
    if (g == "collections OrderedDict") {
      if (!args->v.empty()) return bad("unexpected OrderedDict arguments");
      out = mk(Val::OrderedDict);
      return true;
    }
    if (g == "torch._utils _rebuild_tensor_v2") {
      if (args->v.size() < 5) return bad("bad tensor rebuild arguments");
      const VP& stor = args->v[0];
      if (stor->k != Val::Storage || !is_int(args->v[1]) || args->v[2]->k != Val::Tuple || args->v[3]->k != Val::Tuple) return bad("bad tensor rebuild arguments");
      auto t = mk(Val::Tensor);
      t->t.dtype = stor->dtype;
      t->t.member = stor->s;
      t->t.offset = args->v[1]->i;
      std::uint64_t n = 1;
      for (auto& x : args->v[2]->v) {
        if (!is_int(x) || x->i < 0 || x->i > (1ll << 40)) return bad("bad tensor shape");
        t->t.shape.push_back(x->i);
        if (x->i && n > lim.max_tensor_elements / std::uint64_t(x->i)) return bad("tensor too large");
        n *= std::uint64_t(x->i);
      }
      for (auto& x : args->v[3]->v) {
        if (!is_int(x) || x->i < 0 || x->i > (1ll << 40)) return bad("bad tensor stride");
        t->t.stride.push_back(x->i);
      }
      if (t->t.stride.size() != t->t.shape.size() || t->t.offset < 0) return bad("bad tensor geometry");
      t->t.numel = n;
      // the viewed range must fit inside the storage
      std::uint64_t last = std::uint64_t(t->t.offset);
      for (std::size_t k = 0; k < t->t.shape.size(); ++k) if (t->t.shape[k] > 0) last += std::uint64_t(t->t.shape[k] - 1) * std::uint64_t(t->t.stride[k]);
      if (n > 0 && last >= stor->numel) return bad("tensor view exceeds its storage");
      out = t;
      return true;
    }
    // harmless value constructors that only need to be skipped over
    if (g == "fractions Fraction" || g == "numpy.core.multiarray scalar" || g == "numpy._core.multiarray scalar" || g == "numpy dtype") {
      out = mk(Val::Opaque);
      return true;
    }
    if (g == "_codecs encode") {
      if (args->v.size() != 2 || args->v[0]->k != Val::Str || args->v[1]->k != Val::Str || args->v[0]->s.size() > (1u << 20)) return bad("bad _codecs.encode");
      auto b = mk(Val::Bytes);
      // latin1 text -> bytes
      for (std::size_t i = 0; i < args->v[0]->s.size();) {
        unsigned char c = static_cast<unsigned char>(args->v[0]->s[i]);
        if (c < 0x80) { b->s += char(c); i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < args->v[0]->s.size()) { b->s += char(((c & 3) << 6) | (args->v[0]->s[i + 1] & 0x3F)); i += 2; }
        else return bad("bad latin1 text");
      }
      out = b;
      return true;
    }
    return bad("pickle calls a global that is not on the allow-list: " + g);
  }

  bool run() {
    std::size_t p = 0;
    const std::size_t n = d.size();
    auto rd = [&](std::size_t k) -> const std::uint8_t* { if (p + k > n) { bad("truncated pickle"); return nullptr; } const std::uint8_t* r = &d[p]; p += k; return r; };
    std::uint64_t steps = 0;
    while (p < n) {
      if (++steps > 200000000ull) return bad("pickle too long");
      std::uint8_t op = d[p++];
      switch (op) {
        case 0x80: if (!rd(1)) return false; break;                             // PROTO
        case 0x95: if (!rd(8)) return false; break;                             // FRAME
        case '.': return st.size() == 1 ? true : bad("pickle ends with an unexpected stack");  // STOP
        case '(': marks.push_back(st.size()); break;                            // MARK
        case 'N': if (!push(mk(Val::None))) return false; break;
        case 0x88: { auto v = mk(Val::Bool); v->i = 1; if (!push(v)) return false; break; }
        case 0x89: { auto v = mk(Val::Bool); if (!push(v)) return false; break; }
        case 'K': { auto b = rd(1); if (!b) return false; auto v = mk(Val::Int); v->i = *b; if (!push(v)) return false; break; }
        case 'M': { auto b = rd(2); if (!b) return false; auto v = mk(Val::Int); v->i = le16(b); if (!push(v)) return false; break; }
        case 'J': { auto b = rd(4); if (!b) return false; auto v = mk(Val::Int); v->i = std::int32_t(le32(b)); if (!push(v)) return false; break; }
        case 0x8a: {  // LONG1
          auto l = rd(1); if (!l) return false;
          std::size_t len = *l;
          auto b = rd(len); if (!b && len) return false;
          if (len > 8) return bad("long integer too large");
          std::uint64_t x = 0;
          for (std::size_t i = 0; i < len; ++i) x |= std::uint64_t(b[i]) << (8 * i);
          if (len && (b[len - 1] & 0x80) && len < 8) x |= ~0ull << (8 * len);
          auto v = mk(Val::Int); v->i = std::int64_t(x); if (!push(v)) return false; break;
        }
        case 'G': { auto b = rd(8); if (!b) return false; std::uint64_t x = 0; for (int i = 0; i < 8; ++i) x = (x << 8) | b[i]; auto v = mk(Val::Float); std::memcpy(&v->d, &x, 8); if (!push(v)) return false; break; }
        case 'X': case 0x8c: case 'B': case 'C': {  // BINUNICODE, SHORT_BINUNICODE, BINBYTES, SHORT_BINBYTES
          bool shortlen = op == 0x8c || op == 'C';
          std::size_t len;
          if (shortlen) { auto l = rd(1); if (!l) return false; len = *l; } else { auto l = rd(4); if (!l) return false; len = le32(l); }
          if (len > (1u << 24)) return bad("pickle string too long");
          auto b = rd(len); if (!b && len) return false;
          auto v = mk(op == 'X' || op == 0x8c ? Val::Str : Val::Bytes);
          v->s.assign(reinterpret_cast<const char*>(b), len);
          if (!push(v)) return false; break;
        }
        case ')': if (!push(mk(Val::Tuple))) return false; break;
        case ']': if (!push(mk(Val::List))) return false; break;
        case '}': if (!push(mk(Val::Dict))) return false; break;
        case 0x85: case 0x86: case 0x87: {  // TUPLE1/2/3
          std::size_t k = op - 0x84;
          if (!need(k)) return false;
          auto t = mk(Val::Tuple);
          t->v.assign(st.end() - long(k), st.end());
          st.resize(st.size() - k);
          if (!push(t)) return false; break;
        }
        case 't': { std::vector<VP> it; if (!pop_mark(it)) return false; auto t = mk(Val::Tuple); t->v = std::move(it); if (!push(t)) return false; break; }
        case 'a': { if (!need(2)) return false; VP x = pop(); if (st.back()->k != Val::List) return bad("APPEND to a non-list"); st.back()->v.push_back(x); break; }
        case 'e': { std::vector<VP> it; if (!pop_mark(it)) return false; if (st.empty() || st.back()->k != Val::List) return bad("APPENDS to a non-list"); for (auto& x : it) st.back()->v.push_back(x); break; }
        case 's': { if (!need(3)) return false; VP v = pop(), k = pop(); if (st.back()->k != Val::Dict && st.back()->k != Val::OrderedDict) return bad("SETITEM on a non-dict"); st.back()->kv.emplace_back(k, v); break; }
        case 'u': { std::vector<VP> it; if (!pop_mark(it)) return false; if (st.empty() || (st.back()->k != Val::Dict && st.back()->k != Val::OrderedDict) || it.size() % 2) return bad("SETITEMS on a non-dict");
                    for (std::size_t i = 0; i < it.size(); i += 2) st.back()->kv.emplace_back(it[i], it[i + 1]); break; }
        case 'q': { auto b = rd(1); if (!b) return false; if (!need(1)) return false; memo[*b] = st.back(); break; }
        case 'r': { auto b = rd(4); if (!b) return false; if (!need(1)) return false; if (memo.size() > 4000000) return bad("pickle memo too large"); memo[le32(b)] = st.back(); break; }
        case 0x94: { if (!need(1)) return false; memo[memo.size()] = st.back(); break; }  // MEMOIZE
        case 'h': { auto b = rd(1); if (!b) return false; auto it = memo.find(*b); if (it == memo.end()) return bad("bad memo reference"); if (!push(it->second)) return false; break; }
        case 'j': { auto b = rd(4); if (!b) return false; auto it = memo.find(le32(b)); if (it == memo.end()) return bad("bad memo reference"); if (!push(it->second)) return false; break; }
        case 'c': {  // GLOBAL "module\nname\n"
          std::size_t e1 = p; while (e1 < n && d[e1] != '\n') ++e1;
          std::size_t e2 = e1 + 1; while (e2 < n && d[e2] != '\n') ++e2;
          if (e2 >= n || e1 - p > 200 || e2 - e1 > 200) return bad("bad GLOBAL");
          auto g = mk(Val::Global);
          g->s = std::string(reinterpret_cast<const char*>(&d[p]), e1 - p) + " " + std::string(reinterpret_cast<const char*>(&d[e1 + 1]), e2 - e1 - 1);
          p = e2 + 1;
          if (!push(g)) return false; break;
        }
        case 'R': { if (!need(2)) return false; VP a = pop(), f = pop(); VP out; if (!call(f, a, out)) return false; if (!push(out)) return false; break; }
        case 'Q': {  // BINPERSID: ("storage", <storage class>, key, location, numel)
          if (!need(1)) return false;
          VP id = pop();
          if (id->k != Val::Tuple || id->v.size() < 5 || id->v[0]->k != Val::Str || id->v[0]->s != "storage" || id->v[1]->k != Val::Global ||
              id->v[2]->k != Val::Str || !is_int(id->v[4]) || id->v[4]->i < 0) return bad("unsupported persistent id");
          const std::string& g = id->v[1]->s;
          std::size_t sp = g.find(' ');
          const char* dt = g.compare(0, sp, "torch") == 0 ? storage_dtype(g.substr(sp + 1)) : nullptr;
          if (!dt) return bad("unsupported storage type " + g);
          auto s = mk(Val::Storage);
          s->s = id->v[2]->s; s->dtype = dt; s->numel = std::uint64_t(id->v[4]->i);
          if (s->s.size() > 32) return bad("bad storage key");
          if (!push(s)) return false; break;
        }
        case 'b': {  // BUILD: only "set metadata on an OrderedDict" is accepted
          if (!need(2)) return false;
          VP state = pop();
          if (st.back()->k == Val::Opaque) break;  // numpy dtype state: nothing is executed, the value stays opaque
          if (st.back()->k != Val::OrderedDict || (state->k != Val::Dict && state->k != Val::OrderedDict)) return bad("BUILD on an unsupported object");
          break;
        }
        default: return bad("pickle opcode not allowed: 0x" + [&] { char b[8]; std::snprintf(b, sizeof b, "%02x", op); return std::string(b); }());
      }
    }
    return bad("pickle has no STOP");
  }
};

void collect(const VP& dict, const std::string& prefix, std::vector<CkptTensor>& out, std::uint64_t max, std::string& err, int depth = 0) {
  if (depth > 8) return;
  for (auto& kv : dict->kv) {
    if (kv.first->k != Val::Str) continue;
    std::string name = prefix.empty() ? kv.first->s : prefix + "." + kv.first->s;
    if (kv.second->k == Val::Tensor) {
      if (out.size() >= max) { err = "too many tensors"; return; }
      CkptTensor t = kv.second->t;
      t.name = name;
      out.push_back(std::move(t));
    }
  }
}

}  // namespace

bool read_checkpoint(const fs::path& file, Checkpoint& out, std::string* error, const CkptLimits& lim) {
  out = Checkpoint();
  std::FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) return fail(error, "cannot open " + file.string());
  struct Close { std::FILE* f; ~Close() { std::fclose(f); } } closer{f};
  Zip z;
  if (!open_zip(f, z, lim, error)) return false;
  std::string pk_name;
  for (auto& kv : z.files) {
    const std::string& n = kv.first;
    if (n.size() >= 8 && n.compare(n.size() - 8, 8, "data.pkl") == 0 && (n.size() == 8 || n[n.size() - 9] == '/')) { pk_name = n; break; }
  }
  if (pk_name.empty()) return fail(error, "no data.pkl in archive (not a PyTorch zip checkpoint)");
  const ZipEntry& pe = z.files[pk_name];
  if (pe.size > lim.max_pickle_bytes) return fail(error, "pickle too large");
  std::vector<std::uint8_t> pk;
  pk.resize(static_cast<std::size_t>(pe.size));
  if (!read_at(f, pe.offset, pk.data(), pk.size())) return fail(error, "read error");
  std::string prefix = pk_name.substr(0, pk_name.size() - 8);  // "archive/"
  Interp in(pk, lim);
  if (!in.run()) return fail(error, "checkpoint rejected: " + in.err);
  VP root = in.st.back();
  VP state = root;
  if (root->k == Val::Dict) {  // Demucs "package": {klass, args, kwargs, state, ...}
    VP st2;
    for (auto& kv : root->kv) {
      if (kv.first->k == Val::Str && kv.first->s == "state") st2 = kv.second;
      if (kv.first->k == Val::Str && kv.first->s == "klass" && kv.second->k == Val::Global) out.wrapper_class = kv.second->s;
    }
    if (st2) state = st2;
  }
  if (state->k != Val::OrderedDict && state->k != Val::Dict) return fail(error, "checkpoint does not contain a state dict");
  // a "state_dict" key wrapping (some trainers) is accepted too
  for (auto& kv : state->kv)
    if (kv.first->k == Val::Str && kv.first->s == "state_dict" && (kv.second->k == Val::OrderedDict || kv.second->k == Val::Dict)) { state = kv.second; break; }
  std::string err;
  collect(state, "", out.tensors, lim.max_tensors, err);
  if (!err.empty()) return fail(error, err);
  for (auto& t : out.tensors) {
    t.member = prefix + "data/" + t.member;
    auto it = z.files.find(t.member);
    if (it == z.files.end()) return fail(error, "tensor storage missing from archive: " + t.member);
  }
  if (out.tensors.empty()) return fail(error, "no tensors found");
  return true;
}

bool read_tensor_f32(const fs::path& file, const CkptTensor& t, std::vector<float>& out, std::string* error) {
  std::FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) return fail(error, "cannot open " + file.string());
  struct Close { std::FILE* f; ~Close() { std::fclose(f); } } closer{f};
  Zip z;
  CkptLimits lim;
  if (!open_zip(f, z, lim, error)) return false;
  auto it = z.files.find(t.member);
  if (it == z.files.end()) return fail(error, "storage missing");
  std::size_t es = t.dtype == "float32" || t.dtype == "int32" ? 4 : t.dtype == "float16" || t.dtype == "bfloat16" || t.dtype == "int16" ? 2 :
                   t.dtype == "float64" || t.dtype == "int64" ? 8 : t.dtype == "int8" || t.dtype == "uint8" || t.dtype == "bool" ? 1 : 0;
  if (!es) return fail(error, "unsupported dtype " + t.dtype);
  std::uint64_t elems = it->second.size / es;
  std::uint64_t last = std::uint64_t(t.offset);
  for (std::size_t k = 0; k < t.shape.size(); ++k) if (t.shape[k] > 0) last += std::uint64_t(t.shape[k] - 1) * std::uint64_t(t.stride[k]);
  if (t.numel && last >= elems) return fail(error, "tensor exceeds its storage");
  std::vector<std::uint8_t> raw;
  raw.resize(static_cast<std::size_t>(it->second.size));
  if (!read_at(f, it->second.offset, raw.data(), raw.size())) return fail(error, "read error");
  auto get = [&](std::uint64_t idx) -> float {
    const std::uint8_t* p = &raw[std::size_t(idx) * es];
    if (t.dtype == "float32") { std::uint32_t u = le32(p); float v; std::memcpy(&v, &u, 4); return v; }
    if (t.dtype == "float16") {
      std::uint16_t h = le16(p);
      int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
      float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), e - 25);
      return s ? -v : v;
    }
    if (t.dtype == "bfloat16") { std::uint32_t u = std::uint32_t(le16(p)) << 16; float v; std::memcpy(&v, &u, 4); return v; }
    if (t.dtype == "float64") { std::uint64_t u = le64(p); double v; std::memcpy(&v, &u, 8); return float(v); }
    if (t.dtype == "int64") return float(std::int64_t(le64(p)));
    if (t.dtype == "int32") return float(std::int32_t(le32(p)));
    if (t.dtype == "int16") return float(std::int16_t(le16(p)));
    if (t.dtype == "int8") return float(std::int8_t(p[0]));
    return float(p[0]);
  };
  out.assign(std::size_t(t.numel), 0.f);
  std::vector<std::int64_t> idx(t.shape.size(), 0);
  for (std::uint64_t n = 0; n < t.numel; ++n) {
    std::uint64_t off = std::uint64_t(t.offset);
    for (std::size_t k = 0; k < idx.size(); ++k) off += std::uint64_t(idx[k]) * std::uint64_t(t.stride[k]);
    out[std::size_t(n)] = get(off);
    for (std::size_t k = idx.size(); k-- > 0;) { if (++idx[k] < t.shape[k]) break; idx[k] = 0; }
  }
  return true;
}

}  // namespace deckhand::stems
