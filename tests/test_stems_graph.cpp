// The built-in ONNX graph executor (src/stems/graph): protobuf reader (including hostile input), every operator against
// outputs recorded once from the reference runtime (tools/make_stems_graph_fixtures.py; no Python needed here), the tiny
// test models, memory planning, and, when the Demucs weights are available on this machine, the whole htdemucs network
// and a real separation. Real-model tests skip themselves when the weights are absent (CI has none).
#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#if defined(_WIN32)
#include <process.h>
#define DH_GETPID _getpid
#else
#include <unistd.h>
#define DH_GETPID ::getpid
#endif

#include "check.hpp"
#include "deckhand/stems/demucs_dsp.hpp"
#include "deckhand/stems/manifest.hpp"
#include "deckhand/stems/model_convert.hpp"
#include "deckhand/stems/model_install.hpp"
#include "deckhand/stems/roformer_dsp.hpp"
#include "deckhand/stems/stems.hpp"
#include "internal.hpp"
#include "graph/executor.hpp"

using namespace deckhand::stems;
using namespace deckhand::stems::graph;
namespace fs = std::filesystem;

static int g_tests = 0, g_skipped = 0;
static std::vector<fs::path> g_cleanup;  // big converted-weights folders, removed when the run ends
// DH_STEMS_MODELS_DIR_OVERRIDE points the real-model tests elsewhere (an empty folder reproduces the CI case: no weights).
static fs::path models_root() { const char* e = std::getenv("DH_STEMS_MODELS_DIR_OVERRIDE"); return e ? fs::path(e) : fs::path(DH_STEMS_MODELS_DIR); }
#define TEST(name) static void name(); struct name##_reg { name##_reg() { run_list().push_back({#name, name}); } } name##_inst; static void name()
struct Case { const char* name; void (*fn)(); };
static std::vector<Case>& run_list() { static std::vector<Case> v; return v; }

static fs::path tmpdir(const char* tag) {
  static int n = 0;
  fs::path p = fs::temp_directory_path() / ("dh_graph_test_" + std::string(tag) + "_" + std::to_string(DH_GETPID()) + "_" + std::to_string(n++));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

static std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  CHECK(f);
  std::vector<std::uint8_t> v(std::size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size()));
  return v;
}

// ---- fixture container --------------------------------------------------------------------------------------------
struct NamedTensor { std::string name; Tensor t; };
struct FixtureCase {
  std::string name;
  std::vector<std::uint8_t> model;
  float rtol = 0, atol = 0;
  std::vector<NamedTensor> in, out;
};
struct Cursor {
  const std::vector<std::uint8_t>& b;
  std::size_t p = 0;
  template <class T> T get() { CHECK(p + sizeof(T) <= b.size()); T v; std::memcpy(&v, b.data() + p, sizeof v); p += sizeof v; return v; }
  std::string str() { auto n = get<std::uint32_t>(); CHECK(p + n <= b.size()); std::string s(reinterpret_cast<const char*>(b.data() + p), n); p += n; return s; }
  std::vector<NamedTensor> list() {
    std::vector<NamedTensor> v;
    auto n = get<std::uint32_t>();
    for (std::uint32_t i = 0; i < n; ++i) {
      NamedTensor nt;
      nt.name = str();
      auto dt = get<std::uint8_t>();
      auto rank = get<std::uint32_t>();
      Shape s;
      for (std::uint32_t d = 0; d < rank; ++d) s.push(get<std::int64_t>());
      nt.t = make_owned(dt == 0 ? DType::F32 : dt == 1 ? DType::I64 : DType::Bool, s);
      CHECK(p + nt.t.bytes() <= b.size());
      std::memcpy(nt.t.data, b.data() + p, nt.t.bytes());
      p += nt.t.bytes();
      v.push_back(std::move(nt));
    }
    return v;
  }
};
static std::vector<FixtureCase> load_fixtures() {
  auto b = read_file(fs::path(DH_STEMS_TEST_DIR) / "graph_ops.dhgf");
  Cursor c{b};
  CHECK(b.size() > 8 && std::memcmp(b.data(), "DHGF1\n", 6) == 0);
  c.p = 6;
  std::vector<FixtureCase> v(c.get<std::uint32_t>());
  for (auto& fc : v) {
    fc.name = c.str();
    auto n = c.get<std::uint32_t>();
    CHECK(c.p + n <= b.size());
    fc.model.assign(b.begin() + std::ptrdiff_t(c.p), b.begin() + std::ptrdiff_t(c.p + n));
    c.p += n;
    fc.rtol = c.get<float>();
    fc.atol = c.get<float>();
    fc.in = c.list();
    fc.out = c.list();
  }
  CHECK(c.p == b.size());
  return v;
}

static std::unique_ptr<Executor> open_bytes(const std::vector<std::uint8_t>& bytes, int threads, std::string* err) {
  try {
    auto buf = std::make_shared<std::vector<std::uint8_t>>(bytes);
    return Executor::from_proto(parse_model(buf), ".", "", threads, err);
  } catch (const std::exception& e) { *err = e.what(); return nullptr; }
}

static std::string compare_tensor(const Tensor& got, const Tensor& want, float rtol, float atol) {
  if (got.dt != want.dt) return "dtype differs";
  if (got.shape != want.shape) return "shape " + got.shape.str() + " vs " + want.shape.str();
  for (std::int64_t i = 0; i < want.numel(); ++i) {
    if (want.dt == DType::F32) {
      float a = got.f()[i], b = want.f()[i];
      if (!(std::fabs(a - b) <= atol + rtol * std::fabs(b))) return "element " + std::to_string(i) + ": " + std::to_string(a) + " vs " + std::to_string(b);
    } else if (want.dt == DType::I64) {
      if (got.i()[i] != want.i()[i]) return "element " + std::to_string(i) + ": " + std::to_string(got.i()[i]) + " vs " + std::to_string(want.i()[i]);
    } else if (got.b()[i] != want.b()[i]) return "element " + std::to_string(i) + " differs";
  }
  return "";
}

static int run_fixtures(int threads, int repeats) {
  auto cases = load_fixtures();
  int n = 0;
  for (auto& fc : cases) {
    std::string err;
    auto ex = open_bytes(fc.model, threads, &err);
    if (!ex) { std::fprintf(stderr, "%s: %s\n", fc.name.c_str(), err.c_str()); CHECK(false); }
    std::vector<InputBinding> ins;
    for (auto& nt : fc.in) { InputBinding b; b.name = nt.name; b.data = nt.t.data; b.shape = nt.t.shape; b.dt = nt.t.dt; ins.push_back(b); }
    for (int rep = 0; rep < repeats; ++rep) {  // first run plans, later runs use the arena
      if (!ex->run(ins, &err)) { std::fprintf(stderr, "%s: %s\n", fc.name.c_str(), err.c_str()); CHECK(false); }
      for (auto& want : fc.out) {
        const Tensor* got = ex->output(want.name);
        CHECK(got);
        std::string d = compare_tensor(*got, want.t, fc.rtol, fc.atol);
        if (!d.empty()) { std::fprintf(stderr, "%s / %s (run %d, %d threads): %s\n", fc.name.c_str(), want.name.c_str(), rep, threads, d.c_str()); CHECK(false); }
      }
    }
    ++n;
  }
  return n;
}

TEST(operators_against_reference_single_thread) {
  int n = run_fixtures(1, 2);
  std::printf("  %d operator cases (2 runs each)\n", n);
  CHECK(n >= 85);
}
TEST(operators_against_reference_threaded) { run_fixtures(4, 2); }
TEST(convolutions_are_exact_across_tile_borders) {
  set_conv_tiling_for_test(1, 3);  // three output columns per tile: every border case in the fixtures is hit
  run_fixtures(1, 1);
  run_fixtures(3, 1);
  set_conv_tiling_for_test(4 << 20, 256);
}

