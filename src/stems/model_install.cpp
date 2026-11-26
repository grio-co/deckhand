#include "deckhand/stems/model_install.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "deckhand/stems/cache.hpp"
#include "deckhand/stems/model_convert.hpp"
#include "deckhand/stems/manifest.hpp"
#include "internal.hpp"

namespace deckhand::stems {
namespace fs = std::filesystem;
using detail::Json;

namespace {
bool fail(std::string* e, const std::string& m) { if (e) *e = m; return false; }
std::string sget(const Json& j, const char* k) { const Json* v = j.get(k); return v && v->is_str() ? v->s : std::string(); }
bool hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
std::string host_of(const std::string& url) {
  if (url.compare(0, 8, "https://") != 0) return {};
  std::size_t e = url.find_first_of("/?#", 8);
  std::string h = url.substr(8, e == std::string::npos ? std::string::npos : e - 8);
  if (h.find('@') != std::string::npos || h.find(':') != std::string::npos) return {};  // no userinfo or ports
  for (auto& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
  return h;
}
}  // namespace

bool url_host_allowed(const std::string& url) {
  static const char* ok[] = {"dl.fbaipublicfiles.com", "huggingface.co", "github.com"};
  std::string h = host_of(url);
  for (auto* o : ok) if (h == o) return true;
  return false;
}

bool parse_catalogue_entry(const std::string& text, CatalogueEntry& out, std::string* err) {
  Json j;
  std::string e;
  if (!detail::parse_json(text, j, &e)) return fail(err, "invalid JSON: " + e);
  if (j.t != Json::T::Obj || sget(j, "format") != "deckhand-stems-catalogue/1") return fail(err, "not a deckhand-stems-catalogue/1 entry");
  CatalogueEntry c;
  c.id = sget(j, "id");
  if (c.id.empty() || c.id.size() > 64) return fail(err, "bad id");
  for (char ch : c.id) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) return fail(err, "bad id");
  c.name = sget(j, "name"); c.kind = sget(j, "kind"); c.summary = sget(j, "summary"); c.quality = sget(j, "quality");
  c.speed = sget(j, "speed"); c.upstream_url = sget(j, "upstream_url");
  if (const Json* s = j.get("stems")) for (auto& x : s->a) if (x.is_str()) c.stems.push_back(x.s);
  const Json* files = j.get("files");
  if (!files || files->t != Json::T::Arr || (files->a.empty() && c.kind != "recipe")) return fail(err, "no files");  // a recipe downloads nothing
  for (auto& f : files->a) {
    CatalogueFile cf;
    cf.name = sget(f, "name"); cf.url = sget(f, "url"); cf.sha256 = sget(f, "sha256");
    const Json* sz = f.get("size");
    cf.size = sz && sz->is_num() && sz->n > 0 ? std::uint64_t(sz->n) : 0;
    if (cf.name.empty() || cf.name.find('/') != std::string::npos || cf.name.find('\\') != std::string::npos || cf.name.find("..") != std::string::npos)
      return fail(err, "bad file name");
    if (!hex64(cf.sha256)) return fail(err, "file " + cf.name + ": SHA-256 is mandatory");
    if (!url_host_allowed(cf.url)) return fail(err, "file " + cf.name + ": host not on the allow-list");
    if (!cf.size) return fail(err, "file " + cf.name + ": size is mandatory");
    c.files.push_back(std::move(cf));
  }
  const Json* l = j.get("licence");
  if (!l || l->t != Json::T::Obj || sget(*l, "summary").empty()) return fail(err, "a licence summary is mandatory");
  c.licence_summary = sget(*l, "summary"); c.licence_source = sget(*l, "statement_source"); c.licence_link = sget(*l, "link");
  c.weights_licence = sget(*l, "weights_licence"); c.code_licence = sget(*l, "code_licence"); c.training_data = sget(*l, "training_data");
  c.restriction = sget(*l, "restriction");
  if (const Json* rq = j.get("requires")) {
    if (rq->t != Json::T::Arr) return fail(err, "\"requires\" must be a list of model ids");
    for (auto& r : rq->a) { if (!r.is_str() || r.s.empty()) return fail(err, "bad model id in \"requires\""); c.requires_ids.push_back(r.s); }
  }
  if (const Json* in = j.get("install")) {
    const Json* sp = in->get("supported");
    c.install_supported = sp && sp->t == Json::T::Bool && sp->b;
    c.blocked_by = sget(*in, "blocked_by");
  }
  out = std::move(c);
  return true;
}

std::vector<CatalogueEntry> list_catalogue(const fs::path& dir, std::vector<std::string>* problems) {
  std::vector<CatalogueEntry> v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json") continue;
    std::ifstream f(e.path(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    CatalogueEntry c;
    std::string err;
    if (parse_catalogue_entry(ss.str(), c, &err)) v.push_back(std::move(c));
    else if (problems) problems->push_back(e.path().filename().string() + ": " + err);
  }
  std::sort(v.begin(), v.end(), [](const CatalogueEntry& a, const CatalogueEntry& b) { return a.id < b.id; });
  return v;
}

// ---- downloader: the system curl, no shell, no extra dependency ---------------------------------------------------------
DownloadFn default_downloader() {
  return [](const std::string& url, const fs::path& dest, std::uint64_t, const std::function<void(std::uint64_t)>& bytes_cb,
            const std::atomic<bool>* cancel, std::string* error) -> bool {
    if (!url_host_allowed(url)) return fail(error, "download host not allowed");
    std::error_code ec;
    fs::remove(dest, ec);
    const std::string d = dest.string();
#if defined(_WIN32)
    std::string cmd = "curl.exe --fail --silent --show-error --location --proto =https --proto-redir =https --tlsv1.2 -o \"" + d + "\" \"" + url + "\"";
    STARTUPINFOA si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return fail(error, "cannot start curl.exe");
    for (;;) {
      DWORD w = WaitForSingleObject(pi.hProcess, 200);
      std::error_code e2; auto sz = fs::exists(dest, e2) ? fs::file_size(dest, e2) : 0;
      if (bytes_cb && !e2) bytes_cb(sz);
      if (w == WAIT_OBJECT_0) break;
      if (cancel && cancel->load()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); fs::remove(dest, e2); return fail(error, "cancelled"); }
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code == 0 ? true : fail(error, "curl failed with exit code " + std::to_string(code));
#else
    std::vector<std::string> args = {"curl", "--fail", "--silent", "--show-error", "--location", "--proto", "=https", "--proto-redir", "=https",
                                     "--tlsv1.2", "--connect-timeout", "30", "-o", d, url};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) return fail(error, "cannot start curl");
    int status = 0;
    for (;;) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      std::error_code e2;
      auto sz = fs::exists(dest, e2) ? fs::file_size(dest, e2) : 0;
      if (bytes_cb && !e2) bytes_cb(sz);
      if (r == pid) break;
      if (r < 0) return fail(error, "waitpid failed");
      if (cancel && cancel->load()) { kill(pid, SIGTERM); waitpid(pid, &status, 0); fs::remove(dest, e2); return fail(error, "cancelled"); }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    return fail(error, "curl failed (exit code " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
#endif
  };
}

// ---- install / list / remove --------------------------------------------------------------------------------------------
InstallResult install_model(const fs::path& data, const CatalogueEntry& e, bool accept, const std::function<void(float)>& progress,
                            const std::atomic<bool>* cancel, const InstallHooks& hooks) {
  InstallResult r;
  auto bad = [&](const std::string& m) { r.ok = false; r.error = m; return r; };
  if (!accept) return bad("the licence has not been accepted");
  if (!e.install_supported) return bad("this model cannot be installed yet: " + (e.blocked_by.empty() ? std::string("conversion step missing") : e.blocked_by));
  for (auto& id : e.requires_ids) {
    ModelManifest mm;
    if (!load_manifest(models_dir(data) / (id + ".dhstems.json"), mm, nullptr)) return bad("this entry needs \"" + id + "\" installed first");
  }
  for (auto& f : e.files) if (!url_host_allowed(f.url) || !hex64(f.sha256) || !f.size) return bad("catalogue entry is not acceptable (host, hash or size)");
  const fs::path dir = models_dir(data) / e.id;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return bad("cannot create " + dir.string());
  auto sp = fs::space(dir, ec);
  if (!ec && sp.available < e.total_bytes() * 2) return bad("not enough free disk space (need about " + std::to_string(e.total_bytes() * 2 / (1 << 20)) + " MB)");
  DownloadFn dl = hooks.download ? hooks.download : default_downloader();
  std::uint64_t done_before = 0;
  const std::uint64_t total = e.total_bytes();
  for (auto& f : e.files) {
    const fs::path final_path = dir / f.name, part = dir / (f.name + ".part");
    std::string have;
    if (fs::exists(final_path, ec) && sha256_file(final_path, have) && have == f.sha256) { done_before += f.size; if (progress) progress(float(done_before) / float(total)); continue; }
    std::string err;
    bool ok = dl(f.url, part, f.size, [&](std::uint64_t b) { if (progress) progress(std::min(1.f, float(done_before + b) / float(total)) * 0.95f); }, cancel, &err);
    if (!ok) {
      fs::remove(part, ec);
      r.cancelled = cancel && cancel->load();
      return bad(r.cancelled ? "cancelled" : "download failed: " + err);
    }
    if (fs::file_size(part, ec) != f.size) { fs::remove(part, ec); return bad("downloaded size of " + f.name + " does not match the catalogue"); }
    if (!sha256_file(part, have) || have != f.sha256) { fs::remove(part, ec); return bad("SHA-256 mismatch for " + f.name + ": the download was discarded"); }
    fs::rename(part, final_path, ec);
    if (ec) return bad("cannot move " + f.name + " into place");
    done_before += f.size;
  }
  if (hooks.convert) {
    std::string err;
    if (!hooks.convert(dir, e, &err)) return bad("conversion failed: " + err);
  }
  std::ofstream rec(dir / "install.json", std::ios::trunc);
  std::time_t now = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
  auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n') o += "\\n"; else o += c; } return o; };
  rec << "{\"id\":\"" << esc(e.id) << "\",\"name\":\"" << esc(e.name) << "\",\"licence\":\"" << esc(e.weights_licence) << "\",\"licence_summary_accepted\":\""
      << esc(e.licence_summary) << "\",\"licence_accepted_at\":\"" << ts << "\"}\n";
  if (progress) progress(1.f);
  r.ok = true;
  return r;
}

std::function<bool(const fs::path&, const CatalogueEntry&, std::string*)> demucs_converter(const fs::path& data, const fs::path& skel) {
  auto demucs = [data, skel](const fs::path& dir, const CatalogueEntry& e, std::string* err) -> bool {
    // One checkpoint = htdemucs; four = htdemucs_ft in the order of the upstream bag: drums, bass, other, vocals.
    static const char* const kBagStems[4] = {"drums", "bass", "other", "vocals"};
    const bool bag = e.files.size() == 4;
    if (e.kind != "demucs" || (e.files.size() != 1 && !bag)) return fail(err, "not a Demucs entry with one or four checkpoints");
    std::error_code ec;
    fs::copy_file(skel / "htdemucs.onnx", dir / "htdemucs.onnx", fs::copy_options::overwrite_existing, ec);
    if (ec) return fail(err, "skeleton missing: " + (skel / "htdemucs.onnx").string());
    for (std::size_t k = 0; k < e.files.size(); ++k) {
      std::string out = bag ? std::string("htdemucs.") + kBagStems[k] + ".weights" : "htdemucs.weights";
      if (!convert_checkpoint(dir / e.files[k].name, skel / "htdemucs.layout.json", dir / out, nullptr, err)) return false;
    }
    std::string sk;
    if (!sha256_file(dir / "htdemucs.onnx", sk)) return fail(err, "cannot hash the skeleton");
    std::ostringstream m;
    m << "{\"format\":\"deckhand-stems-model/1\",\"id\":\"" << e.id << "\",\"name\":\"" << e.name << "\",\"file\":\"" << e.id << "/htdemucs.onnx\","
      << "\"licence\":\"" << e.weights_licence << "\",\"source_url\":\"" << e.files[0].url << "\",\"sha256\":\"" << sk << "\","
      << "\"sample_rate\":44100,\"segment_frames\":343980,\"overlap\":0.25,\"channels\":2,\"pipeline\":\"" << (bag ? "htdemucs-ft-core" : "htdemucs-core") << "\","
      << "\"stems\":[\"drums\",\"bass\",\"other\",\"vocals\"]}\n";
    fs::path mf = models_dir(data) / (e.id + ".dhstems.json"), part = mf;
    part += ".part";
    { std::ofstream f(part, std::ios::trunc); f << m.str(); if (!f) return fail(err, "cannot write manifest"); }
    fs::rename(part, mf, ec);
    if (ec) return fail(err, "cannot write manifest");
    for (auto& f : e.files) fs::remove(dir / f.name, ec);  // the downloads are no longer needed
    return true;
  };
  auto roformer = [data, skel](const fs::path& dir, const CatalogueEntry& e, std::string* err) -> bool {
    if (e.files.size() != 1) return fail(err, "not a single-checkpoint RoFormer entry");
    std::error_code ec;
    for (const char* n : {"roformer.onnx", "roformer.layout.json"}) {
      fs::copy_file(skel / n, dir / n, fs::copy_options::overwrite_existing, ec);
      if (ec) return fail(err, "skeleton missing: " + (skel / n).string());
    }
    if (!convert_checkpoint(dir / e.files[0].name, skel / "roformer.layout.json", dir / "roformer.weights", nullptr, err)) return false;
    std::string sk;
    if (!sha256_file(dir / "roformer.onnx", sk)) return fail(err, "cannot hash the skeleton");
    std::ostringstream m;
    m << "{\"format\":\"deckhand-stems-model/1\",\"id\":\"" << e.id << "\",\"name\":\"" << e.name << "\",\"file\":\"" << e.id << "/roformer.onnx\","
      << "\"licence\":\"" << e.weights_licence << "\",\"source_url\":\"" << e.files[0].url << "\",\"sha256\":\"" << sk << "\","
      << "\"sample_rate\":44100,\"segment_frames\":343980,\"overlap\":0.25,\"channels\":2,\"pipeline\":\"roformer-vocals\","
      << "\"stems\":[\"vocals\",\"drums\",\"bass\",\"other\"]}\n";
    fs::path mf = models_dir(data) / (e.id + ".dhstems.json"), part = mf;
    part += ".part";
    { std::ofstream f(part, std::ios::trunc); f << m.str(); if (!f) return fail(err, "cannot write manifest"); }
    fs::rename(part, mf, ec);
    if (ec) return fail(err, "cannot write manifest");
    fs::remove(dir / e.files[0].name, ec);
    return true;
  };
  // A recipe downloads nothing: it only records which two installed models it combines.
  auto recipe = [data](const fs::path&, const CatalogueEntry& e, std::string* err) -> bool {
    if (e.requires_ids.size() != 2) return fail(err, "a recipe names exactly two models");
    for (auto& id : e.requires_ids) {
      ModelManifest mm;
      if (!load_manifest(models_dir(data) / (id + ".dhstems.json"), mm, nullptr)) return fail(err, "this recipe needs \"" + id + "\" installed first");
    }
    std::ostringstream m;
    m << "{\"format\":\"deckhand-stems-model/1\",\"id\":\"" << e.id << "\",\"name\":\"" << e.name << "\","
      << "\"licence\":\"" << e.weights_licence << "\",\"source_url\":\"" << e.upstream_url << "\","
      << "\"sample_rate\":44100,\"segment_frames\":343980,\"overlap\":0.25,\"channels\":2,\"pipeline\":\"recipe-vocals-then-demucs\","
      << "\"parts\":[\"" << e.requires_ids[0] << "\",\"" << e.requires_ids[1] << "\"],\"stems\":[\"vocals\",\"drums\",\"bass\",\"other\"]}\n";
    fs::path mf = models_dir(data) / (e.id + ".dhstems.json"), part = mf;
    part += ".part";
    std::error_code ec;
    { std::ofstream f(part, std::ios::trunc); f << m.str(); if (!f) return fail(err, "cannot write manifest"); }
    fs::rename(part, mf, ec);
    return ec ? fail(err, "cannot write manifest") : true;
  };
  return [demucs, roformer, recipe](const fs::path& dir, const CatalogueEntry& e, std::string* err) -> bool {
    if (e.kind == "demucs") return demucs(dir, e, err);
    if (e.kind == "roformer") return roformer(dir, e, err);
    if (e.kind == "recipe") return recipe(dir, e, err);
    return fail(err, "unknown model kind \"" + e.kind + "\"");
  };
}

std::vector<InstalledModel> installed_models(const fs::path& data) {
  std::vector<InstalledModel> v;
  std::error_code ec;
  const fs::path md = models_dir(data);
  for (auto& d : fs::directory_iterator(md, ec)) {
    if (!d.is_directory(ec) || !fs::exists(d.path() / "install.json", ec)) continue;
    InstalledModel m;
    m.id = d.path().filename().string();
    m.dir = d.path();
    std::ifstream f(d.path() / "install.json");
    std::ostringstream ss;
    ss << f.rdbuf();
    Json j;
    if (detail::parse_json(ss.str(), j, nullptr)) { m.name = sget(j, "name"); m.licence = sget(j, "licence"); m.licence_accepted_at = sget(j, "licence_accepted_at"); }
    for (auto& x : fs::recursive_directory_iterator(d.path(), ec)) if (x.is_regular_file(ec)) m.bytes += x.file_size(ec);
    ModelManifest mm;
    m.usable = load_manifest(md / (m.id + ".dhstems.json"), mm, nullptr);
    v.push_back(std::move(m));
  }
  std::sort(v.begin(), v.end(), [](const InstalledModel& a, const InstalledModel& b) { return a.id < b.id; });
  return v;
}

bool remove_model(const fs::path& data, const std::string& id) {
  if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.find("..") != std::string::npos) return false;
  const fs::path md = models_dir(data);
  std::error_code ec;
  bool any = false;
  any |= fs::remove_all(md / id, ec) > 0;
  any |= fs::remove(md / (id + ".dhstems.json"), ec);
  any |= fs::remove(md / (id + ".onnx"), ec);
  any |= fs::remove(md / (id + ".onnx.data"), ec);
  return any;
}

}  // namespace deckhand::stems
