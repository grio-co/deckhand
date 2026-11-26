#include "executor.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace deckhand::stems::graph {
namespace fs = std::filesystem;

namespace {
constexpr std::int64_t kFoldLimit = 1 << 20;  // larger constant-input results are recomputed instead of kept
constexpr std::size_t kAlign = 64;
float half_to_float(std::uint16_t h) {
  std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 1023u, bits;
  if (e == 0) {
    if (m == 0) bits = sign;
    else { int sh = 0; while (!(m & 0x400u)) { m <<= 1; ++sh; } m &= 1023u; bits = sign | (std::uint32_t(113 - sh) << 23) | (m << 13); }
  } else if (e == 31) bits = sign | 0x7f800000u | (m << 13);
  else bits = sign | ((e + 112u) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
std::size_t round_up(std::size_t n) { return (n + kAlign - 1) / kAlign * kAlign; }
}  // namespace

std::uint8_t* SharedArena::ensure(std::size_t bytes) {
  if (bytes > size_) {
    Tensor t = make_owned(DType::Bool, Shape{std::int64_t(bytes + kAlign)});  // anonymous pages for big arenas
    owner_ = t.owner;
    base_ = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(t.data) + kAlign - 1) / kAlign * kAlign);
    size_ = bytes;
  }
  return base_;
}

struct Executor::Impl {
  struct Value {
    std::string name;
    Tensor t;
    bool is_static = false, is_input = false, is_output = false;
    int def = -1;
    int last_use = -1;
    int alias_of = -1;      // value this one is a view of (set by the planning run)
    int root = -1;          // arena root (value id) or -1 when it lives outside the arena
    std::ptrdiff_t off = -1;
    DType dt = DType::F32;  // recorded by the planning run
    Shape shape;
  };
  struct Node {
    const NodeProto* proto = nullptr;
    OpFn fn = nullptr;
    std::vector<int> in, out;
    std::unique_ptr<bool[]> wanted;
    bool dynamic = true;
  };

  ModelProto model;
  std::vector<Value> vals;
  std::vector<Node> nodes;
  std::unordered_map<std::string, int> by_name;
  std::vector<std::string> in_names, out_names;
  std::vector<int> input_ids, output_ids, schedule;
  std::vector<std::vector<float>> scratch{kScratchSlots};
  std::unique_ptr<ThreadPool> pool;
  std::uint8_t* arena = nullptr;
  std::shared_ptr<SharedArena> shared;
  std::size_t arena_size = 0, weights_size = 0;
  bool planned = false, keep_all = false, profiling = false;
  std::map<std::string, double> prof;
  std::vector<Shape> plan_shapes;
  std::vector<std::shared_ptr<std::vector<std::uint8_t>>> weight_files;

  // ---- per-run allocation hooks ----
  struct RunState { Impl* self; const Node* node; bool planning; };
  static void alloc_hook(OpCtx& c, int idx, DType dt, const Shape& s) {
    auto* rs = static_cast<RunState*>(c.user);
    for (int k = 0; k < s.rank; ++k)
      if (s[k] < 0) throw GraphError("internal: negative output dimension");
    if (rs->planning) { c.outs[idx] = make_owned(dt, s); return; }
    int v = rs->node->out[std::size_t(idx)];
    Value& val = rs->self->vals[std::size_t(v)];
    if (val.off < 0 || val.dt != dt || val.shape != s)
      throw GraphError("internal: output " + val.name + " changed shape or type after planning");
    c.outs[idx].dt = dt; c.outs[idx].shape = s; c.outs[idx].data = rs->self->arena + val.off; c.outs[idx].owner.reset();
  }

  int id_of(const std::string& n) {
    auto it = by_name.find(n);
    return it == by_name.end() ? -1 : it->second;
  }
  int define(const std::string& n, int def) {
    if (by_name.count(n)) throw GraphError("value '" + n + "' is defined twice");
    int id = int(vals.size());
    vals.emplace_back();
    vals.back().name = n;
    vals.back().def = def;
    by_name[n] = id;
    return id;
  }