// ---- protobuf reader ---------------------------------------------------------------------------------------------------
static void put_varint(std::string& s, std::uint64_t v) { while (v >= 0x80) { s += char(v | 0x80); v >>= 7; } s += char(v); }
static void put_field(std::string& s, int field, const std::string& payload) { put_varint(s, std::uint64_t(field) << 3 | 2); put_varint(s, payload.size()); s += payload; }
static void put_int(std::string& s, int field, std::uint64_t v) { put_varint(s, std::uint64_t(field) << 3); put_varint(s, v); }
static std::shared_ptr<std::vector<std::uint8_t>> bytes_of(const std::string& s) { return std::make_shared<std::vector<std::uint8_t>>(s.begin(), s.end()); }

static std::string tiny_model(const std::string& op, int n_outputs = 1) {
  std::string node, vin, vout, tt, shape, dim, ty, graph, model, opset;
  node.clear();
  put_field(node, 1, "x");
  for (int i = 0; i < n_outputs; ++i) put_field(node, 2, i ? "y" + std::to_string(i) : "y");
  put_field(node, 3, "n0");
  put_field(node, 4, op);
  put_field(graph, 1, node);
  put_int(dim, 1, 2);
  put_field(shape, 1, dim);
  put_int(tt, 1, 1);
  put_field(tt, 2, shape);
  put_field(ty, 1, tt);
  put_field(vin, 1, "x");
  put_field(vin, 2, ty);
  put_field(graph, 11, vin);
  put_field(vout, 1, "y");
  put_field(graph, 12, vout);
  put_int(model, 1, 8);
  put_field(model, 7, graph);
  put_int(opset, 2, 17);
  put_field(model, 8, opset);
  return model;
}

TEST(unsupported_op_is_a_clear_error) {
  std::string err;
  auto ex = Executor::from_proto(parse_model(bytes_of(tiny_model("FooBar"))), ".", "", 1, &err);
  CHECK(!ex);
  std::printf("  %s\n", err.c_str());
  CHECK(err.find("unsupported op FooBar at node 0") != std::string::npos);
  ex = Executor::from_proto(parse_model(bytes_of(tiny_model("Relu"))), ".", "", 1, &err);
  CHECK(ex);
  float in[2] = {-1.f, 2.f};
  InputBinding b; b.name = "x"; b.data = in; b.shape = Shape{2};
  CHECK(ex->run({b}, &err));
  CHECK(ex->output("y")->f()[0] == 0.f && ex->output("y")->f()[1] == 2.f);
  // wrong input shape and missing input are reported, not computed
  b.shape = Shape{3};
  CHECK(!ex->run({b}, &err) && err.find("expects") != std::string::npos);
  CHECK(!ex->run({}, &err) && err.find("not bound") != std::string::npos);
}

TEST(reader_rejects_hostile_input) {
  auto fails = [](const std::string& s) { try { parse_model(bytes_of(s)); return false; } catch (const GraphError&) { return true; } };
  CHECK(fails(""));                                         // no graph
  CHECK(fails(std::string(11, char(0xff))));                // varint longer than 10 bytes
  CHECK(fails(std::string("\x3a\xff\xff\xff\x7f", 5)));     // length past the end of the buffer
  CHECK(fails(std::string("\x0b", 1)));                     // start-group wire type
  CHECK(fails(std::string("\x00", 1)));                     // field number 0
  // initializer with absurd dimensions
  std::string t, g, m;
  put_int(t, 1, std::uint64_t(1) << 40); put_int(t, 1, std::uint64_t(1) << 40); put_int(t, 2, 1);
  put_field(t, 9, "abcd");
  put_field(g, 5, t);
  put_field(m, 7, g);
  auto pm = parse_model(bytes_of(m));
  bool threw = false;
  try { decode_tensor(pm.initializers[0], nullptr, 0); } catch (const GraphError&) { threw = true; }
  CHECK(threw);
  // negative / over-rank dimension lists
  std::string t2, g2, m2;
  for (int i = 0; i < 12; ++i) put_int(t2, 1, 1);
  put_field(g2, 5, t2); put_field(m2, 7, g2);
  CHECK(fails(m2));
  // external data that points outside the file, and one that tries to leave the folder
  fs::path d = tmpdir("ext");
  { std::ofstream(d / "w.bin", std::ios::binary) << std::string(16, 'x'); }
  auto ext = [&](const std::string& loc, int off, int len) {
    std::string t3, e1, e2, e3, g3, m3, vo;
    put_int(t3, 1, 4); put_int(t3, 2, 1); put_field(t3, 8, "w");
    put_field(e1, 1, "location"); put_field(e1, 2, loc); put_field(t3, 13, e1);
    put_field(e2, 1, "offset"); put_field(e2, 2, std::to_string(off)); put_field(t3, 13, e2);
    put_field(e3, 1, "length"); put_field(e3, 2, std::to_string(len)); put_field(t3, 13, e3);
    put_int(t3, 14, 1);
    put_field(g3, 5, t3);
    put_field(vo, 1, "w"); put_field(g3, 12, vo);
    put_field(m3, 7, g3);
    std::string err;
    auto ex = Executor::from_proto(parse_model(bytes_of(m3)), d.string(), "", 1, &err);
    return std::make_pair(bool(ex), err);
  };
  CHECK(ext("w.bin", 0, 16).first);
  CHECK(!ext("w.bin", 8, 16).first);               // runs past the end
  CHECK(!ext("../w.bin", 0, 16).first);            // path traversal
  CHECK(!ext("/etc/passwd", 0, 16).first);         // absolute path
  CHECK(!ext("w.bin", 0, 8).first);                // length disagrees with the shape
  fs::remove_all(d);
}

TEST(reader_survives_truncation_and_corruption_of_a_real_model) {
  auto bytes = read_file(fs::path(DH_STEMS_SKELETON_DIR) / "htdemucs.onnx");
  auto try_parse = [&](std::vector<std::uint8_t> v) {
    try { parse_model(std::make_shared<std::vector<std::uint8_t>>(std::move(v))); return 1; }
    catch (const GraphError&) { return 0; }
  };
  CHECK(try_parse(bytes) == 1);
  int rejected = 0;
  for (int i = 1; i <= 120; ++i) {
    std::size_t cut = bytes.size() * std::size_t(i) / 121 + std::size_t(i) * 7;
    rejected += 1 - try_parse(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + std::ptrdiff_t(cut)));
  }
  std::mt19937 g(77);
  for (int i = 0; i < 200; ++i) {
    auto v = bytes;
    for (int k = 0; k < 1 + i % 4; ++k) v[g() % v.size()] = std::uint8_t(g());
    try_parse(std::move(v));
  }
  std::printf("  %d of 120 truncations rejected, 200 corruptions survived\n", rejected);
  CHECK(rejected > 90);
}

TEST(skeleton_operator_inventory) {
  auto m = load_model_file(std::string(DH_STEMS_SKELETON_DIR) + "/htdemucs.onnx");
  CHECK(m.nodes.size() == 4483 && m.initializers.size() == 533 && m.inputs.size() == 2 && m.outputs.size() == 2);
  std::map<std::string, int> h;
  for (auto& n : m.nodes) ++h[n.op_type];
  std::string line;
  for (auto& kv : h) {
    CHECK(find_op(kv.first));  // every operator of the committed skeleton is implemented
    line += kv.first + ":" + std::to_string(kv.second) + " ";
  }
  std::printf("  %s\n", line.c_str());
  CHECK(h["Conv"] == 92 && h["ConvTranspose"] == 8 && h["MatMul"] == 54);
}

