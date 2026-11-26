// deckhand-stems: separate a WAV file into four stem WAVs for listening checks, and manage installed models.
//   deckhand-stems separate <in.wav> <outdir> [--data DIR] [--model ID] [--cache] [--bits 16|24|32]
//   deckhand-stems models [--data DIR]
// Other input formats: decode to WAV first (the app decodes through the platform layer).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "deckhand/stems/cache.hpp"
#include "deckhand/stems/checkpoint.hpp"
#include "deckhand/stems/model_convert.hpp"
#include "deckhand/stems/manifest.hpp"
#include "deckhand/stems/model_install.hpp"
#include "deckhand/stems/stems.hpp"

using namespace deckhand::stems;
namespace fs = std::filesystem;

static fs::path default_data_dir() {
#if defined(_WIN32)
  const char* a = std::getenv("APPDATA");
  return fs::path(a ? a : ".") / "DeckHand";
#elif defined(__APPLE__)
  const char* h = std::getenv("HOME");
  return fs::path(h ? h : ".") / "Library" / "Application Support" / "DeckHand";
#else
  const char* x = std::getenv("XDG_DATA_HOME");
  const char* h = std::getenv("HOME");
  return x && *x ? fs::path(x) / "deckhand" : fs::path(h ? h : ".") / ".local" / "share" / "deckhand";
#endif
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: deckhand-stems separate <in.wav> <outdir> [--data DIR] [--model ID] [--cache] [--bits N]\n       deckhand-stems models [--data DIR]\n"); return 2; }
  std::string cmd = argv[1], model_id, in, outdir;
  fs::path data = default_data_dir();
  int bits = 24;
  bool use_cache = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--data" && i + 1 < argc) data = argv[++i];
    else if (a == "--model" && i + 1 < argc) model_id = argv[++i];
    else if (a == "--bits" && i + 1 < argc) bits = std::atoi(argv[++i]);
    else if (a == "--cache") use_cache = true;
    else if (in.empty()) in = a;
    else outdir = a;
  }
  if (cmd == "ckpt" && !in.empty()) {  // inspect a PyTorch checkpoint with the hardened reader: ckpt <file> [tensor-name]
    Checkpoint ck;
    std::string err;
    if (!read_checkpoint(in, ck, &err)) { std::fprintf(stderr, "rejected: %s\n", err.c_str()); return 1; }
    std::printf("%zu tensors, wrapper '%s'\n", ck.tensors.size(), ck.wrapper_class.c_str());
    for (auto& t : ck.tensors) {
      if (!outdir.empty() && t.name != outdir) continue;
      std::vector<float> v;
      if (!read_tensor_f32(in, t, v, &err)) { std::fprintf(stderr, "%s: %s\n", t.name.c_str(), err.c_str()); return 1; }
      double sum = 0;
      for (float x : v) sum += x;
      std::printf("%s %s numel=%zu sum=%.6f first=%.6g,%.6g,%.6g\n", t.name.c_str(), t.dtype.c_str(), v.size(), sum, v.size() > 0 ? v[0] : 0.f, v.size() > 1 ? v[1] : 0.f, v.size() > 2 ? v[2] : 0.f);
      if (outdir.empty() && &t - &ck.tensors[0] >= 4) break;
    }
    return 0;
  }
  if (cmd == "convert" && argc >= 5) {  // convert <checkpoint> <layout.json> <out.weights>
    std::string err;
    int last = -1;
    if (!convert_checkpoint(argv[2], argv[3], argv[4], [&](float f) { if (int(f * 100) != last) { last = int(f * 100); std::fprintf(stderr, "\r%3d%%", last); } }, &err)) { std::fprintf(stderr, "\nfailed: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "\n");
    return 0;
  }
  if (cmd == "catalogue" || cmd == "install") {  // catalogue | install <id> [--accept-licence] [--resources DIR]
    fs::path res = fs::path("resources") / "stem-models";
    bool accept = false;
    for (int i = 2; i < argc; ++i) { std::string a = argv[i]; if (a == "--accept-licence") accept = true; else if (a == "--resources" && i + 1 < argc) res = argv[++i]; }
    auto cat = list_catalogue(res);
    if (cmd == "catalogue") {
      for (auto& c : cat) std::printf("%s  %s  %.0f MB  %s\n    licence (%s): %s\n    %s\n", c.id.c_str(), c.name.c_str(), double(c.total_bytes()) / 1e6, c.install_supported ? "installable" : ("not installable yet: " + c.blocked_by).c_str(), c.restriction.c_str(), c.licence_summary.c_str(), c.licence_link.c_str());
      return 0;
    }
    for (auto& c : cat) {
      if (c.id != in) continue;
      std::printf("%s\nLicence statement: %s\nSource: %s\n", c.name.c_str(), c.licence_summary.c_str(), c.licence_source.c_str());
      if (!accept) { std::printf("Not installed. Re-run with --accept-licence if your use is within these terms.\n"); return 4; }
      InstallHooks hk;
      if (c.kind == "demucs") hk.convert = demucs_converter(data, res / "skeletons");
      int last = -1;
      auto r = install_model(data, c, true, [&](float f) { if (int(f * 100) != last) { last = int(f * 100); std::fprintf(stderr, "\r%3d%%", last); } }, nullptr, hk);
      std::fprintf(stderr, "\n");
      if (!r.ok) { std::fprintf(stderr, "install failed: %s\n", r.error.c_str()); return 1; }
      std::printf("installed %s\n", c.id.c_str());
      return 0;
    }
    std::fprintf(stderr, "unknown model id\n");
    return 2;
  }
  std::vector<std::string> problems;
  auto models = list_models(models_dir(data), &problems);
  if (cmd == "models") {
    std::printf("backend: %s (available: %s)\n", backend_name(), stems_available() ? "yes" : "no");
    for (auto& m : models) std::printf("%s  %s  licence: %s\n", m.id.c_str(), m.file.c_str(), m.licence.c_str());
    for (auto& p : problems) std::printf("invalid manifest %s\n", p.c_str());
    if (models.empty()) std::printf("no models installed in %s\n", models_dir(data).string().c_str());
    return 0;
  }
  if (cmd != "separate" || in.empty() || outdir.empty()) { std::fprintf(stderr, "bad arguments\n"); return 2; }
  const ModelManifest* mm = nullptr;
  for (auto& m : models) if (model_id.empty() || m.id == model_id) { mm = &m; break; }
  if (!mm) { std::fprintf(stderr, "no matching model installed in %s (see 'deckhand-stems models')\n", models_dir(data).string().c_str()); return 3; }
  std::string err;
  Audio a;
  if (!read_wav(in, a, &err)) { std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str()); return 1; }
  std::string key = content_key(a, mm->id);
  Stems st;
  std::unique_ptr<StemCache> cache;
  if (use_cache) cache.reset(new StemCache(CacheConfig{cache_dir(data)}));
  if (cache && cache->load_stems(key, st)) {
    std::printf("loaded from cache %s\n", key.c_str());
  } else {
    auto model = open_model(*mm, 0, &err);
    if (!model) { std::fprintf(stderr, "%s\n", err.c_str()); return 3; }
    SeparateOptions o;
    o.overlap = mm->overlap;
    o.progress = [](float f) { std::fprintf(stderr, "\r%3d%%", int(f * 100)); };
    SeparateResult r = separate(a, *model, o);
    std::fprintf(stderr, "\n");
    if (r.status != SeparateResult::Status::Ok) { std::fprintf(stderr, "failed: %s\n", r.error.c_str()); return 1; }
    double secs = double(a.frames()) / a.sample_rate;
    std::printf("%.1f s of audio in %.1f s = %.2fx real time\n", secs, r.seconds, secs / r.seconds);
    st = std::move(r.stems);
    if (cache && !cache->store(key, st, &err)) std::fprintf(stderr, "cache store failed: %s\n", err.c_str());
  }
  std::error_code ec;
  fs::create_directories(outdir, ec);
  std::string base = fs::path(in).stem().string();
  for (int s = 0; s < kStemCount; ++s) {
    std::string out = (fs::path(outdir) / (base + "." + stem_name(s) + ".wav")).string();
    if (!write_wav(out, st[std::size_t(s)], bits, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("wrote %s\n", out.c_str());
  }
  return 0;
}