  std::shared_ptr<std::vector<std::uint8_t>> load_weights(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw GraphError("cannot open weights file " + path);
    std::streamoff n = f.tellg();
    if (n < 0) throw GraphError("cannot size weights file " + path);
    auto b = std::make_shared<std::vector<std::uint8_t>>(std::size_t(n) + 64);
    f.seekg(0);
    if (n && !f.read(reinterpret_cast<char*>(b->data()), n)) throw GraphError("cannot read weights file " + path);
    b->resize(std::size_t(n));
    weights_size += std::size_t(n);
    return b;
  }

  void build(const std::string& weights_dir, const std::string& override_path) {
    std::map<std::string, std::shared_ptr<std::vector<std::uint8_t>>> files;
    for (const TensorProto& tp : model.initializers) {
      int id = define(tp.name, -1);
      Value& v = vals[std::size_t(id)];
      v.is_static = true;
      if (tp.external) {
        std::string path;
        if (!override_path.empty()) path = override_path;
        else {
          fs::path loc(tp.location);
          if (tp.location.empty() || loc.is_absolute() || tp.location.find("..") != std::string::npos)
            throw GraphError("initializer '" + tp.name + "': unsafe external data location");
          path = (fs::path(weights_dir) / loc).string();
        }
        auto& f = files[path];
        if (!f) { f = load_weights(path); weight_files.push_back(f); }
        std::int64_t numel = 1;
        for (auto d : tp.dims) { if (d < 0 || d > (std::int64_t(1) << 40)) throw GraphError("bad dimension in '" + tp.name + "'"); numel *= d; if (numel > (std::int64_t(1) << 40)) throw GraphError("tensor too large"); }
        std::int64_t esz = tp.data_type == 1 ? 4 : tp.data_type == 7 ? 8 : tp.data_type == 10 ? 2 : 0;
        if (!esz) throw GraphError("external initializer '" + tp.name + "' has an unsupported type");
        std::int64_t len = tp.length >= 0 ? tp.length : numel * esz;
        if (tp.offset < 0 || len != numel * esz || std::uint64_t(tp.offset) + std::uint64_t(len) > f->size())
          throw GraphError("initializer '" + tp.name + "' points outside the weights file");
        Tensor t;
        t.dt = tp.data_type == 7 ? DType::I64 : DType::F32;
        for (auto d : tp.dims) t.shape.push(d);
        if (tp.data_type == 10) {  // half precision on disk, float32 compute: widen once at load
          Tensor w = make_owned(DType::F32, t.shape);
          const std::uint8_t* src = f->data() + tp.offset;
          float* dst = w.f();
          for (std::int64_t i = 0; i < numel; ++i) dst[i] = half_to_float(std::uint16_t(src[2 * i] | (src[2 * i + 1] << 8)));
          weights_size += w.bytes();
          v.t = w;
          continue;
        }
        t.data = f->data() + tp.offset;
        t.owner = f;
        v.t = t;
      } else {
        v.t = decode_tensor(tp, nullptr, 0);
        weights_size += v.t.bytes();
      }
    }
    for (const ValueInfo& vi : model.inputs) {
      in_names.push_back(vi.name);
      int id = id_of(vi.name);
      if (id < 0) { id = define(vi.name, -1); vals[std::size_t(id)].is_input = true; input_ids.push_back(id); }
    }
    nodes.resize(model.nodes.size());
    for (std::size_t k = 0; k < model.nodes.size(); ++k) {
      const NodeProto& np = model.nodes[k];
      std::string where = "node " + std::to_string(k) + " (" + np.op_type + (np.name.empty() ? "" : " '" + np.name + "'") + ")";
      if (!np.domain.empty() && np.domain != "ai.onnx") throw GraphError("unsupported op " + np.op_type + " in domain '" + np.domain + "' at " + where);
      const OpFn* fn = find_op(np.op_type);
      if (!fn) throw GraphError("unsupported op " + np.op_type + " at " + where);
      for (auto& a : np.attrs) if (a.has_graph) throw GraphError("subgraph attributes are not supported at " + where);
      Node& nd = nodes[k];
      nd.proto = &np;
      nd.fn = *fn;
      for (auto& nm : np.inputs) {
        if (nm.empty()) { nd.in.push_back(-1); continue; }
        int id = id_of(nm);
        if (id < 0) throw GraphError(where + " uses '" + nm + "' before it is defined");
        nd.in.push_back(id);
        vals[std::size_t(id)].last_use = std::max(vals[std::size_t(id)].last_use, int(k));
      }
      nd.wanted.reset(new bool[np.outputs.size() + 1]);
      for (std::size_t o = 0; o < np.outputs.size(); ++o) {
        nd.wanted[o] = !np.outputs[o].empty();
        nd.out.push_back(np.outputs[o].empty() ? -1 : define(np.outputs[o], int(k)));
      }
    }
    for (auto& vi : model.outputs) {
      out_names.push_back(vi.name);
      int id = id_of(vi.name);
      if (id < 0) throw GraphError("graph output '" + vi.name + "' is never produced");
      output_ids.push_back(id);
      vals[std::size_t(id)].is_output = true;
    }
    for (auto& v : vals) if (v.last_use < v.def) v.last_use = v.def;
    weight_files.clear();  // tensors that view a file keep it alive themselves; widened (half) files are freed here
    pool = std::make_unique<ThreadPool>(1);
  }