// ---- kernels --------------------------------------------------------------------------------------------------------------
TEST(sgemm_matches_portable_reference_for_all_transposes) {
  std::mt19937 g(5);
  std::normal_distribution<float> nd;
  const int sizes[][3] = {{1, 1, 1}, {3, 5, 7}, {17, 33, 9}, {64, 31, 130}, {5, 200, 3}};
  for (auto& s : sizes)
    for (int ta = 0; ta < 2; ++ta)
      for (int tb = 0; tb < 2; ++tb) {
        int M = s[0], N = s[1], K = s[2];
        std::vector<float> A(std::size_t(M * K)), B(std::size_t(K * N)), C1(std::size_t(M * N)), C2;
        for (auto& v : A) v = nd(g);
        for (auto& v : B) v = nd(g);
        for (auto& v : C1) v = nd(g);
        C2 = C1;
        int lda = ta ? M : K, ldb = tb ? K : N;
        sgemm(ta, tb, M, N, K, 0.7f, A.data(), lda, B.data(), ldb, 0.5f, C1.data(), N);
        // naive double-precision reference
        for (int i = 0; i < M; ++i)
          for (int j = 0; j < N; ++j) {
            double acc = 0;
            for (int k = 0; k < K; ++k) acc += double(ta ? A[std::size_t(k * lda + i)] : A[std::size_t(i * lda + k)]) * (tb ? B[std::size_t(j * ldb + k)] : B[std::size_t(k * ldb + j)]);
            double want = 0.7 * acc + 0.5 * C2[std::size_t(i * N + j)];
            CHECK(std::fabs(double(C1[std::size_t(i * N + j)]) - want) < 2e-4 * (1 + std::fabs(want)));
          }
        std::vector<float> C3 = C2;
        sgemm_portable(ta, tb, M, N, K, 0.7f, A.data(), lda, B.data(), ldb, 0.5f, C3.data(), N);
        for (std::size_t i = 0; i < C3.size(); ++i) CHECK(std::fabs(C3[i] - C1[i]) < 2e-4 * (1 + std::fabs(C1[i])));
      }
  std::printf("  kernel backend: %s\n", kernel_backend_name());
}

TEST(thread_pool_covers_every_index_once) {
  for (int threads : {1, 2, 5}) {
    ThreadPool p(threads);
    for (int round = 0; round < 50; ++round) {
      std::vector<std::atomic<int>> hit(1000);
      p.run(1000, 7, [&](std::int64_t b, std::int64_t e) { for (auto i = b; i < e; ++i) hit[std::size_t(i)]++; });
      for (auto& h : hit) CHECK(h.load() == 1);
      // nested use runs inline instead of deadlocking
      p.run(10, 1, [&](std::int64_t, std::int64_t) { p.run(10, 1, [](std::int64_t, std::int64_t) {}); });
    }
  }
}

// ---- tiny test models through the executor --------------------------------------------------------------------------------
TEST(tiny_test_models_run_through_the_executor) {
  const char* files[] = {"gain4_bsct.onnx", "gain4_btcs.onnx", "gain4_multi.onnx"};
  for (auto f : files) {
    std::string err;
    auto ex = Executor::open((fs::path(DH_STEMS_TEST_DIR) / f).string(), "", 2, &err);
    if (!ex) std::fprintf(stderr, "%s: %s\n", f, err.c_str());
    CHECK(ex);
    std::vector<float> in(2 * 4096);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = std::sin(float(i) * 0.01f);
    InputBinding b; b.name = "mix"; b.data = in.data(); b.shape = std::string(f) == "gain4_bsct.onnx" ? Shape{1, 4096, 2} : Shape{1, 2, 4096};
    for (int rep = 0; rep < 2; ++rep) {
      if (!ex->run({b}, &err)) std::fprintf(stderr, "%s: %s\n", f, err.c_str());
      CHECK(ex->run({b}, &err));
      for (auto& on : ex->output_names()) {
        const Tensor* t = ex->output(on);
        CHECK(t && t->numel() % in.size() == 0);
        double s = 0;
        for (std::int64_t i = 0; i < t->numel(); ++i) s += std::fabs(t->f()[i]);
        CHECK(s > 0);
      }
    }
  }
}

TEST(plan_is_reused_and_replanned_when_the_input_shape_changes) {
  // Relu -> Reshape view chain: later runs must not allocate more, and a new input size must work
  auto cases = load_fixtures();
  const FixtureCase* fc = nullptr;
  for (auto& c : cases) if (c.name == "shape_chain") fc = &c;
  CHECK(fc);
  std::string err;
  auto ex = open_bytes(fc->model, 1, &err);
  CHECK(ex);
  InputBinding b; b.name = "x"; b.data = fc->in[0].t.data; b.shape = fc->in[0].t.shape;
  CHECK(ex->run({b}, &err));
  std::size_t arena = ex->arena_bytes(), sched = ex->scheduled_nodes();
  for (int i = 0; i < 3; ++i) CHECK(ex->run({b}, &err) && ex->arena_bytes() == arena && ex->scheduled_nodes() == sched);
  CHECK(sched < ex->node_count());  // shape arithmetic was folded away
}

// ---- the real Demucs network (needs the weights) ---------------------------------------------------------------------------------
static std::vector<float> synth_mix(std::size_t n) {
  std::vector<float> m(2 * n);
  std::uint32_t s = 12345;
  for (std::size_t i = 0; i < n; ++i) {
    s = s * 1664525u + 1013904223u;
    float noise = (float(s >> 8) / float(1 << 24) - 0.5f) * 0.2f;
    float a = 0.25f * std::sin(2.f * 3.14159265f * 110.f * float(i) / 44100.f) + 0.15f * std::sin(2.f * 3.14159265f * 1760.f * float(i) / 44100.f);
    m[i] = a + noise;
    m[n + i] = 0.8f * a - noise * 0.7f + 0.05f * std::sin(2.f * 3.14159265f * 440.f * float(i) / 44100.f);
  }
  return m;
}

// Voice-like test signal: a vibrato tone with vowel formants (700 / 1200 Hz) over a plucked low harmonic bass and noise-burst drums.
// Deterministic; the vocal model finds real vocal content in it, unlike in the pure sines of synth_mix().
static std::vector<float> synth_vocalish(std::size_t n) {
  std::vector<float> m(2 * n);
  std::uint32_t s = 777;
  double ph = 0;
  const double pi = 3.14159265358979323846, notes[4] = {55.0, 65.4, 73.4, 82.4};
  for (std::size_t i = 0; i < n; ++i) {
    const double t = double(i) / 44100.0;
    ph += 2 * pi * 220.0 * (1 + 0.01 * std::sin(2 * pi * 5 * t)) / 44100.0;
    double voice = 0;
    for (int h = 1; h < 30; ++h) {
      double fr = h * 220.0, g = std::exp(-std::pow((fr - 700) / 150, 2)) + 0.6 * std::exp(-std::pow((fr - 1200) / 200, 2));
      voice += g * std::sin(h * ph) / h;
    }
    double tn = std::fmod(t, 0.5), f0 = notes[int(t / 0.5) / 2 % 4], bass = 0;
    for (int h = 1; h < 9; ++h) bass += std::sin(2 * pi * f0 * h * t) / h;
    bass *= std::exp(-tn * 3) * 0.25;
    s = s * 1664525u + 1013904223u;
    double noise = (double(s >> 8) / double(1 << 24) - 0.5) * 2.0 * std::exp(-tn / 0.025) * 0.25;
    s = s * 1664525u + 1013904223u;
    double floor_noise = (double(s >> 8) / double(1 << 24) - 0.5) * 0.01;  // -50 dB noise floor: no digital silence in any band (the network normalises per band)
    m[i] = float(0.35 * voice * 0.5 + bass + noise + floor_noise);
    s = s * 1664525u + 1013904223u;
    m[n + i] = float(0.35 * voice * 0.45 + bass + noise * 0.9 + (double(s >> 8) / double(1 << 24) - 0.5) * 0.01);
  }
  return m;
}

