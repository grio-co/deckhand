#include "onnx_proto.hpp"

#include <cstring>
#include <fstream>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace deckhand::stems::graph {

Tensor make_owned(DType dt, const Shape& s) {
  Tensor t;
  t.dt = dt;
  t.shape = s;
  std::size_t n = std::size_t(s.numel()) * dtype_size(dt);
#if defined(__unix__) || defined(__APPLE__)
  if (n >= (1u << 20)) {
    // Large tensors come straight from the kernel and go straight back on release, so the planning run's temporary
    // buffers do not stay resident (malloc would keep them cached) once the arena exists. Anonymous pages arrive zeroed.
    void* p = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) throw GraphError("out of memory allocating a " + std::to_string(n >> 20) + " MB tensor");
    t.data = p;
    t.owner = std::shared_ptr<void>(p, [n](void* q) { ::munmap(q, n); });
    return t;
  }
#endif
  auto buf = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[n ? n + 64 : 64]());
  t.data = buf.get();
  t.owner = buf;
  return t;
}

namespace {

[[noreturn]] void bad(const char* m) { throw GraphError(std::string("malformed ONNX file: ") + m); }

struct Reader {
  const std::uint8_t* p;
  const std::uint8_t* end;
  const ParseLimits* lim;
  int depth;

  bool done() const { return p >= end; }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      if (p >= end) bad("truncated varint");
      std::uint8_t b = *p++;
      v |= std::uint64_t(b & 0x7f) << shift;
      if (!(b & 0x80)) return v;
    }
    bad("varint too long");
  }
  std::uint32_t fixed32() {
    if (end - p < 4) bad("truncated fixed32");
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  Reader sub() {  // length-delimited payload
    std::uint64_t n = varint();
    if (n > std::uint64_t(end - p)) bad("length beyond end of message");
    Reader r{p, p + n, lim, depth + 1};
    if (r.depth > lim->max_depth) bad("nesting too deep");
    p += n;
    return r;
  }
  std::string str() {
    Reader r = sub();
    std::size_t n = std::size_t(r.end - r.p);
    if (n > lim->max_string) bad("string too long");
    return std::string(reinterpret_cast<const char*>(r.p), n);
  }
  void skip(unsigned wt) {
    switch (wt) {
      case 0: varint(); break;
      case 1: if (end - p < 8) bad("truncated fixed64"); p += 8; break;
      case 2: { std::uint64_t n = varint(); if (n > std::uint64_t(end - p)) bad("length beyond end"); p += n; break; }
      case 5: if (end - p < 4) bad("truncated fixed32"); p += 4; break;
      default: bad("unsupported wire type");
    }
  }
};

struct Tag { std::uint32_t field; unsigned wt; };
inline Tag read_tag(Reader& r) {
  std::uint64_t t = r.varint();
  if ((t >> 3) == 0 || (t >> 3) > 0x1fffffff) bad("bad field number");
  return {std::uint32_t(t >> 3), unsigned(t & 7)};
}

void packed_or_single_i64(Reader& r, unsigned wt, std::vector<std::int64_t>& out, std::size_t cap) {
  if (wt == 2) {
    Reader s = r.sub();
    while (!s.done()) {
      if (out.size() >= cap) bad("too many values");
      out.push_back(std::int64_t(s.varint()));
    }
  } else if (wt == 0) {
    if (out.size() >= cap) bad("too many values");
    out.push_back(std::int64_t(r.varint()));
  } else bad("bad wire type for integer list");
}

TensorProto parse_tensor(Reader r) {
  TensorProto t;
  const std::size_t cap = r.lim->max_attr_values;
  std::string key, val;
  while (!r.done()) {
    Tag g = read_tag(r);
    switch (g.field) {
      case 1: packed_or_single_i64(r, g.wt, t.dims, kMaxRank + 1); break;
      case 2: if (g.wt != 0) bad("data_type"); t.data_type = int(r.varint()); break;
      case 4:  // float_data
        if (g.wt == 2) {
          Reader s = r.sub();
          std::size_t n = std::size_t(s.end - s.p);
          if (n % 4 || n / 4 > cap) bad("float_data");
          t.float_data.resize(n / 4);
          if (n) std::memcpy(t.float_data.data(), s.p, n);
        } else if (g.wt == 5) {
          std::uint32_t u = r.fixed32(); float f; std::memcpy(&f, &u, 4);
          if (t.float_data.size() >= cap) bad("float_data");
          t.float_data.push_back(f);
        } else bad("float_data wire type");
        break;
      case 5: case 7: case 11:  // int32_data, int64_data, uint64_data
        packed_or_single_i64(r, g.wt, t.int_data, cap);
        break;
      case 8: t.name = r.str(); break;
      case 9: { Reader s = r.sub(); t.raw = s.p; t.raw_len = std::size_t(s.end - s.p); break; }
      case 13: {  // external_data: StringStringEntryProto
        Reader s = r.sub();
        key.clear(); val.clear();
        while (!s.done()) {
          Tag e = read_tag(s);
          if (e.field == 1 && e.wt == 2) key = s.str();
          else if (e.field == 2 && e.wt == 2) val = s.str();
          else s.skip(e.wt);
        }
        auto num = [&](const std::string& v) {
          if (v.empty() || v.size() > 18) bad("external data number");
          std::int64_t x = 0;
          for (char c : v) { if (c < '0' || c > '9') bad("external data number"); x = x * 10 + (c - '0'); }
          return x;
        };
        if (key == "location") t.location = val;
        else if (key == "offset") t.offset = num(val);
        else if (key == "length") t.length = num(val);
        break;
      }
      case 14: if (g.wt != 0) bad("data_location"); t.external = r.varint() == 1; break;
      default: r.skip(g.wt);
    }
  }
  if (!t.location.empty()) t.external = true;
  return t;
}