  void finish_value(int v, const Tensor& t) {
    Value& val = vals[std::size_t(v)];
    val.t = t;
    val.dt = t.dt;
    val.shape = t.shape;
  }

  void execute(std::size_t k, bool planning) {
    Node& nd = nodes[k];
    const Tensor* ins[64];
    int nin = int(nd.in.size());
    for (int i = 0; i < nin; ++i) ins[i] = nd.in[std::size_t(i)] < 0 ? nullptr : &vals[std::size_t(nd.in[std::size_t(i)])].t;
    Tensor outs[64];
    int nout = int(nd.out.size());
    if (nout > 64) throw GraphError("too many outputs");
    OpCtx c;
    c.pool = pool.get();
    c.scratch = &scratch;
    c.outs = outs;
    c.nouts = nout;
    c.wanted = nd.wanted.get();
    RunState rs{this, &nd, planning};
    c.user = &rs;
    c.alloc_fn = &alloc_hook;
    nd.fn(c, *nd.proto, ins, nin);
    for (int o = 0; o < nout; ++o) {
      int v = nd.out[std::size_t(o)];
      if (v < 0) continue;
      if (!outs[o].data && outs[o].numel() != 0) throw GraphError("operator produced no output '" + vals[std::size_t(v)].name + "'");
      if (planning) finish_value(v, outs[o]);
      else vals[std::size_t(v)].t = std::move(outs[o]);
    }
  }

  std::string describe(std::size_t k) const {
    const NodeProto& np = model.nodes[k];
    return "node " + std::to_string(k) + " (" + np.op_type + (np.name.empty() ? "" : " '" + np.name + "'") + ")";
  }