// Path of a converted htdemucs.weights, or "" when the checkpoint is not on this machine. DH_STEMS_WEIGHTS overrides.
static std::string htdemucs_weights() {
  static std::string cached;
  static bool done = false;
  if (done) return cached;
  done = true;
  if (const char* e = std::getenv("DH_STEMS_WEIGHTS")) { if (fs::exists(e)) cached = e; return cached; }
  fs::path ck = models_root() / "demucs" / "955717e8-8726e21a.th";
  if (!fs::exists(ck)) return cached;
  fs::path d = tmpdir("weights");
  g_cleanup.push_back(d);
  std::string err;
  if (!convert_checkpoint(ck, fs::path(DH_STEMS_SKELETON_DIR) / "htdemucs.layout.json", d / "htdemucs.weights", nullptr, &err)) {
    std::fprintf(stderr, "convert: %s\n", err.c_str());
    return cached;
  }
  cached = (d / "htdemucs.weights").string();
  return cached;
}

static std::string ft_weights(int i) {
  static const char* const ck[4] = {"f7e0c4bc-ba3fe64a", "d12395a8-e57c48e6", "92cfc3b6-ef3bcb9c", "04573f0d-f3cf25b2"};
  if (const char* d = std::getenv("DH_STEMS_FT_DIR")) {
    fs::path p = fs::path(d) / ("w" + std::to_string(i) + ".weights");
    return fs::exists(p) ? p.string() : "";
  }
  static std::string cache[4];
  if (!cache[i].empty()) return cache[i];
  fs::path c = models_root() / "demucs" / (std::string(ck[i]) + ".th");
  if (!fs::exists(c)) return "";
  fs::path d = tmpdir("ftw");
  g_cleanup.push_back(d);
  std::string err;
  if (!convert_checkpoint(c, fs::path(DH_STEMS_SKELETON_DIR) / "htdemucs.layout.json", d / "w.weights", nullptr, &err)) return "";
  return cache[i] = (d / "w.weights").string();
}

// Runs the skeleton with `weights` on the synthetic mix and compares with the sampled reference values in `fixture`.
static std::unique_ptr<Executor> check_core(const std::string& weights, const char* fixture, const char* label, bool dump) {
  std::string err;
  auto ex = Executor::open(std::string(DH_STEMS_SKELETON_DIR) + "/htdemucs.onnx", weights, 4, &err);
  if (!ex) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(ex);
  const std::size_t S = 343980, T = DemucsDsp::frames_for(S), Fq = DemucsDsp::kBins;
  auto mix = synth_mix(S);
  std::vector<float> spec;
  DemucsDsp::analyse(mix.data(), S, spec);
  InputBinding bs, bm;
  bs.name = "spec"; bs.data = spec.data(); bs.shape = Shape{1, 4, std::int64_t(Fq), std::int64_t(T)};
  bm.name = "mix"; bm.data = mix.data(); bm.shape = Shape{1, 2, std::int64_t(S)};
  auto t0 = std::chrono::steady_clock::now();
  CHECK(ex->run({bs, bm}, &err));
  double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  CHECK(ex->run({bs, bm}, &err));
  double second = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const Tensor* xs = ex->output("xspec");
  const Tensor* xt = ex->output("xtime");
  CHECK(xs && xt && xs->numel() == std::int64_t(4 * 4 * Fq * T) && xt->numel() == std::int64_t(4 * 2 * S));
  std::printf("  %s core run: first %.2f s (planning), second %.2f s; %zu of %zu nodes scheduled; arena %.0f MB, scratch %.0f MB, weights %.0f MB\n", label, first,
              second, ex->scheduled_nodes(), ex->node_count(), double(ex->arena_bytes()) / 1e6, double(ex->scratch_bytes()) / 1e6, double(ex->weights_bytes()) / 1e6);
  if (const char* dir = dump ? std::getenv("DH_STEMS_DUMP_DIR") : nullptr) {  // used by tools/stems_graph_compare.py
    auto dumpf = [&](const char* n, const void* p, std::size_t bytes) { std::ofstream(fs::path(dir) / n, std::ios::binary).write(static_cast<const char*>(p), std::streamsize(bytes)); };
    dumpf("spec.bin", spec.data(), spec.size() * 4); dumpf("mix.bin", mix.data(), mix.size() * 4);
    dumpf("xspec.bin", xs->data, xs->bytes()); dumpf("xtime.bin", xt->data, xt->bytes());
  }
  auto b = read_file(fs::path(DH_STEMS_TEST_DIR) / fixture);
  CHECK(b.size() > 32 && std::memcmp(b.data(), "DHCR1\n", 6) == 0);
  Cursor c{b};
  c.p = 6;
  auto n = c.get<std::uint32_t>();
  float xs_rms = c.get<float>(), xs_max = c.get<float>(), xt_rms = c.get<float>(), xt_max = c.get<float>();
  auto check_one = [&](const Tensor* t, float rms_ref, float max_ref, const char* what) {
    double e = 0, mx = 0;
    for (std::int64_t i = 0; i < t->numel(); ++i) { e += double(t->f()[i]) * t->f()[i]; mx = std::max(mx, double(std::fabs(t->f()[i]))); }
    double rms = std::sqrt(e / double(t->numel()));
    std::int64_t N = t->numel();
    double worst = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
      std::int64_t idx = std::int64_t((std::uint64_t(k) * 2654435761ull + 12345ull) % std::uint64_t(N));
      double want = c.get<float>();
      worst = std::max(worst, std::fabs(double(t->f()[idx]) - want));
    }
    std::printf("  %s: rms %.6f (ref %.6f), peak %.4f (ref %.4f), worst sampled abs error %.2e\n", what, rms, rms_ref, mx, max_ref, worst);
    CHECK(std::fabs(rms - rms_ref) < 1e-3 * rms_ref + 1e-7);
    CHECK(std::fabs(mx - max_ref) < 2e-3 * max_ref + 1e-6);
    CHECK(worst < 2e-3 * max_ref + 1e-6);
  };
  check_one(xs, xs_rms, xs_max, "xspec");
  check_one(xt, xt_rms, xt_max, "xtime");
  return ex;
}

TEST(htdemucs_core_matches_reference) {
  std::string w = htdemucs_weights();
  if (w.empty()) { std::printf("  (no htdemucs weights here: skipped)\n"); ++g_skipped; return; }
  check_core(w, "htdemucs_core_ref.bin", "htdemucs", true);
}

TEST(htdemucs_ft_each_model_matches_reference) {
  for (int i = 0; i < 4; ++i) {
    std::string w = ft_weights(i);
    if (w.empty()) { std::printf("  (no htdemucs_ft weights here: skipped)\n"); ++g_skipped; return; }
    std::string fx = "htdemucs_ft" + std::to_string(i) + "_core_ref.bin";
    check_core(w, fx.c_str(), ("ft model " + std::to_string(i)).c_str(), false);
  }
}

