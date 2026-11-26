#include "deckhand/stems/manifest.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "deckhand/stems/cache.hpp"
#include "internal.hpp"

namespace deckhand::stems {
namespace fs = std::filesystem;
using detail::Json;

namespace {
bool fail(std::string* e, const std::string& m) { if (e) *e = m; return false; }
bool plain_name(const std::string& s) {  // "name" or "dir/name": relative, no "..", no drive letters, no backslashes
  if (s.empty() || s.find('\\') != std::string::npos || s.find("..") != std::string::npos || s.find(':') != std::string::npos || s[0] == '/' || s.back() == '/') return false;
  return std::count(s.begin(), s.end(), '/') <= 1 && s.find("//") == std::string::npos;
}
bool id_ok(const std::string& s) {
  if (s.empty() || s.size() > 96) return false;
  for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
  return true;
}
bool hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
std::string str_of(const Json& j, const char* k, bool* present = nullptr) {
  const Json* v = j.get(k);
  if (present) *present = v && v->is_str();
  return v && v->is_str() ? v->s : std::string();
}
}  // namespace

bool parse_manifest(const std::string& text, ModelManifest& m, std::string* err) {
  Json j;
  std::string e;
  if (!detail::parse_json(text, j, &e)) return fail(err, "invalid JSON: " + e);
  if (j.t != Json::T::Obj) return fail(err, "manifest must be a JSON object");
  ModelManifest o;
  if (str_of(j, "format") != "deckhand-stems-model/1") return fail(err, "missing or unsupported \"format\" (want deckhand-stems-model/1)");
  o.id = str_of(j, "id");
  if (!id_ok(o.id)) return fail(err, "\"id\" must be 1-96 chars of A-Z a-z 0-9 . _ -");
  o.name = str_of(j, "name");
  o.pipeline = str_of(j, "pipeline");
  const bool recipe = o.pipeline == "recipe-vocals-then-demucs";  // runs two installed models: no file of its own
  o.file = str_of(j, "file");
  if (!recipe && !plain_name(o.file)) return fail(err, "\"file\" must be a plain file name next to the manifest");
  if (recipe) {
    const Json* pt = j.get("parts");
    if (!pt || pt->t != Json::T::Arr || pt->a.size() != 2) return fail(err, "\"parts\" must name the vocal model and the Demucs model");
    for (auto& p : pt->a) { if (!p.is_str() || !id_ok(p.s)) return fail(err, "bad model id in \"parts\""); o.parts.push_back(p.s); }
    o.file.clear();
  }
  o.licence = str_of(j, "licence");
  if (o.licence.empty()) return fail(err, "\"licence\" is required");
  o.source_url = str_of(j, "source_url");
  if (o.source_url.empty()) return fail(err, "\"source_url\" is required");
  o.sha256 = str_of(j, "sha256");
  if (!recipe && !hex64(o.sha256)) return fail(err, "\"sha256\" must be 64 lower-case hex digits");
  const Json* v = j.get("sample_rate");
  if (!v || !v->is_num() || v->n < 8000 || v->n > 192000 || v->n != std::floor(v->n)) return fail(err, "\"sample_rate\" must be an integer 8000..192000");
  o.sample_rate = int(v->n);
  if ((v = j.get("segment_frames")) && v->is_num()) {
    if (v->n < 256 || v->n > 1e8 || v->n != std::floor(v->n)) return fail(err, "\"segment_frames\" out of range");
    o.segment_frames = std::size_t(v->n);
  } else if ((v = j.get("segment_seconds")) && v->is_num()) {
    double f = v->n * o.sample_rate;
    if (f < 256 || f > 1e8) return fail(err, "\"segment_seconds\" out of range");
    o.segment_frames = std::size_t(std::llround(f));
  } else {
    return fail(err, "\"segment_frames\" or \"segment_seconds\" is required");
  }
  if ((v = j.get("channels"))) { if (!v->is_num() || v->n != 2) return fail(err, "only \"channels\": 2 is supported"); }
  if ((v = j.get("overlap"))) { if (!v->is_num() || v->n < 0 || v->n > 0.5) return fail(err, "\"overlap\" must be 0..0.5"); o.overlap = float(v->n); }
  if (!o.pipeline.empty() && o.pipeline != "htdemucs-core" && o.pipeline != "htdemucs-ft-core" && o.pipeline != "roformer-vocals" && !recipe)
    return fail(err, "unknown \"pipeline\"");
  const Json* in = j.get("input");
  if (!o.pipeline.empty() && !in) {  // fixed-signature pipeline: tensor names come from the pipeline
    if ((v = j.get("execution_provider"))) o.execution_provider = v->is_str() ? v->s : "";
    const Json* st0 = j.get("stems");
    if (!st0 || st0->t != Json::T::Arr || st0->a.size() != kStemCount) return fail(err, "\"stems\" must list the model's output order");
    std::array<bool, kStemCount> seen0{};
    for (std::size_t i = 0; i < kStemCount; ++i) {
      int id = st0->a[i].is_str() ? stem_from_name(st0->a[i].s) : -1;
      if (id < 0 || seen0[std::size_t(id)]) return fail(err, "\"stems\" must be a permutation of vocals, drums, bass, other");
      seen0[std::size_t(id)] = true;
      o.stems[i] = st0->a[i].s;
    }
    m = std::move(o);
    return true;
  }
  if (!in || in->t != Json::T::Obj || !in->get("name") || !in->get("name")->is_str() || in->get("name")->s.empty()) return fail(err, "\"input.name\" is required");
  o.input_name = in->get("name")->s;
  if (in->get("layout")) o.input_layout = str_of(*in, "layout");
  const Json* out = j.get("output");
  if (!out || out->t != Json::T::Obj) return fail(err, "\"output\" is required");
  if (out->get("layout")) o.output_layout = str_of(*out, "layout");
  const Json* names = out->get("names");
  if (names && names->t == Json::T::Arr) {
    for (auto& n : names->a) { if (!n.is_str() || n.s.empty()) return fail(err, "\"output.names\" must be strings"); o.output_names.push_back(n.s); }
    if (o.output_names.size() != kStemCount) return fail(err, "\"output.names\" must list four tensors");
  } else {
    o.output_name = str_of(*out, "name");
    if (o.output_name.empty()) return fail(err, "\"output.name\" or \"output.names\" is required");
  }
  const bool has_s = o.output_layout.find('S') != std::string::npos;
  if (has_s != o.output_names.empty()) return fail(err, "output layout needs an S (stem) axis exactly when a single output tensor is used");
  auto layout_ok = [](const std::string& l, const std::string& letters) {
    for (char c : l) if (letters.find(c) == std::string::npos) return false;
    return !l.empty();
  };
  if (!layout_ok(o.input_layout, "BCT") || o.input_layout.find('C') == std::string::npos || o.input_layout.find('T') == std::string::npos)
    return fail(err, "input layout must use letters B, C, T (C and T required)");
  if (!layout_ok(o.output_layout, "BSCT") || o.output_layout.find('C') == std::string::npos || o.output_layout.find('T') == std::string::npos)
    return fail(err, "output layout must use letters B, S, C, T (C and T required)");
  const Json* st = j.get("stems");
  if (!st || st->t != Json::T::Arr || st->a.size() != kStemCount) return fail(err, "\"stems\" must list vocals, drums, bass, other in model output order");
  std::array<bool, kStemCount> seen{};
  for (std::size_t i = 0; i < kStemCount; ++i) {
    int id = st->a[i].is_str() ? stem_from_name(st->a[i].s) : -1;
    if (id < 0 || seen[std::size_t(id)]) return fail(err, "\"stems\" must be a permutation of vocals, drums, bass, other");
    seen[std::size_t(id)] = true;
    o.stems[i] = st->a[i].s;
  }
  if ((v = j.get("execution_provider"))) {
    o.execution_provider = v->is_str() ? v->s : "";
    if (o.execution_provider != "cpu" && o.execution_provider != "coreml") return fail(err, "\"execution_provider\" must be cpu or coreml");
  }
  m = std::move(o);
  return true;
}

bool load_manifest(const fs::path& file, ModelManifest& m, std::string* err) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return fail(err, "cannot open " + file.string());
  std::ostringstream ss;
  ss << f.rdbuf();
  if (!parse_manifest(ss.str(), m, err)) return false;
  m.dir = file.parent_path();
  return true;
}