  void plan_run() {
    // Reset everything except initializers.
    for (auto& v : vals) {
      if (v.def < 0 && !v.is_input) continue;
      v.is_static = false; v.alias_of = -1; v.root = -1; v.off = -1;
      if (!v.is_input) v.t = Tensor();
    }
    schedule.clear();
    std::vector<std::vector<int>> release(nodes.size() + 1);
    for (std::size_t i = 0; i < vals.size(); ++i) {
      const Value& v = vals[i];
      if (v.def >= 0 && !v.is_output && v.last_use >= 0) release[std::size_t(v.last_use)].push_back(int(i));
    }
    for (std::size_t k = 0; k < nodes.size(); ++k) {
      Node& nd = nodes[k];
      bool all_static = true;
      for (int v : nd.in) if (v >= 0 && !vals[std::size_t(v)].is_static) all_static = false;
      bool stat = all_static || nd.proto->op_type == "Shape";
      try { execute(k, true); } catch (const GraphError& e) { throw GraphError(describe(k) + ": " + e.what()); }
      if (stat) {
        bool big = false;
        for (int v : nd.out) if (v >= 0 && vals[std::size_t(v)].t.dt == DType::F32 && vals[std::size_t(v)].t.numel() > kFoldLimit) big = true;
        if (big) stat = false;
      }
      nd.dynamic = !stat;
      for (int v : nd.out) if (v >= 0) vals[std::size_t(v)].is_static = stat;
      if (!stat) {
        schedule.push_back(int(k));
        // alias detection: an output sharing storage with an input is a view of it
        for (int o : nd.out) {
          if (o < 0) continue;
          for (int i : nd.in)
            if (i >= 0 && vals[std::size_t(i)].t.data && vals[std::size_t(o)].t.data == vals[std::size_t(i)].t.data) { vals[std::size_t(o)].alias_of = i; break; }
        }
      }
      if (!keep_all)
        for (int v : release[k])
          if (!vals[std::size_t(v)].is_static && !vals[std::size_t(v)].is_output) { vals[std::size_t(v)].t.data = nullptr; vals[std::size_t(v)].t.owner.reset(); }
    }
    layout();
    planned = true;
  }

  void layout() {
    const int INF = INT_MAX;
    // roots
    for (int k : schedule)
      for (int o : nodes[std::size_t(k)].out) {
        if (o < 0) continue;
        Value& v = vals[std::size_t(o)];
        if (v.alias_of >= 0) { const Value& a = vals[std::size_t(v.alias_of)]; v.root = (a.is_static || a.is_input || a.def < 0) ? -1 : a.root; }
        else v.root = o;
      }
    std::vector<int> last(vals.size(), -1);
    for (std::size_t i = 0; i < vals.size(); ++i) {
      const Value& v = vals[i];
      if (v.root < 0 || v.is_static) continue;
      int lu = (keep_all || v.is_output) ? INF : v.last_use;
      last[std::size_t(v.root)] = std::max(last[std::size_t(v.root)], lu);
    }
    struct Blk { std::size_t off, size; int until; };
    std::vector<Blk> live;
    std::size_t high = 0;
    for (int k : schedule) {
      for (int o : nodes[std::size_t(k)].out) {
        if (o < 0) continue;
        Value& v = vals[std::size_t(o)];
        if (v.root != o) continue;
        std::size_t sz = round_up(v.t.bytes());
        if (sz == 0) { v.off = 0; continue; }
        std::sort(live.begin(), live.end(), [](const Blk& a, const Blk& b) { return a.off < b.off; });
        std::size_t pos = 0;
        bool placed = false;
        for (auto& b : live) {
          if (b.off >= pos + sz) { placed = true; break; }
          pos = std::max(pos, b.off + b.size);
        }
        (void)placed;
        v.off = std::ptrdiff_t(pos);
        live.push_back({pos, sz, last[std::size_t(o)]});
        high = std::max(high, pos + sz);
      }
      live.erase(std::remove_if(live.begin(), live.end(), [&](const Blk& b) { return b.until <= k; }), live.end());
    }
    // views and their roots share an offset
    for (int k : schedule)
      for (int o : nodes[std::size_t(k)].out) {
        if (o < 0) continue;
        Value& v = vals[std::size_t(o)];
        if (v.root >= 0 && v.root != o) v.off = -1;  // alias: no allocation of its own
      }
    arena_size = high;
    if (!shared) shared = std::make_shared<SharedArena>();
    arena = shared->ensure(high);
    // The planning run left heap-owned outputs; they stay valid until the next run replaces them.
  }