// The bag runner built through the normal manifest path: stem k must come from model k alone.
TEST(htdemucs_ft_bag_takes_each_stem_from_its_own_model) {
  std::string w[4];
  for (int i = 0; i < 4; ++i) { w[i] = ft_weights(i); if (w[i].empty()) { std::printf("  (no htdemucs_ft weights here: skipped)\n"); ++g_skipped; return; } }
  fs::path d = tmpdir("bag");
  fs::create_directories(d / "bag");
  fs::copy_file(fs::path(DH_STEMS_SKELETON_DIR) / "htdemucs.onnx", d / "bag" / "htdemucs.onnx");
  const char* names[4] = {"drums", "bass", "other", "vocals"};
  for (int i = 0; i < 4; ++i) {  // symlinks need privileges on Windows: fall back to copying
    fs::path dst = d / "bag" / (std::string("htdemucs.") + names[i] + ".weights");
    std::error_code ec;
    fs::create_symlink(fs::absolute(w[i]), dst, ec);
    if (ec) fs::copy_file(w[i], dst);
  }
  detail::Sha256 h;
  auto onnx = read_file(d / "bag" / "htdemucs.onnx");
  h.update(onnx.data(), onnx.size());
  { std::ofstream(d / "bag.dhstems.json") << "{\"format\":\"deckhand-stems-model/1\",\"id\":\"bag-test\",\"file\":\"bag/htdemucs.onnx\",\"licence\":\"test\",\"source_url\":\"test\",\"sha256\":\"" << h.finish_hex() << "\",\"sample_rate\":44100,\"segment_frames\":343980,\"channels\":2,\"pipeline\":\"htdemucs-ft-core\",\"stems\":[\"drums\",\"bass\",\"other\",\"vocals\"]}"; }
  auto models = list_models(d);
  CHECK(models.size() == 1 && models[0].pipeline == "htdemucs-ft-core");
  std::string err;
  auto bag = open_model(models[0], 4, &err);
  if (!bag) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(bag);
  const std::size_t S = 343980;
  auto mix = synth_mix(S);
  std::vector<float> out(std::size_t(kStemCount) * 2 * S);
  CHECK(bag->run(mix.data(), out.data(), &err));
  CHECK(bag->run(mix.data(), out.data(), &err));  // steady state with the shared arena
  // independent composition: model k alone, stem k only (network order drums,bass,other,vocals -> canonical)
  const int canon[4] = {Drums, Bass, Other, Vocals};
  std::vector<float> spec, y;
  DemucsDsp::analyse(mix.data(), S, spec);
  for (int k = 0; k < 4; ++k) {
    auto ex = Executor::open(std::string(DH_STEMS_SKELETON_DIR) + "/htdemucs.onnx", w[k], 4, &err);
    CHECK(ex);
    InputBinding bs, bm;
    bs.name = "spec"; bs.data = spec.data(); bs.shape = Shape{1, 4, std::int64_t(DemucsDsp::kBins), std::int64_t(DemucsDsp::frames_for(S))};
    bm.name = "mix"; bm.data = mix.data(); bm.shape = Shape{1, 2, std::int64_t(S)};
    CHECK(ex->run({bs, bm}, &err));
    DemucsDsp::synthesise(ex->output("xspec")->f(), kStemCount, S, y);
    const float* xt = ex->output("xtime")->f();
    double worst = 0, peak = 0;
    for (std::size_t i = 0; i < 2 * S; ++i) {
      float want = y[std::size_t(k) * 2 * S + i] + xt[std::size_t(k) * 2 * S + i];
      worst = std::max(worst, double(std::fabs(out[std::size_t(canon[k]) * 2 * S + i] - want)));
      peak = std::max(peak, double(std::fabs(want)));
    }
    std::printf("  bag stem %s from model %d: max |diff| %.2e (peak %.3f)\n", stem_name(canon[k]), k, worst, peak);
    CHECK(worst < 1e-4 && peak > 1e-4);  // the synthetic mix has little vocals or drums, so the peaks differ widely
  }
  fs::remove_all(d);
}

// The installer's converter for the four-checkpoint htdemucs_ft entry (needs the four checkpoints from third_party/models).
TEST(installer_converts_the_ft_bag) {
  const char* ck[4] = {"f7e0c4bc-ba3fe64a", "d12395a8-e57c48e6", "92cfc3b6-ef3bcb9c", "04573f0d-f3cf25b2"};
  for (auto c : ck)
    if (!fs::exists(models_root() / "demucs" / (std::string(c) + ".th"))) { std::printf("  (no htdemucs_ft checkpoints here: skipped)\n"); ++g_skipped; return; }
  fs::path res = fs::path(DH_STEMS_SKELETON_DIR).parent_path();
  CatalogueEntry entry;
  bool found = false;
  for (auto& e : list_catalogue(res)) if (e.id == "demucs-htdemucs-ft") { entry = e; found = true; }
  CHECK(found && entry.install_supported && entry.files.size() == 4);
  fs::path data = tmpdir("ftinst"), dir = models_dir(data) / entry.id;
  fs::create_directories(dir);
  for (auto& f : entry.files) {  // stand in for the downloads (the converter deletes them afterwards)
    fs::path src = fs::absolute(models_root() / "demucs" / f.name), dst = dir / f.name;
    std::error_code ec;
    fs::create_symlink(src, dst, ec);
    if (ec) fs::copy_file(src, dst);
  }
  std::string err;
  CHECK(demucs_converter(data, res / "skeletons")(dir, entry, &err));
  auto models = list_models(models_dir(data));
  CHECK(models.size() == 1 && models[0].pipeline == "htdemucs-ft-core" && models[0].id == "demucs-htdemucs-ft");
  CHECK(verify_model_file(models[0], &err));
  for (auto n : {"drums", "bass", "other", "vocals"}) CHECK(fs::file_size(dir / (std::string("htdemucs.") + n + ".weights")) == 167939968);
  for (auto& f : entry.files) CHECK(!fs::exists(dir / f.name));
  CHECK(fs::exists(models_root() / "demucs" / entry.files[0].name));  // the user's own files are untouched
  fs::remove_all(data);
}


// ---- Mel-Band RoFormer ------------------------------------------------------------------------------------------------------
// An installed copy of the model (skeleton + weights converted from the checkpoint by the installer's converter), or "" when the
// checkpoint is not on this machine. DH_STEMS_ROFORMER_DATA names an already prepared data folder instead.
static fs::path roformer_data() {
  static fs::path cached;
  static bool done = false;
  if (done) return cached;
  done = true;
  if (const char* e = std::getenv("DH_STEMS_ROFORMER_DATA")) { if (fs::exists(e)) cached = e; return cached; }
  fs::path ck = models_root() / "roformer" / "MelBandRoformer.ckpt";
  if (!fs::exists(ck)) return cached;
  fs::path res = fs::path(DH_STEMS_SKELETON_DIR).parent_path();
  CatalogueEntry entry;
  bool found = false;
  for (auto& e : list_catalogue(res)) if (e.id == "melband-roformer-vocals") { entry = e; found = true; }
  CHECK(found && entry.install_supported && entry.files.size() == 1);
  fs::path data = tmpdir("rfdata"), dir = models_dir(data) / entry.id;
  g_cleanup.push_back(data);
  fs::create_directories(dir);
  std::error_code ec;
  fs::create_symlink(fs::absolute(ck), dir / entry.files[0].name, ec);
  if (ec) fs::copy_file(ck, dir / entry.files[0].name);
  std::string err;
  if (!demucs_converter(data, res / "skeletons")(dir, entry, &err)) { std::fprintf(stderr, "roformer convert: %s\n", err.c_str()); return cached; }
  CHECK(fs::exists(fs::absolute(ck)));  // the user's checkpoint is untouched
  return cached = data;
}