Attribute parse_attr(Reader r) {
  Attribute a;
  const std::size_t cap = r.lim->max_attr_values;
  while (!r.done()) {
    Tag g = read_tag(r);
    switch (g.field) {
      case 1: a.name = r.str(); break;
      case 2: if (g.wt != 5) bad("attr f"); { std::uint32_t u = r.fixed32(); std::memcpy(&a.f, &u, 4); } break;
      case 3: if (g.wt != 0) bad("attr i"); a.i = std::int64_t(r.varint()); break;
      case 4: a.s = r.str(); break;
      case 5: a.t = parse_tensor(r.sub()); break;
      case 6: r.skip(g.wt); a.has_graph = true; break;
      case 7:
        if (g.wt == 2) {
          Reader s = r.sub();
          std::size_t n = std::size_t(s.end - s.p);
          if (n % 4 || n / 4 > cap) bad("floats");
          a.floats.resize(n / 4);
          if (n) std::memcpy(a.floats.data(), s.p, n);
        } else if (g.wt == 5) {
          std::uint32_t u = r.fixed32(); float f; std::memcpy(&f, &u, 4);
          if (a.floats.size() >= cap) bad("floats");
          a.floats.push_back(f);
        } else bad("floats wire type");
        break;
      case 8: packed_or_single_i64(r, g.wt, a.ints, cap); break;
      case 20: if (g.wt != 0) bad("attr type"); a.type = int(r.varint()); break;
      default: r.skip(g.wt);
    }
  }
  if (a.type == 0) {  // very old writers omit the type
    if (!a.ints.empty()) a.type = 7; else if (!a.floats.empty()) a.type = 6;
  }
  return a;
}

NodeProto parse_node(Reader r) {
  NodeProto n;
  while (!r.done()) {
    Tag g = read_tag(r);
    switch (g.field) {
      case 1: n.inputs.push_back(r.str()); break;
      case 2: n.outputs.push_back(r.str()); break;
      case 3: n.name = r.str(); break;
      case 4: n.op_type = r.str(); break;
      case 5: if (n.attrs.size() >= 256) bad("too many attributes"); n.attrs.push_back(parse_attr(r.sub())); break;
      case 7: n.domain = r.str(); break;
      default: r.skip(g.wt);
    }
  }
  if (n.inputs.size() > 64 || n.outputs.size() > 64) bad("node arity");
  return n;
}

ValueInfo parse_value_info(Reader r) {
  ValueInfo v;
  while (!r.done()) {
    Tag g = read_tag(r);
    if (g.field == 1 && g.wt == 2) v.name = r.str();
    else if (g.field == 2 && g.wt == 2) {  // TypeProto
      Reader ty = r.sub();
      while (!ty.done()) {
        Tag t = read_tag(ty);
        if (t.field == 1 && t.wt == 2) {  // tensor_type
          Reader tt = ty.sub();
          while (!tt.done()) {
            Tag u = read_tag(tt);
            if (u.field == 1 && u.wt == 0) v.elem_type = int(tt.varint());
            else if (u.field == 2 && u.wt == 2) {  // shape
              Reader sh = tt.sub();
              while (!sh.done()) {
                Tag d = read_tag(sh);
                if (d.field == 1 && d.wt == 2) {
                  Reader dim = sh.sub();
                  std::int64_t val = -1;
                  while (!dim.done()) {
                    Tag e = read_tag(dim);
                    if (e.field == 1 && e.wt == 0) val = std::int64_t(dim.varint());
                    else dim.skip(e.wt);
                  }
                  if (v.dims.size() >= 64) bad("rank");
                  v.dims.push_back(val);
                } else sh.skip(d.wt);
              }
            } else tt.skip(u.wt);
          }
        } else ty.skip(t.wt);
      }
    } else r.skip(g.wt);
  }
  return v;
}