std::vector<ModelManifest> list_models(const fs::path& dir, std::vector<std::string>* problems) {
  std::vector<ModelManifest> v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.size() < 13 || n.compare(n.size() - 13, 13, ".dhstems.json") != 0) continue;
    ModelManifest m;
    std::string err;
    if (load_manifest(e.path(), m, &err)) v.push_back(std::move(m));
    else if (problems) problems->push_back(n + ": " + err);
  }
  std::sort(v.begin(), v.end(), [](const ModelManifest& a, const ModelManifest& b) { return a.id < b.id; });
  return v;
}

bool verify_model_file(const ModelManifest& m, std::string* err) {
  if (m.pipeline == "recipe-vocals-then-demucs") return true;  // nothing of its own; the parts are verified when opened
  fs::path p = m.dir / m.file;
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return fail(err, "model file missing: " + p.string());
  std::string h;
  if (!sha256_file(p, h)) return fail(err, "cannot read " + p.string());
  if (h != m.sha256) return fail(err, "SHA-256 mismatch for " + m.file + " (expected " + m.sha256 + ", got " + h + ")");
  return true;
}

std::unique_ptr<StemModel> open_model(const ModelManifest& m, int threads, std::string* err) {
  if (!stems_available()) { fail(err, "this build has no model backend (stems are disabled at build time)"); return nullptr; }
  if (!verify_model_file(m, err)) return nullptr;
  // The built-in executor runs everything; ONNX Runtime (when compiled in) is used only where CoreML is asked for.
  if (m.pipeline == "recipe-vocals-then-demucs") return detail::open_recipe(m, threads, err);
  if (detail::kHaveOnnx && m.execution_provider == "coreml") return detail::open_onnx(m, (m.dir / m.file).string(), threads, err);
  return detail::open_graph(m, (m.dir / m.file).string(), threads, err);
}

}  // namespace deckhand::stems