struct RoformerRun { std::vector<float> x, masks, vocals; };
// The pipeline by hand (STFT -> network -> masks -> inverse STFT), so the network's own output can be inspected.
static RoformerRun roformer_by_hand(const fs::path& data, const std::vector<float>& mix, double* net_seconds) {
  fs::path dir = models_dir(data) / "melband-roformer-vocals";
  std::string err;
  auto ex = Executor::open((dir / "roformer.onnx").string(), "", 4, &err);
  if (!ex) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(ex);
  auto lay = read_file(dir / "roformer.layout.json");
  detail::Json j;
  CHECK(detail::parse_json(std::string(lay.begin(), lay.end()), j, &err));
  std::vector<std::pair<int, int>> bands;
  for (auto& e : j.get("bands")->a) bands.push_back({int(e.a[0].n), int(e.a[1].n)});
  RoformerDsp dsp;
  CHECK(dsp.set_bands(bands, &err));
  const std::size_t S = mix.size() / 2, T = RoformerDsp::frames_for(S);
  RoformerRun r;
  std::vector<float> stft;
  dsp.analyse(mix.data(), S, stft, r.x);
  InputBinding b;
  b.name = "x"; b.data = r.x.data(); b.shape = Shape{1, std::int64_t(T), std::int64_t(dsp.features)};
  auto t0 = std::chrono::steady_clock::now();
  if (!ex->run({b}, &err)) std::fprintf(stderr, "roformer run: %s\n", err.c_str());
  CHECK(err.empty() || ex->output("masks"));
  *net_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  {  // steady state: the second run uses the planned arena; DH_STEMS_PROFILE=1 prints where the time goes
    const bool prof = std::getenv("DH_STEMS_PROFILE") != nullptr;
    ex->set_profiling(prof);
    t0 = std::chrono::steady_clock::now();
    CHECK(ex->run({b}, &err));
    double second = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  roformer network: first run %.2f s (with planning), second %.2f s; arena %.0f MB, scratch %.0f MB, weights %.0f MB\n", *net_seconds, second,
                double(ex->arena_bytes()) / 1e6, double(ex->scratch_bytes()) / 1e6, double(ex->weights_bytes()) / 1e6);
    if (prof) {
      std::vector<std::pair<double, std::string>> v;
      for (auto& kv : ex->profile()) v.push_back({kv.second, kv.first});
      std::sort(v.rbegin(), v.rend());
      for (std::size_t i = 0; i < v.size() && i < 8; ++i) std::printf("    %-12s %.2f s\n", v[i].second.c_str(), v[i].first);
    }
  }
  const Tensor* m = ex->output("masks");
  CHECK(m && m->numel() == std::int64_t(T * dsp.features));
  r.masks.assign(m->f(), m->f() + m->numel());
  dsp.synthesise(stft, r.masks.data(), S, r.vocals);
  CHECK(r.vocals.size() == 2 * S);
  if (const char* d = std::getenv("DH_STEMS_DUMP_DIR")) {  // for tools/stems_roformer_compare.py
    auto dumpf = [&](const char* n, const void* p, std::size_t bytes) { std::ofstream(fs::path(d) / n, std::ios::binary).write(static_cast<const char*>(p), std::streamsize(bytes)); };
    dumpf("rf_mix.bin", mix.data(), mix.size() * 4); dumpf("rf_x.bin", r.x.data(), r.x.size() * 4);
    dumpf("rf_masks.bin", r.masks.data(), r.masks.size() * 4); dumpf("rf_vocals.bin", r.vocals.data(), r.vocals.size() * 4);
  }
  return r;
}

TEST(roformer_matches_reference) {
  fs::path data = roformer_data();
  if (data.empty()) { std::printf("  (no RoFormer checkpoint here: skipped)\n"); ++g_skipped; return; }
  const std::size_t S = 343980;
  auto mix = synth_vocalish(S);
  double secs = 0;
  RoformerRun r = roformer_by_hand(data, mix, &secs);
  std::printf("  roformer network run (first, with planning): %.2f s\n", secs);
  auto b = read_file(fs::path(DH_STEMS_TEST_DIR) / "roformer_ref.bin");
  CHECK(b.size() > 32 && std::memcmp(b.data(), "DHRF1\n", 6) == 0);
  Cursor c{b};
  c.p = 6;
  auto n = c.get<std::uint32_t>();
  float m_rms = c.get<float>(), m_max = c.get<float>(), v_rms = c.get<float>(), v_max = c.get<float>();
  auto check_one = [&](const std::vector<float>& t, float rms_ref, float max_ref, const char* what) {
    double e = 0, mx = 0;
    for (float v : t) { e += double(v) * v; mx = std::max(mx, double(std::fabs(v))); }
    double rms = std::sqrt(e / double(t.size())), worst = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
      std::size_t idx = std::size_t((std::uint64_t(k) * 2654435761ull + 12345ull) % std::uint64_t(t.size()));
      worst = std::max(worst, std::fabs(double(t[idx]) - double(c.get<float>())));
    }
    std::printf("  %s: rms %.6f (ref %.6f), peak %.4f (ref %.4f), worst sampled abs error %.2e\n", what, rms, rms_ref, mx, max_ref, worst);
    CHECK(std::fabs(rms - rms_ref) < 2e-3 * rms_ref + 1e-7);
    CHECK(std::fabs(mx - max_ref) < 5e-3 * max_ref + 1e-6);
    CHECK(worst < 5e-3 * max_ref + 1e-6);
  };
  check_one(r.masks, m_rms, m_max, "masks");
  check_one(r.vocals, v_rms, v_max, "vocals");
}

// Through the manifest: the vocal slot is the network's vocals, drums and bass are silent, other is exactly mix - vocals.
TEST(roformer_model_through_the_manifest) {
  fs::path data = roformer_data();
  if (data.empty()) { std::printf("  (no RoFormer checkpoint here: skipped)\n"); ++g_skipped; return; }
  auto models = list_models(models_dir(data));
  const ModelManifest* mm = nullptr;
  for (auto& m : models) if (m.id == "melband-roformer-vocals") mm = &m;
  CHECK(mm && mm->pipeline == "roformer-vocals");
  std::string err;
  CHECK(verify_model_file(*mm, &err));
  auto model = open_model(*mm, 4, &err);
  if (!model) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(model);
  const std::size_t S = 343980;
  auto mix = synth_vocalish(S);
  std::vector<float> out(std::size_t(kStemCount) * 2 * S);
  auto t0 = std::chrono::steady_clock::now();
  CHECK(model->run(mix.data(), out.data(), &err));
  double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  CHECK(model->run(mix.data(), out.data(), &err));
  double second = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  roformer model run: first %.2f s, second %.2f s (7.8 s chunk)\n", first, second);
  double vmax = 0, d_sum = 0;
  for (std::size_t i = 0; i < 2 * S; ++i) {
    vmax = std::max(vmax, double(std::fabs(out[std::size_t(Vocals) * 2 * S + i])));
    CHECK(out[std::size_t(Drums) * 2 * S + i] == 0.f && out[std::size_t(Bass) * 2 * S + i] == 0.f);
    d_sum = std::max(d_sum, double(std::fabs(out[std::size_t(Vocals) * 2 * S + i] + out[std::size_t(Other) * 2 * S + i] - mix[i])));
  }
  std::printf("  vocals peak %.3f, vocals + other - mix: max %.2e\n", vmax, d_sum);
  CHECK(vmax > 1e-4 && d_sum < 1e-6);
}


// ---- portable kernel versus the system BLAS, at scale -----------------------------------------------------------------------
TEST(portable_sgemm_matches_the_system_blas_at_scale) {
  struct Sz { int M, N, K; };
  const Sz sizes[] = {{1000, 700, 300}, {257, 1025, 129}, {5, 2049, 7}, {64, 64, 1024}, {333, 17, 515}};
  std::mt19937 g(11);
  std::normal_distribution<float> nd;
  for (auto z : sizes)
    for (int ta = 0; ta < 2; ++ta)
      for (int tb = 0; tb < 2; ++tb) {
        std::vector<float> A(std::size_t(z.M) * z.K), B(std::size_t(z.K) * z.N), C0(std::size_t(z.M) * z.N);
        for (auto& v : A) v = nd(g);
        for (auto& v : B) v = nd(g);
        for (auto& v : C0) v = nd(g);
        auto C1 = C0, C2 = C0;
        const int lda = ta ? z.M : z.K, ldb = tb ? z.K : z.N;
        sgemm(ta, tb, z.M, z.N, z.K, 0.8f, A.data(), lda, B.data(), ldb, 0.3f, C1.data(), z.N);          // system BLAS (Accelerate on macOS)
        sgemm_portable(ta, tb, z.M, z.N, z.K, 0.8f, A.data(), lda, B.data(), ldb, 0.3f, C2.data(), z.N);
        double worst = 0, peak = 0;
        for (std::size_t i = 0; i < C1.size(); ++i) { worst = std::max(worst, double(std::fabs(C1[i] - C2[i]))); peak = std::max(peak, double(std::fabs(C1[i]))); }
        CHECK(worst < 1e-5 * peak * std::sqrt(double(z.K)) + 1e-5);
      }
}