  void fast_run() {
    if (shared) arena = shared->data();
    for (int k : schedule) {
      auto t0 = profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
      try { execute(std::size_t(k), false); } catch (const GraphError& e) { throw GraphError(describe(std::size_t(k)) + ": " + e.what()); }
      if (profiling) prof[nodes[std::size_t(k)].proto->op_type] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
  }
};

Executor::Executor(std::unique_ptr<Impl> i) : p_(std::move(i)) {}
Executor::~Executor() = default;

std::unique_ptr<Executor> Executor::from_proto(ModelProto&& m, const std::string& weights_dir, const std::string& weights_override,
                                               int threads, std::string* err) {
  try {
    auto impl = std::make_unique<Impl>();
    impl->model = std::move(m);
    impl->build(weights_dir, weights_override);
    impl->pool = std::make_unique<ThreadPool>(threads);
    return std::unique_ptr<Executor>(new Executor(std::move(impl)));
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return nullptr;
  }
}

std::unique_ptr<Executor> Executor::open(const std::string& model_path, const std::string& weights_override, int threads,
                                         std::string* err) {
  try {
    ModelProto m = load_model_file(model_path);
    return from_proto(std::move(m), fs::path(model_path).parent_path().string(), weights_override, threads, err);
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return nullptr;
  }
}

void Executor::set_profiling(bool on) { p_->profiling = on; }
std::map<std::string, double> Executor::profile() const { return p_->prof; }
void Executor::set_shared_arena(std::shared_ptr<SharedArena> a) { p_->shared = std::move(a); }
void Executor::set_threads(int t) { p_->pool = std::make_unique<ThreadPool>(t); }
int Executor::threads() const { return p_->pool->threads(); }
void Executor::set_keep_all(bool k) { p_->keep_all = k; p_->planned = false; }

bool Executor::run(const std::vector<InputBinding>& inputs, std::string* err) {
  try {
    Impl& s = *p_;
    std::vector<Shape> shapes;
    for (int id : s.input_ids) {
      Impl::Value& v = s.vals[std::size_t(id)];
      const InputBinding* b = nullptr;
      for (auto& in : inputs) if (in.name == v.name) b = &in;
      if (!b || !b->data) throw GraphError("input '" + v.name + "' is not bound");
      v.t = Tensor();
      v.t.dt = b->dt;
      v.t.shape = b->shape;
      v.t.data = const_cast<void*>(b->data);
      for (const ValueInfo& vi : s.model.inputs) {
        if (vi.name != v.name) continue;
        bool ok = int(vi.dims.size()) == b->shape.rank;
        for (std::size_t d = 0; ok && d < vi.dims.size(); ++d) ok = vi.dims[d] < 0 || vi.dims[d] == b->shape[int(d)];
        if (!ok) throw GraphError("input '" + v.name + "' has shape " + b->shape.str() + " but the model expects a different one");
      }
      shapes.push_back(b->shape);
    }
    if (!s.planned || shapes.size() != s.plan_shapes.size() || !std::equal(shapes.begin(), shapes.end(), s.plan_shapes.begin())) {
      s.plan_shapes = shapes;
      s.planned = false;
      s.plan_run();
    } else {
      s.fast_run();
    }
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    p_->planned = false;
    return false;
  }
}

const Tensor* Executor::value(const std::string& name) const {
  auto it = p_->by_name.find(name);
  return it == p_->by_name.end() ? nullptr : &p_->vals[std::size_t(it->second)].t;
}
const Tensor* Executor::output(const std::string& name) const { return value(name); }
const std::vector<std::string>& Executor::input_names() const { return p_->in_names; }
const std::vector<std::string>& Executor::output_names() const { return p_->out_names; }
std::map<std::string, int> Executor::op_histogram() const {
  std::map<std::string, int> h;
  for (auto& n : p_->model.nodes) ++h[n.op_type];
  return h;
}
std::size_t Executor::arena_bytes() const { return p_->arena_size; }
std::size_t Executor::scratch_bytes() const {
  std::size_t n = 0;
  for (auto& v : p_->scratch) n += v.capacity() * sizeof(float);
  return n;
}
std::size_t Executor::weights_bytes() const { return p_->weights_size; }
std::size_t Executor::node_count() const { return p_->nodes.size(); }
std::size_t Executor::scheduled_nodes() const { return p_->schedule.size(); }

}  // namespace deckhand::stems::graph