void parse_graph(Reader r, ModelProto& m) {
  while (!r.done()) {
    Tag g = read_tag(r);
    switch (g.field) {
      case 1:
        if (g.wt != 2) bad("node");
        if (m.nodes.size() >= r.lim->max_nodes) bad("too many nodes");
        m.nodes.push_back(parse_node(r.sub()));
        break;
      case 5:
        if (g.wt != 2) bad("initializer");
        if (m.initializers.size() >= r.lim->max_initializers) bad("too many initializers");
        m.initializers.push_back(parse_tensor(r.sub()));
        break;
      case 11: if (g.wt != 2) bad("input"); m.inputs.push_back(parse_value_info(r.sub())); break;
      case 12: if (g.wt != 2) bad("output"); m.outputs.push_back(parse_value_info(r.sub())); break;
      case 15: throw GraphError("sparse initializers are not supported");
      default: r.skip(g.wt);
    }
  }
}

}  // namespace

ModelProto parse_model(std::shared_ptr<std::vector<std::uint8_t>> buffer, const ParseLimits& lim) {
  ModelProto m;
  m.buffer = std::move(buffer);
  Reader r{m.buffer->data(), m.buffer->data() + m.buffer->size(), &lim, 0};
  bool have_graph = false;
  while (!r.done()) {
    Tag g = read_tag(r);
    switch (g.field) {
      case 1: if (g.wt != 0) bad("ir_version"); m.ir_version = std::int64_t(r.varint()); break;
      case 2: m.producer = r.str(); break;
      case 7: if (g.wt != 2) bad("graph"); parse_graph(r.sub(), m); have_graph = true; break;
      case 8: {  // opset_import
        Reader s = r.sub();
        std::string domain;
        std::int64_t ver = 0;
        while (!s.done()) {
          Tag e = read_tag(s);
          if (e.field == 1 && e.wt == 2) domain = s.str();
          else if (e.field == 2 && e.wt == 0) ver = std::int64_t(s.varint());
          else s.skip(e.wt);
        }
        if (domain.empty() || domain == "ai.onnx") m.opset = ver;
        break;
      }
      default: r.skip(g.wt);
    }
  }
  if (!have_graph) bad("no graph");
  return m;
}

ModelProto load_model_file(const std::string& path, const ParseLimits& lim) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw GraphError("cannot open " + path);
  std::streamoff n = f.tellg();
  if (n < 0 || n > (std::streamoff(1) << 31)) throw GraphError("model file size not accepted: " + path);
  auto buf = std::make_shared<std::vector<std::uint8_t>>(std::size_t(n));
  f.seekg(0);
  if (n && !f.read(reinterpret_cast<char*>(buf->data()), n)) throw GraphError("cannot read " + path);
  return parse_model(std::move(buf), lim);
}

Tensor decode_tensor(const TensorProto& t, const std::uint8_t* ext, std::size_t ext_len) {
  Shape s;
  std::int64_t n = 1;
  for (auto d : t.dims) {
    if (d < 0 || d > (std::int64_t(1) << 40)) throw GraphError("tensor '" + t.name + "': bad dimension");
    if (d != 0 && n > (std::int64_t(1) << 40) / d) throw GraphError("tensor '" + t.name + "': too large");
    n *= d;
    s.push(d);
  }
  DType dt;
  std::size_t src_size;
  switch (t.data_type) {
    case 1: dt = DType::F32; src_size = 4; break;
    case 7: dt = DType::I64; src_size = 8; break;
    case 6: dt = DType::I64; src_size = 4; break;   // int32 held as int64
    case 9: dt = DType::Bool; src_size = 1; break;
    case 2: case 3: dt = DType::I64; src_size = 1; break;  // uint8 / int8
    default: throw GraphError("tensor '" + t.name + "': unsupported data type " + std::to_string(t.data_type));
  }
  Tensor out = make_owned(dt, s);
  const std::uint8_t* src = nullptr;
  std::size_t src_len = 0;
  if (t.external) {
    if (!ext) throw GraphError("tensor '" + t.name + "': external data not available");
    src = ext; src_len = ext_len;
  } else if (t.raw) { src = t.raw; src_len = t.raw_len; }
  if (src) {
    if (src_len != std::size_t(n) * src_size) throw GraphError("tensor '" + t.name + "': data length does not match its shape");
    if (src_size == dtype_size(dt)) { if (src_len) std::memcpy(out.data, src, src_len); }
    else for (std::int64_t i = 0; i < n; ++i) {
      std::int64_t v = 0;
      if (src_size == 4) { std::int32_t x; std::memcpy(&x, src + 4 * i, 4); v = x; }
      else if (t.data_type == 3) v = std::int8_t(src[i]);
      else v = src[i];
      out.i()[i] = v;
    }
    return out;
  }
  if (dt == DType::F32) {
    if (std::int64_t(t.float_data.size()) != n) throw GraphError("tensor '" + t.name + "': float_data length does not match its shape");
    if (n) std::memcpy(out.data, t.float_data.data(), std::size_t(n) * 4);
  } else {
    if (std::int64_t(t.int_data.size()) != n) throw GraphError("tensor '" + t.name + "': integer data length does not match its shape");
    for (std::int64_t i = 0; i < n; ++i) {
      if (dt == DType::I64) out.i()[i] = t.int_data[std::size_t(i)];
      else out.b()[i] = t.int_data[std::size_t(i)] != 0;
    }
  }
  return out;
}

}  // namespace deckhand::stems::graph