TEST(portable_kernel_speed_relative_to_the_system_blas) {  // reported, never asserted: timing must not make a test flaky
  const int n = 768;
  std::vector<float> A(std::size_t(n) * n, 0.5f), B(std::size_t(n) * n, 0.25f), C(std::size_t(n) * n);
  auto bench = [&](bool portable) {
    double best = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      if (portable) sgemm_portable(false, false, n, n, n, 1.f, A.data(), n, B.data(), n, 0.f, C.data(), n);
      else sgemm(false, false, n, n, n, 1.f, A.data(), n, B.data(), n, 0.f, C.data(), n);
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return 2.0 * n * n * n / best / 1e9;
  };
  double sys = bench(false), port = bench(true);
  std::printf("  768^3 sgemm: %s %.1f GFLOP/s, portable %.1f GFLOP/s (%.1f%%), one thread\n", kernel_backend_name(), sys, port, 100.0 * port / sys);
}

TEST(operators_on_the_portable_kernel) {
  set_force_portable_kernel(true);
  run_fixtures(1, 1);
  set_conv_tiling_for_test(1, 3);
  run_fixtures(3, 1);
  set_conv_tiling_for_test(4 << 20, 256);
  set_force_portable_kernel(false);
}

static std::string conv_model(bool two_d, int N, int C, int Hin, int Win, int M, int kh, int kw, int sh, int sw, int dh, int dw, int pad, int group) {
  std::mt19937 g(3);
  std::normal_distribution<float> nd;
  auto floats = [&](std::size_t n, float sc) { std::string b(n * 4, '\0'); for (std::size_t i = 0; i < n; ++i) { float v = nd(g) * sc; std::memcpy(&b[i * 4], &v, 4); } return b; };
  auto ints_attr = [&](const char* name, std::vector<int> v) {
    std::string a, packed;
    put_field(a, 1, name);
    for (int x : v) put_varint(packed, std::uint64_t(x));
    put_field(a, 8, packed);
    put_int(a, 20, 7);
    return a;
  };
  std::vector<int> ks, st, di, pd;
  if (two_d) { ks = {kh, kw}; st = {sh, sw}; di = {dh, dw}; pd = {pad, pad, pad, pad}; } else { ks = {kw}; st = {sw}; di = {dw}; pd = {pad, pad}; }
  std::string node, graph, model, opset;
  put_field(node, 1, "x"); put_field(node, 1, "w"); put_field(node, 1, "b"); put_field(node, 2, "y"); put_field(node, 4, "Conv");
  put_field(node, 5, ints_attr("kernel_shape", ks)); put_field(node, 5, ints_attr("strides", st));
  put_field(node, 5, ints_attr("dilations", di)); put_field(node, 5, ints_attr("pads", pd));
  { std::string a; put_field(a, 1, "group"); put_int(a, 3, std::uint64_t(group)); put_int(a, 20, 2); put_field(node, 5, a); }
  put_field(graph, 1, node);
  auto init = [&](const char* name, std::vector<int> dims, std::size_t n, float sc) {
    std::string t;
    for (int d : dims) put_int(t, 1, std::uint64_t(d));
    put_int(t, 2, 1); put_field(t, 8, name); put_field(t, 9, floats(n, sc));
    put_field(graph, 5, t);
  };
  std::vector<int> wd = two_d ? std::vector<int>{M, C / group, kh, kw} : std::vector<int>{M, C / group, kw};
  std::size_t wn = 1;
  for (int d : wd) wn *= std::size_t(d);
  init("w", wd, wn, 0.2f);
  init("b", {M}, std::size_t(M), 0.5f);
  std::string vin, ty, tt, shape, vout;
  for (int d : (two_d ? std::vector<int>{N, C, Hin, Win} : std::vector<int>{N, C, Win})) { std::string dim; put_int(dim, 1, std::uint64_t(d)); put_field(shape, 1, dim); }
  put_int(tt, 1, 1); put_field(tt, 2, shape); put_field(ty, 1, tt);
  put_field(vin, 1, "x"); put_field(vin, 2, ty); put_field(graph, 11, vin);
  put_field(vout, 1, "y"); put_field(graph, 12, vout);
  put_int(model, 1, 8); put_field(model, 7, graph);
  put_int(opset, 2, 17); put_field(model, 8, opset);
  return model;
}

TEST(portable_convolution_matches_the_system_blas_at_scale) {
  struct Cfg { bool two_d; int N, C, H, W, M, kh, kw, sh, sw, dh, dw, pad, group; };
  const Cfg cfgs[] = {
      {false, 1, 48, 1, 3001, 96, 1, 8, 4, 4, 1, 1, 2, 1},   // strided 1-D, tile borders in the output
      {false, 2, 32, 1, 1237, 32, 1, 3, 1, 1, 1, 5, 5, 4},   // dilated, grouped
      {true, 1, 8, 41, 303, 16, 3, 5, 1, 2, 1, 2, 2, 2},      // 2-D, grouped, strided and dilated
      {true, 1, 4, 64, 129, 24, 8, 1, 4, 1, 1, 1, 0, 1},      // the Demucs first-layer shape
  };
  for (int tile : {0, 1}) {
    if (tile) set_conv_tiling_for_test(1, 7);  // tiny tiles: every tile border on every row
    for (auto& c : cfgs) {
      std::string bytes = conv_model(c.two_d, c.N, c.C, c.H, c.W, c.M, c.kh, c.kw, c.sh, c.sw, c.dh, c.dw, c.pad, c.group);
      std::string err;
      std::vector<float> x(std::size_t(c.N) * c.C * (c.two_d ? c.H : 1) * c.W);
      std::mt19937 g(9);
      std::normal_distribution<float> nd;
      for (auto& v : x) v = nd(g);
      std::vector<float> res[2];
      for (int portable = 0; portable < 2; ++portable) {
        set_force_portable_kernel(portable == 1);
        auto ex = Executor::from_proto(parse_model(bytes_of(bytes)), ".", "", 3, &err);
        if (!ex) std::fprintf(stderr, "%s\n", err.c_str());
        CHECK(ex);
        InputBinding b; b.name = "x"; b.data = x.data();
        b.shape = c.two_d ? Shape{c.N, c.C, c.H, c.W} : Shape{c.N, c.C, c.W};
        if (!ex->run({b}, &err)) std::fprintf(stderr, "%s\n", err.c_str());
        CHECK(ex->run({b}, &err));
        res[portable].assign(ex->output("y")->f(), ex->output("y")->f() + ex->output("y")->numel());
      }
      set_force_portable_kernel(false);
      CHECK(res[0].size() == res[1].size() && res[0].size() > 1000);
      double worst = 0, peak = 0;
      for (std::size_t i = 0; i < res[0].size(); ++i) { worst = std::max(worst, double(std::fabs(res[0][i] - res[1][i]))); peak = std::max(peak, double(std::fabs(res[0][i]))); }
      CHECK(worst < 2e-5 * peak + 1e-6);
    }
    set_conv_tiling_for_test(4 << 20, 256);
  }
}

// ---- the recipe: RoFormer vocals, then Demucs on the remainder --------------------------------------------------------------
TEST(recipe_vocals_then_demucs_sums_to_the_input) {
  fs::path data = roformer_data();
  fs::path ck = models_root() / "demucs" / "955717e8-8726e21a.th";
  if (data.empty() || !fs::exists(ck)) { std::printf("  (RoFormer or Demucs checkpoint missing here: skipped)\n"); ++g_skipped; return; }
  fs::path res = fs::path(DH_STEMS_SKELETON_DIR).parent_path();
  std::string err;
  CatalogueEntry dm, rec;
  for (auto& e : list_catalogue(res)) { if (e.id == "demucs-htdemucs") dm = e; if (e.id == "recipe-roformer-demucs") rec = e; }
  CHECK(!dm.id.empty() && !rec.id.empty() && rec.requires_ids.size() == 2 && rec.files.empty() && rec.install_supported);
  auto conv = demucs_converter(data, res / "skeletons");
  // the recipe refuses to install before both models are there
  fs::path rdir = models_dir(data) / rec.id;
  fs::create_directories(rdir);
  fs::path dmdir = models_dir(data) / dm.id;
  if (!fs::exists(models_dir(data) / (dm.id + ".dhstems.json"))) {
    CHECK(!conv(rdir, rec, &err) && err.find("demucs-htdemucs") != std::string::npos);
    fs::create_directories(dmdir);
    std::error_code ec;
    fs::create_symlink(fs::absolute(ck), dmdir / dm.files[0].name, ec);
    if (ec) fs::copy_file(ck, dmdir / dm.files[0].name);
    CHECK(conv(dmdir, dm, &err));
  }
  CHECK(conv(rdir, rec, &err));
  auto models = list_models(models_dir(data));
  const ModelManifest* mm = nullptr;
  for (auto& m : models) if (m.id == rec.id) mm = &m;
  CHECK(mm && mm->pipeline == "recipe-vocals-then-demucs" && mm->parts.size() == 2);
  auto model = open_model(*mm, 4, &err);
  if (!model) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(model);
  // 10 s of voice-like audio: two overlapping chunks through the real separate() with its exact mixture-consistency step
  const std::size_t n = 441000;
  auto mono = synth_vocalish(n);
  Audio in;
  in.sample_rate = 44100;
  in.l.assign(mono.begin(), mono.begin() + std::ptrdiff_t(n));
  in.r.assign(mono.begin() + std::ptrdiff_t(n), mono.end());
  auto t0 = std::chrono::steady_clock::now();
  SeparateResult r = separate(in, *model);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.status == SeparateResult::Status::Ok);
  double sum_err = 0, peak[4] = {0, 0, 0, 0};
  for (std::size_t i = 0; i < n; ++i) {
    double sl = 0, sr = 0;
    for (int k = 0; k < 4; ++k) {
      sl += r.stems[std::size_t(k)].l[i]; sr += r.stems[std::size_t(k)].r[i];
      peak[k] = std::max(peak[k], double(std::fabs(r.stems[std::size_t(k)].l[i])));
    }
    sum_err = std::max({sum_err, std::fabs(sl - in.l[i]), std::fabs(sr - in.r[i])});
  }
  std::printf("  recipe, 10 s in %.1f s: stem peaks vocals %.3f drums %.3f bass %.3f other %.3f; max |sum of stems - input| = %.2e\n", secs, peak[0], peak[1], peak[2], peak[3], sum_err);
  CHECK(sum_err < 1e-5);                      // "all stems on" is transparent
  CHECK(peak[0] > 1e-3 && peak[1] > 1e-3 && peak[2] > 1e-3);  // every part of the recipe contributed
}


// ---- half-precision weights on disk (--half skeletons): converter writes IEEE half, the executor widens at load ----------------
TEST(half_precision_weights_round_trip) {
  fs::path d = tmpdir("half");
  // converter: tiny_state.pt holds a.weight [2,3] = 0..5 and b.bias [4] (float16 0.5, -1, ..., 0.25); both are exact in half precision
  fs::path tiny = fs::path(DH_STEMS_TEST_DIR) / "tiny_state.pt";
  { std::ofstream(d / "l.json") << R"J({"format":"deckhand-stems-layout/1","total_bytes":64,"tensors":[
    {"name":"a.weight","offset":0,"length":12,"shape":[2,3],"dtype":"float16"},{"name":"b.bias","offset":64,"length":8,"shape":[4],"dtype":"float16"}],
    "ignore":["c.t","d.idx"]})J"; }
  std::string err;
  if (fs::exists(tiny)) {
    // total_bytes 64 cannot hold the second tensor at offset 64: refused, nothing left behind
    CHECK(!convert_checkpoint(tiny, d / "l.json", d / "w.bin", nullptr, &err) && !fs::exists(d / "w.bin.part"));
    { std::ofstream(d / "l2.json") << R"J({"format":"deckhand-stems-layout/1","total_bytes":128,"tensors":[
      {"name":"a.weight","offset":0,"length":12,"shape":[2,3],"dtype":"float16"},{"name":"b.bias","offset":64,"length":8,"shape":[4],"dtype":"float16"}],
      "ignore":["c.t","d.idx"]})J"; }
    CHECK(convert_checkpoint(tiny, d / "l2.json", d / "w.bin", nullptr, &err));
    auto b = read_file(d / "w.bin");
    CHECK(b.size() == 128 && b[0] == 0 && b[1] == 0 && b[2] == 0x00 && b[3] == 0x3c && b[4] == 0 && b[5] == 0x40);  // 0, 1.0 (0x3c00), 2.0 (0x4000)
    CHECK(b[64] == 0 && b[65] == 0x38);                                                                              // 0.5 = 0x3800
    CHECK(b[66] == 0 && b[67] == 0xbc);                                                                              // -1.0 = 0xbc00
  }
  // executor: y = x + w with w stored as external half data
  std::vector<std::uint16_t> half = {0x3c00, 0x4000, 0xbc00, 0x3800, 0x0001, 0x7bff};  // 1, 2, -1, 0.5, 2^-24, 65504
  { std::ofstream f(d / "h.bin", std::ios::binary); f.write(reinterpret_cast<const char*>(half.data()), std::streamsize(half.size() * 2)); }
  std::string t, e1, e2, e3, vin, vout, ty, tt, shape, dim, node, graph, model, opset;
  put_int(t, 1, 6); put_int(t, 2, 10); put_field(t, 8, "w");
  put_field(e1, 1, "location"); put_field(e1, 2, "h.bin"); put_field(t, 13, e1);
  put_field(e2, 1, "offset"); put_field(e2, 2, "0"); put_field(t, 13, e2);
  put_field(e3, 1, "length"); put_field(e3, 2, "12"); put_field(t, 13, e3);
  put_int(t, 14, 1);
  put_field(node, 1, "x"); put_field(node, 1, "w"); put_field(node, 2, "y"); put_field(node, 4, "Add");
  put_field(graph, 1, node); put_field(graph, 5, t);
  put_int(dim, 1, 6); put_field(shape, 1, dim); put_int(tt, 1, 1); put_field(tt, 2, shape); put_field(ty, 1, tt);
  put_field(vin, 1, "x"); put_field(vin, 2, ty); put_field(graph, 11, vin);
  put_field(vout, 1, "y"); put_field(graph, 12, vout);
  put_int(model, 1, 8); put_field(model, 7, graph);
  put_int(opset, 2, 17); put_field(model, 8, opset);
  auto ex = Executor::from_proto(parse_model(bytes_of(model)), d.string(), "", 1, &err);
  if (!ex) std::fprintf(stderr, "%s\n", err.c_str());
  CHECK(ex);
  float in[6] = {10, 10, 10, 10, 10, 10};
  InputBinding b; b.name = "x"; b.data = in; b.shape = Shape{6};
  CHECK(ex->run({b}, &err));
  const float want[6] = {11, 12, 9, 10.5f, 10.f + 5.9604645e-8f, 10.f + 65504.f};
  for (int i = 0; i < 6; ++i) CHECK(ex->output("y")->f()[i] == want[i]);
  fs::remove_all(d);
}

int main(int argc, char** argv) {
  bool filtered = argc > 1;
  for (auto& c : run_list()) {
    if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
    std::printf("[ RUN ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    ++g_tests;
  }
  for (auto& d : g_cleanup) { std::error_code ec; fs::remove_all(d, ec); }
  std::printf("test_stems_graph: %d tests passed (%d skipped for missing weights), kernels: %s%s\n", g_tests, g_skipped, kernel_backend_name(), filtered ? " (filtered)" : "");
  return 0;
}
