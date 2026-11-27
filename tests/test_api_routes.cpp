// The REST routes of the effect chains, plug-ins, browser, relocation, trash and sync, driven through the real ApiBridge on
// a loopback port the system picks (the library setting api.port = 0; never the shipped port), with an offline engine and a
// stub host: no audio or MIDI device, no sound. Every operation is called with its success path, bad input and a token whose
// scope does not cover it; the scope each operation needs and the status codes and body types it may answer are read from
// docs/openapi.yaml, so the document and the code cannot drift apart unnoticed.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/api/auth.hpp"
#include "deckhand/api/json.hpp"
#include "deckhand/api/test_client.hpp"
#include "deckhand/app/api_bridge.hpp"
#include "capture_synth.hpp"
#include "deckhand/app/capture_service.hpp"
#include "deckhand/app/chain_presets.hpp"
#include "deckhand/app/plugin_service.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/plugin_host.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/plugin.hpp"

using namespace deckhand;
using deckhand::api::Json;
namespace fs = std::filesystem;

namespace {

// ---- the stub host ------------------------------------------------------------------------------------------------------
class StubHost : public ApiHost {
 public:
  Engine engine;
  int library_changed = 0, browser_changed = 0, sync_runs = 0;
  std::unique_ptr<CaptureService> capture;
  StubHost() { engine.configure_offline(48000, 2); }
  void init_capture(const std::string& folder) {  // recording from the engine's offline input: no device is ever opened
    engine.configure_offline_input(44100);
    CaptureService::Options o;
    o.open_device = false;
    capture = std::make_unique<CaptureService>(engine, folder, o);
  }
  CaptureService* api_capture() override { return capture.get(); }
  IEngine& api_engine() override { return engine; }
  bool api_load(int, std::int64_t, std::string& e) override { e = "stub"; return false; }
  void api_add_file(const std::string&, int) override {}
  bool api_export(const std::string&, std::int64_t, std::string& e, bool, const std::string&) override { e = "stub"; return false; }
  void api_library_changed() override { ++library_changed; }
  bool api_record(bool, std::string&, std::string& e) override { e = "stub"; return false; }
  void api_load_sample(int, const std::string&) override {}
  bool api_prep(int, const std::string&, const PrepArgs&, std::string& e) override { e = "stub"; return false; }
  std::string api_deck_prep_json(int) override { return {}; }
  bool api_preview_load(std::int64_t, bool, std::string& e) override { e = "stub"; return false; }
  std::string api_preview_title() override { return {}; }
  void api_browser_changed() override { ++browser_changed; }
  void api_sync_run() override { ++sync_runs; }
  std::string api_sync_json() override { return "{\"state\":\"idle\",\"runs\":" + std::to_string(sync_runs) + "}"; }
  std::string api_status() override { return "{}"; }
  std::string api_audio_device() override { return "{}"; }
};

// ---- a blocking HTTP client over the test socket --------------------------------------------------------------------------
struct Http {
  int status = 0;
  std::string body;
  Json json() const { return body.empty() ? Json() : Json::parse(body); }
};

std::uint16_t g_port = 0;

Http call(const std::string& method, const std::string& target, const std::string& token, const std::string& body = "") {
  api::testing::TestClient c;
  std::string err;
  CHECK(c.connect("127.0.0.1", g_port, false, &err));
  std::string r = method + " " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
  if (!token.empty()) r += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") r += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  r += "\r\n" + body;
  CHECK(c.send(r));
  std::string buf;
  char tmp[8192];
  std::size_t need = std::string::npos;
  Http h;
  for (;;) {
    const long n = c.recv(tmp, sizeof tmp, 4000);
    if (n > 0) buf.append(tmp, static_cast<std::size_t>(n));
    const auto p = buf.find("\r\n\r\n");
    if (p != std::string::npos) {
      if (need == std::string::npos) {
        h.status = std::atoi(buf.c_str() + 9);
        std::string head = buf.substr(0, p);
        for (char& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto cl = head.find("content-length:");
        need = p + 4 + (cl == std::string::npos ? 0 : static_cast<std::size_t>(std::atol(head.c_str() + cl + 15)));
      }
      if (buf.size() >= need) {
        h.body = buf.substr(p + 4, need - p - 4);
        return h;
      }
    }
    CHECK(n > 0);
  }
}

// ---- what docs/openapi.yaml says about an operation -----------------------------------------------------------------------
struct SpecOp {
  std::string scope;                   // x-scope
  std::map<int, std::string> codes;    // documented status -> "object" / "array" / "" (no body or not stated)
  bool has_default = false;
};
std::map<std::string, SpecOp> g_spec;  // "METHOD /path"

std::string indent_trim(const std::string& l, std::size_t& indent) {
  indent = l.find_first_not_of(' ');
  return indent == std::string::npos ? std::string() : l.substr(indent);
}

void load_spec() {
  std::ifstream in("docs/openapi.yaml");
  CHECK(in.good());
  std::string line, path, key;
  bool in_paths = false;
  int code = -2;  // -2 none, -1 default
  std::size_t code_indent = 0;
  while (std::getline(in, line)) {
    if (line.rfind("paths:", 0) == 0) { in_paths = true; continue; }
    if (!in_paths) continue;
    if (!line.empty() && line[0] != ' ' && line[0] != '#') break;
    std::size_t ind = 0;
    const std::string t = indent_trim(line, ind);
    if (t.empty()) continue;
    if (ind == 2 && t[0] == '/') { path = t.substr(0, t.find(':')); key.clear(); continue; }
    if (ind == 4 && t.back() == ':') {
      const std::string m = t.substr(0, t.size() - 1);
      if (m == "get" || m == "put" || m == "post" || m == "delete" || m == "patch") {
        std::string up = m;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        key = up + " " + path;
        g_spec[key];
        code = -2;
      } else {
        key.clear();
      }
      continue;
    }
    if (key.empty()) continue;
    SpecOp& op = g_spec[key];
    if (t.rfind("x-scope:", 0) == 0) {
      std::string v = t.substr(8);
      v.erase(0, v.find_first_not_of(' '));
      if (!v.empty() && v.front() == '"') v = v.substr(1, v.rfind('"') - 1);
      op.scope = v;
      continue;
    }
    if (ind == 8 && (t[0] == '"' || t.rfind("default:", 0) == 0)) {
      code = t[0] == '"' ? std::atoi(t.c_str() + 1) : -1;
      code_indent = ind;
      if (code == -1) op.has_default = true;
      else op.codes[code];
    }
    if (code >= 0 && ind >= code_indent) {
      std::string& ty = op.codes[code];
      if (ty.empty()) {
        if (t.find("type: object") != std::string::npos) ty = "object";
        else if (t.find("type: array") != std::string::npos) ty = "array";
      }
    }
  }
  CHECK(g_spec.size() > 100);
}

// ---- the table of operations under test ---------------------------------------------------------------------------------
std::set<std::string> g_called;  // "METHOD /path" patterns exercised

struct Tokens {
  std::string admin, reader, nothing;
  std::map<std::string, std::string> by_scope;
};
Tokens g_tok;

std::string token_for(const std::string& scope) {
  const auto it = g_tok.by_scope.find(scope);
  if (it != g_tok.by_scope.end()) return it->second;
  Json b = Json::object();
  b.set("name", scope).set("scopes", Json::array().push_back(scope));
  const Http h = call("POST", "/v1/tokens", g_tok.admin, b.dump());
  CHECK(h.status == 201);
  return g_tok.by_scope[scope] = h.json()["token"].as_string();
}

// One call of an operation. `pattern` is the OpenAPI path ("/v1/browser/panes/{slot}"), `path` the concrete one. The right
// scope (from the spec) lets it through, a token with another scope gets 403, and the answer's status and body type are the
// documented ones. Returns the answer of the call made with the right scope.
Http op(const std::string& method, const std::string& pattern, const std::string& path, const std::string& body, int want, const char* target = "deck1") {
  const std::string key = method + " " + pattern;
  g_called.insert(key);
  const auto it = g_spec.find(key);
  CHECK(it != g_spec.end());
  std::string scope = it->second.scope;
  CHECK(!scope.empty());
  for (std::size_t p; (p = scope.find("{target}")) != std::string::npos;) scope.replace(p, 8, target);
  const Http denied = call(method, path, g_tok.nothing, body);
  if (denied.status != 403) std::fprintf(stderr, "%s %s with the wrong scope answered %d (%s)\n", method.c_str(), path.c_str(), denied.status, denied.body.c_str());
  CHECK(denied.status == 403);
  if (scope != "admin") {  // a token for just this scope is enough; admin is the top scope
    const Http mid = call(method, path, token_for(scope), body);
    if (mid.status != want) std::fprintf(stderr, "%s %s with scope %s answered %d, wanted %d (%s)\n", method.c_str(), path.c_str(), scope.c_str(), mid.status, want, mid.body.c_str());
    CHECK(mid.status == want);
    return mid;  // callers that need a second identical effect use the admin token themselves
  }
  const Http h = call(method, path, g_tok.admin, body);
  if (h.status != want) std::fprintf(stderr, "%s %s answered %d, wanted %d (%s)\n", method.c_str(), path.c_str(), h.status, want, h.body.c_str());
  CHECK(h.status == want);
  // shape: documented status and body type
  const SpecOp& so = it->second;
  const auto c = so.codes.find(h.status);
  if (c == so.codes.end() && !(h.status >= 400 && so.has_default)) std::fprintf(stderr, "%s answered undocumented status %d\n", key.c_str(), h.status);
  CHECK(c != so.codes.end() || (h.status >= 400 && so.has_default));
  return h;
}

// op() plus the shape rules for the answer of the successful call.
Http ok(const std::string& method, const std::string& pattern, const std::string& path, const std::string& body, int want, const char* target = "deck1") {
  Http h = op(method, pattern, path, body, want, target);
  const SpecOp& so = g_spec[method + " " + pattern];
  const auto c = so.codes.find(h.status);
  const bool documented = c != so.codes.end() || (h.status >= 400 && so.has_default);
  if (!documented) std::fprintf(stderr, "%s %s: status %d is not documented\n", method.c_str(), pattern.c_str(), h.status);
  CHECK(documented);
  if (c == so.codes.end()) {
    CHECK(h.json().is_object());  // an error answer
  } else if (h.status == 204) {
    CHECK(h.body.empty());
  } else if (c->second == "object") {
    if (!h.json().is_object()) std::fprintf(stderr, "%s %s: the answer is not an object (%s)\n", method.c_str(), pattern.c_str(), h.body.c_str());
    CHECK(h.json().is_object());
  } else if (c->second == "array") {
    CHECK(h.json().is_array());
  }
  return h;
}

// A failing call: status as expected, an error object, and the status documented (directly or through `default`).
void fails(const std::string& method, const std::string& pattern, const std::string& path, const std::string& body, int want, const char* target = "deck1") {
  const std::string key = method + " " + pattern;
  const auto it = g_spec.find(key);
  CHECK(it != g_spec.end());
  std::string scope = it->second.scope;
  for (std::size_t p; (p = scope.find("{target}")) != std::string::npos;) scope.replace(p, 8, target);
  const Http h = call(method, path, token_for(scope), body);
  if (h.status != want) std::fprintf(stderr, "%s %s answered %d, wanted %d (%s)\n", method.c_str(), path.c_str(), h.status, want, h.body.c_str());
  CHECK(h.status == want);
  if (!h.json().is_object()) std::fprintf(stderr, "%s %s: the error answer is not an object (%s)\n", method.c_str(), path.c_str(), h.body.c_str());
  CHECK(h.json().is_object());
  CHECK(it->second.has_default || it->second.codes.count(h.status));
}

// ---- library fixture ------------------------------------------------------------------------------------------------------
struct Fixture {
  fs::path dir, old_dir, new_dir, keep_dir, sync_dir;
  std::int64_t a = 0, b = 0, c = 0, d = 0, e = 0, gone = 0, folder = 0, list = 0;
};

void put(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << content;
}
std::int64_t add(Library& lib, const fs::path& p, const char* title) {
  NewTrack t;
  t.path = p.string();
  t.title = title;
  t.artist = "T";
  t.duration_ms = 1000;
  return lib.upsert_track(t);
}

bool has_key(const Json& j, const char* k) { return j.is_object() && j.contains(k); }

// ---- effect chains ----------------------------------------------------------------------------------------------------------
void test_chains(StubHost& host, const fs::path& dir) {
  chain_presets_set_dir((dir / "fx-presets").string());
  // plug-ins: the list answers with its shape; a rescan with no plug-in folder set does nothing (a scan would instantiate
  // plug-ins) and answers 202 where plug-ins are hosted, 409 elsewhere
  Http h = ok("GET", "/v1/fx/plugins", "/v1/fx/plugins", "", 200);
  for (const char* k : {"supported", "scanning", "progress", "plugins", "last_message"}) CHECK(has_key(h.json(), k));
  CHECK(h.json()["plugins"].is_array());
  const bool supported = h.json()["supported"].as_bool();
  ok("POST", "/v1/fx/plugins/rescan", "/v1/fx/plugins/rescan", "", supported ? 202 : 409);
  plugin_service().wait();

  // a chain: insert, edit, move, preset round trip, remove
  fails("GET", "/v1/fx/chains/{target}/units/{id}/plugin", "/v1/fx/chains/deck1/units/1/plugin", "", 404);  // nothing there yet
  fails("GET", "/v1/fx/chains/{target}/units/{id}/plugin", "/v1/fx/chains/nowhere/units/1/plugin", "", 404);
  h = ok("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck1/units", "{\"effect\":\"tape_echo\"}", 201);
  const int u1 = static_cast<int>(h.json()["id"].as_int());
  CHECK(u1 > 0 && h.json()["count"].as_int() == 1);
  h = call("POST", "/v1/fx/chains/deck1/units", g_tok.admin, "{\"effect\":\"beat_reverb\",\"on\":true}");
  CHECK(h.status == 201);
  const int u2 = static_cast<int>(h.json()["id"].as_int());
  CHECK(u2 > 0 && u2 != u1 && h.json()["units"].size() == 2 && h.json()["units"].at(1)["on"].as_bool());
  fails("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck1/units", "{}", 400);
  fails("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck1/units", "{\"effect\":\"no_such_effect\"}", 400);
  fails("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck1/units", "{\"effect\":\"tape_echo\",\"plugin\":\"x\"}", 400);
  fails("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck1/units", "{\"plugin\":\"au:aufx:nope:nope\"}", 409);  // not a known plug-in
  fails("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/nowhere/units", "{\"effect\":\"tape_echo\"}", 404, "nowhere");
  // a token for deck1 only does not reach deck2
  {
    const Http a = call("POST", "/v1/fx/chains/deck2/units", token_for("control:fx:deck1"), "{\"effect\":\"tape_echo\"}");
    CHECK(a.status == 403);
    const Http r = call("POST", "/v1/fx/chains/deck2/units", g_tok.reader, "{\"effect\":\"tape_echo\"}");
    CHECK(r.status == 403);
  }
  fails("GET", "/v1/fx/chains/{target}/units/{id}/plugin", "/v1/fx/chains/deck1/units/" + std::to_string(u1) + "/plugin", "", 409);  // not a plug-in
  ok("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u1), "{\"wet\":0.25,\"param\":0.5,\"on\":true}", 200);
  CHECK(host.engine.fx_chain(0).unit[0].wet > 0.24f && host.engine.fx_chain(0).unit[0].wet < 0.26f);
  fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u1), "{\"wet\":2}", 400);
  fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u1), "{\"on\":\"yes\"}", 400);
  fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u1), "{\"params\":[1]}", 400);
  fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u1), "{\"effect\":\"nope\"}", 400);
  fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/999", "{\"wet\":0.5}", 404);
  ok("POST", "/v1/fx/chains/{target}/units/{id}/move", "/v1/fx/chains/deck1/units/" + std::to_string(u2) + "/move", "{\"pos\":0}", 200);
  CHECK(host.engine.fx_chain(0).unit[0].id == u2);
  fails("POST", "/v1/fx/chains/{target}/units/{id}/move", "/v1/fx/chains/deck1/units/" + std::to_string(u2) + "/move", "{\"pos\":-1}", 400);
  fails("POST", "/v1/fx/chains/{target}/units/{id}/move", "/v1/fx/chains/deck1/units/" + std::to_string(u2) + "/move", "{}", 400);
  fails("POST", "/v1/fx/chains/{target}/units/{id}/move", "/v1/fx/chains/deck1/units/999/move", "{\"pos\":0}", 404);
  ok("PUT", "/v1/fx/chains/{target}/settings", "/v1/fx/chains/deck1/settings", "{\"bypass\":true,\"wet\":0.5}", 200);
  CHECK(host.engine.fx_chain(0).bypass != 0);
  fails("PUT", "/v1/fx/chains/{target}/settings", "/v1/fx/chains/deck1/settings", "{}", 400);
  fails("PUT", "/v1/fx/chains/{target}/settings", "/v1/fx/chains/deck1/settings", "{\"wet\":-1}", 400);
  fails("PUT", "/v1/fx/chains/{target}/settings", "/v1/fx/chains/deck1/settings", "{\"bypass\":1}", 400);
  h = ok("GET", "/v1/fx/chains/{target}", "/v1/fx/chains/deck1", "", 200);
  for (const char* k : {"target", "exists", "bypass", "wet", "count", "max_units", "rev", "units"}) CHECK(has_key(h.json(), k));
  for (std::size_t i = 0; i < h.json()["units"].size(); ++i)
    for (const char* k : {"id", "pos", "effect", "number", "name", "on", "wet", "param", "param2", "param3", "beats", "seconds"}) CHECK(has_key(h.json()["units"].at(i), k));
  fails("GET", "/v1/fx/chains/{target}", "/v1/fx/chains/deck9", "", 404);
  h = ok("GET", "/v1/fx/chains/{target}/preset", "/v1/fx/chains/deck1/preset", "", 200);
  const std::string chain_preset = h.body;
  ok("PUT", "/v1/fx/chains/{target}/preset", "/v1/fx/chains/deck2/preset", chain_preset, 200, "deck2");
  CHECK(host.engine.fx_chain(1).count == 2);
  fails("PUT", "/v1/fx/chains/{target}/preset", "/v1/fx/chains/deck2/preset", "[1]", 400, "deck2");
  fails("PUT", "/v1/fx/chains/{target}/preset", "/v1/fx/chains/deck2/preset", "{\"nonsense\":true}", 400, "deck2");

  // named presets
  h = ok("GET", "/v1/fx/presets", "/v1/fx/presets", "", 200);
  CHECK(h.json()["presets"].is_array() && h.json()["presets"].size() == 0);
  h = ok("POST", "/v1/fx/chains/{target}/presets", "/v1/fx/chains/deck1/presets", "{\"name\":\"  Big/Echo \"}", 201);
  CHECK(h.json()["name"].as_string() == "BigEcho");
  fails("POST", "/v1/fx/chains/{target}/presets", "/v1/fx/chains/deck1/presets", "{}", 400);
  fails("POST", "/v1/fx/chains/{target}/presets", "/v1/fx/chains/deck1/presets", "{\"name\":\"..\"}", 400);
  fails("POST", "/v1/fx/chains/{target}/presets", "/v1/fx/chains/nowhere/presets", "{\"name\":\"x\"}", 404, "nowhere");
  h = call("GET", "/v1/fx/presets", g_tok.reader);
  CHECK(h.status == 200 && h.json()["presets"].size() == 1 && h.json()["presets"].at(0).as_string() == "BigEcho");
  h = ok("POST", "/v1/fx/chains/{target}/presets/load", "/v1/fx/chains/deck3/presets/load", "{\"name\":\"BigEcho\"}", 200, "deck3");
  CHECK(has_key(h.json(), "notes") && has_key(h.json(), "units") && h.json()["count"].as_int() == 2);
  CHECK(host.engine.fx_chain(2).count == 2);
  fails("POST", "/v1/fx/chains/{target}/presets/load", "/v1/fx/chains/deck3/presets/load", "{\"name\":\"nope\"}", 404, "deck3");
  fails("POST", "/v1/fx/chains/{target}/presets/load", "/v1/fx/chains/deck3/presets/load", "{}", 400, "deck3");
  fails("POST", "/v1/fx/chains/{target}/presets/load", "/v1/fx/chains/deck3/presets/load", "{\"name\":\"../../etc/passwd\"}", 404, "deck3");
  ok("POST", "/v1/fx/presets/delete", "/v1/fx/presets/delete", "{\"name\":\"BigEcho\"}", 204);
  fails("POST", "/v1/fx/presets/delete", "/v1/fx/presets/delete", "{\"name\":\"BigEcho\"}", 404);
  fails("POST", "/v1/fx/presets/delete", "/v1/fx/presets/delete", "{}", 404);

  ok("DELETE", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u2), "", 204);
  CHECK(host.engine.fx_chain(0).count == 1);
  fails("DELETE", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck1/units/" + std::to_string(u2), "", 404);

#if defined(__APPLE__) && !defined(DECKHAND_PLUGINS_DISABLED)
  // A plug-in unit, with Apple's own delay only (never a third-party plug-in): the catalog is scanned for metadata and
  // nothing but that one unit is probed. The service is pointed at a folder only now, after the rescan above.
  {
    const std::string pdir = (dir / "plugins").string();
    fs::create_directories(pdir);
    plugins::PluginCatalog cat(pdir + "/plugins.cache");
    cat.scan();
    plugin_service().set_dir(pdir);
    plugin_host_set_probe_exe(DH_PLUGIN_PROBE_EXE);
    std::string delay;
    for (const auto& p : plugins::scan_installed())
      if (p.type == "aufx" && p.subtype == "dely" && p.manufacturer == "appl") delay = p.id;
    if (!delay.empty() && plugin_service().status().supported) {
      std::string err;
      CHECK(plugin_service().vet(delay, err));
      h = ok("GET", "/v1/fx/plugins", "/v1/fx/plugins", "", 200);
      bool seen = false;
      const Json pj = h.json();
      for (std::size_t i = 0; i < pj["plugins"].size(); ++i) {
        const Json& p = pj["plugins"].at(i);
        for (const char* k : {"id", "name", "maker", "format", "vetted", "blocked"}) CHECK(has_key(p, k));
        if (p["id"].as_string() == delay) seen = p["vetted"].as_bool();
      }
      CHECK(seen);
      Json body = Json::object();
      body.set("plugin", delay);
      h = ok("POST", "/v1/fx/chains/{target}/units", "/v1/fx/chains/deck4/units", body.dump(), 201, "deck4");
      const int pid = static_cast<int>(h.json()["id"].as_int());
      h = ok("GET", "/v1/fx/chains/{target}/units/{id}/plugin", "/v1/fx/chains/deck4/units/" + std::to_string(pid) + "/plugin", "", 200, "deck4");
      CHECK(h.json()["loaded"].as_bool() && h.json()["params"].is_array() && h.json()["params"].size() > 0);
      for (const char* k : {"id", "name", "maker"}) CHECK(has_key(h.json(), k));
      ok("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck4/units/" + std::to_string(pid), "{\"params\":{\"0\":0.25}}", 200, "deck4");
      fails("PATCH", "/v1/fx/chains/{target}/units/{id}", "/v1/fx/chains/deck4/units/" + std::to_string(pid), "{\"params\":{\"0\":2}}", 400, "deck4");
      plugin_service().wait();
    }
  }
#else
  std::printf("plug-ins not supported here: skipping the plug-in unit part\n");
#endif
}

// ---- browser, folders, relocation, trash ---------------------------------------------------------------------------------------
void test_browser(StubHost& host, const Fixture& fx) {
  Http h = ok("GET", "/v1/browser", "/v1/browser", "", 200);
  for (const char* k : {"split", "live_pane", "panes", "active_layout", "layouts", "available_columns"}) CHECK(has_key(h.json(), k));
  CHECK(h.json()["split"].as_string() == "off" && h.json()["panes"].size() == 2 && h.json()["available_columns"].size() > 3);
  std::string builtin;
  const Json bj = h.json();
  for (std::size_t i = 0; i < bj["layouts"].size(); ++i) {
    const Json& l = bj["layouts"].at(i);
    for (const char* k : {"name", "builtin", "columns", "widths", "sort", "descending"}) CHECK(has_key(l, k));
    if (l["builtin"].as_bool() && builtin.empty()) builtin = l["name"].as_string();
  }
  CHECK(!builtin.empty());
  for (std::size_t i = 0; i < h.json()["panes"].size(); ++i)
    for (const char* k : {"kind", "id", "path", "sort", "descending", "search"}) CHECK(has_key(h.json()["panes"].at(i), k));

  const int changed0 = host.browser_changed;
  h = ok("PUT", "/v1/browser/split", "/v1/browser/split", "{\"mode\":\"side\",\"live_pane\":1}", 200);
  CHECK(h.json()["split"].as_string() == "side" && h.json()["live_pane"].as_int() == 1 && host.browser_changed > changed0);
  fails("PUT", "/v1/browser/split", "/v1/browser/split", "{\"mode\":\"diagonal\"}", 400);
  fails("PUT", "/v1/browser/split", "/v1/browser/split", "{}", 400);
  fails("PUT", "/v1/browser/split", "/v1/browser/split", "{\"mode\":\"stacked\",\"live_pane\":2}", 400);
  fails("PUT", "/v1/browser/split", "/v1/browser/split", "{\"mode\":\"stacked\",\"live_pane\":\"1\"}", 400);
  CHECK(call("GET", "/v1/browser", g_tok.reader).json()["split"].as_string() == "side");  // refused calls changed nothing

  Json pb = Json::object();
  pb.set("kind", "playlist").set("id", fx.list).set("sort", "title").set("descending", true).set("search", "x");
  h = ok("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", pb.dump(), 200);
  CHECK(h.json()["panes"].at(0)["kind"].as_string() == "playlist" && h.json()["panes"].at(0)["id"].as_int() == fx.list);
  CHECK(h.json()["panes"].at(0)["sort"].as_string() == "title" && h.json()["panes"].at(0)["descending"].as_bool() && h.json()["panes"].at(0)["search"].as_string() == "x");
  h = ok("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/1", "{\"kind\":\"disk\",\"path\":\"" + fx.new_dir.generic_string() + "\"}", 200);
  CHECK(h.json()["panes"].at(1)["kind"].as_string() == "disk");
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/2", "{}", 404);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/x", "{}", 404);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", "{\"kind\":\"history_header\"}", 400);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", "{\"kind\":\"nonsense\"}", 400);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", "{\"id\":\"3\"}", 400);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", "{\"path\":\"a|b\"}", 400);
  fails("PUT", "/v1/browser/panes/{slot}", "/v1/browser/panes/0", "{\"sort\":\"sideways\"}", 400);
  CHECK(call("GET", "/v1/browser", g_tok.reader).json()["panes"].at(0)["sort"].as_string() == "title");
  ok("PUT", "/v1/browser/split", "/v1/browser/split", "{\"mode\":\"off\"}", 200);
  h = call("GET", "/v1/browser", g_tok.reader);
  CHECK(h.json()["split"].as_string() == "off" && h.json()["live_pane"].as_int() == 0);

  h = ok("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"name\":\"Tags\",\"columns\":[\"title\",\"artist\"],\"widths\":{\"title\":200},\"sort\":\"title\",\"descending\":true}", 201);
  bool found = false;
  for (std::size_t i = 0; i < h.json()["layouts"].size(); ++i)
    if (h.json()["layouts"].at(i)["name"].as_string() == "Tags") {
      found = true;
      CHECK(!h.json()["layouts"].at(i)["builtin"].as_bool() && h.json()["layouts"].at(i)["columns"].size() == 2 && h.json()["layouts"].at(i)["descending"].as_bool());
    }
  CHECK(found);
  fails("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"name\":\"" + builtin + "\",\"columns\":[\"title\"]}", 409);
  fails("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"name\":\"\",\"columns\":[\"title\"]}", 409);
  fails("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"name\":\"X\",\"columns\":[]}", 400);
  fails("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"name\":\"X\",\"columns\":[\"nonsense\"]}", 400);
  fails("POST", "/v1/browser/layouts", "/v1/browser/layouts", "{\"columns\":[\"title\"]}", 400);
  h = ok("PUT", "/v1/browser/layout", "/v1/browser/layout", "{\"name\":\"Tags\"}", 200);
  CHECK(h.json()["active_layout"].as_string() == "Tags");
  CHECK(h.json()["panes"].at(h.json()["live_pane"].as_int())["sort"].as_string() == "title");
  fails("PUT", "/v1/browser/layout", "/v1/browser/layout", "{\"name\":\"No such\"}", 404);
  fails("PUT", "/v1/browser/layout", "/v1/browser/layout", "{}", 404);
  ok("PUT", "/v1/browser/layout", "/v1/browser/layout", "{\"name\":\"" + builtin + "\"}", 200);
  ok("POST", "/v1/browser/layouts/delete", "/v1/browser/layouts/delete", "{\"name\":\"Tags\"}", 200);
  fails("POST", "/v1/browser/layouts/delete", "/v1/browser/layouts/delete", "{\"name\":\"Tags\"}", 404);
  fails("POST", "/v1/browser/layouts/delete", "/v1/browser/layouts/delete", "{\"name\":\"" + builtin + "\"}", 404);
  fails("POST", "/v1/browser/layouts/delete", "/v1/browser/layouts/delete", "{}", 404);
}

void test_folders(const Fixture& fx) {
  // playlist folder: its playlist's track, once, with the playlist's name as its source
  Http h = ok("GET", "/v1/library/folders/{id}/tracks", "/v1/library/folders/" + std::to_string(fx.folder) + "/tracks", "", 200);
  CHECK(h.json()["total"].as_int() == 1 && h.json()["offset"].as_int() == 0 && h.json()["items"].size() == 1);
  CHECK(h.json()["items"].at(0)["id"].as_int() == fx.e && has_key(h.json()["items"].at(0), "source") && has_key(h.json()["items"].at(0), "title"));
  h = call("GET", "/v1/library/folders/" + std::to_string(fx.folder) + "/tracks?search=zzzz&limit=5&offset=0&sort=title&desc=1", token_for("library:read"));
  CHECK(h.status == 200 && h.json()["total"].as_int() == 0 && h.json()["items"].size() == 0);
  fails("GET", "/v1/library/folders/{id}/tracks", "/v1/library/folders/x/tracks", "", 404);

  h = ok("GET", "/v1/library/disk-folders", "/v1/library/disk-folders", "", 200);
  CHECK(h.json()["folders"].is_array() && h.json()["folders"].size() >= 1);
  std::string dir;
  std::function<void(const Json&)> walk = [&](const Json& arr) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
      const Json& f = arr.at(i);
      for (const char* k : {"path", "name", "tracks", "total", "has_children"}) CHECK(has_key(f, k));
      if (f["name"].as_string() == "keep") dir = f["path"].as_string();
    }
  };
  walk(h.json()["folders"]);
  // the chain of directories above the music is skipped, so keep/ and new/ are among the first level or one below
  if (dir.empty()) {
    for (std::size_t i = 0; i < h.json()["folders"].size() && dir.empty(); ++i) {
      const Http sub = call("GET", "/v1/library/disk-folders?dir=" + h.json()["folders"].at(i)["path"].as_string(), token_for("library:read"));
      CHECK(sub.status == 200);
      walk(sub.json()["folders"]);
    }
  }
  CHECK(!dir.empty());
  h = ok("GET", "/v1/library/disk-folders/tracks", "/v1/library/disk-folders/tracks?dir=" + dir, "", 200);
  CHECK(h.json()["total"].as_int() == 1 && h.json()["items"].at(0)["id"].as_int() == fx.e);
  h = call("GET", "/v1/library/disk-folders/tracks?dir=" + dir + "&recursive=0&search=nomatch&limit=1", token_for("library:read"));
  CHECK(h.status == 200 && h.json()["total"].as_int() == 0);
  fails("GET", "/v1/library/disk-folders/tracks", "/v1/library/disk-folders/tracks", "", 400);
  h = call("GET", "/v1/library/disk-folders/tracks?dir=" + dir, g_tok.nothing);
  CHECK(h.status == 403);
  CHECK(call("GET", "/v1/library/disk-folders?dir=" + dir, token_for("library:read")).json()["folders"].is_array());
}

void test_relocation(StubHost& host, const Fixture& fx) {
  // A, B, C and D moved from old/ to new/; E did not move
  Json scan = Json::object();
  scan.set("folders", Json::array().push_back(fx.new_dir.string()));
  Http h = ok("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", scan.dump(), 200);
  for (const char* k : {"missing", "scanned", "found", "ambiguous", "not_found"}) CHECK(has_key(h.json(), k));
  CHECK(h.json()["missing"].as_int() == 4 && h.json()["scanned"].as_int() >= 4 && h.json()["found"].size() == 4);
  for (std::size_t i = 0; i < h.json()["found"].size(); ++i)
    for (const char* k : {"track_id", "old_path", "new_path", "how", "weak"}) CHECK(has_key(h.json()["found"].at(i), k));
  const Json found = h.json()["found"];
  Json opts = scan;
  opts.set("duration_tolerance_ms", 100).set("allow_name_only", true);
  CHECK(call("POST", "/v1/library/relocate/scan", token_for("library:read"), opts.dump()).status == 200);
  fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", "{}", 400);
  fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", "{\"folders\":[]}", 400);
  fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", "{\"folders\":[\"relative/dir\"]}", 400);
  fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", "{\"folders\":[\"" + (fx.dir / "no-such-dir").generic_string() + "\"]}", 400);
  fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", "{\"folders\":[\"" + (fx.new_dir / "b.mp3").generic_string() + "\"]}", 400);  // a file, not a directory
  {
    Json many = Json::array();
    for (int i = 0; i < 17; ++i) many.push_back(fx.new_dir.string());
    Json b = Json::object();
    b.set("folders", many);
    fails("POST", "/v1/library/relocate/scan", "/v1/library/relocate/scan", b.dump(), 400);
  }

  // the siblings of B when B is pointed at its new file: A, C and D sat in the same folder
  const std::string newb = (fx.new_dir / "b.mp3").string();
  Json sib = Json::object();
  sib.set("path", newb);
  h = ok("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/" + std::to_string(fx.b) + "/relink/siblings", sib.dump(), 200);
  for (const char* k : {"missing", "scanned", "found", "ambiguous", "not_found"}) CHECK(has_key(h.json(), k));
  CHECK(h.json()["found"].size() == 3);
  fails("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/" + std::to_string(fx.b) + "/relink/siblings", "{}", 400);
  fails("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/" + std::to_string(fx.b) + "/relink/siblings", "{\"path\":\"b.mp3\"}", 400);
  fails("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/" + std::to_string(fx.b) + "/relink/siblings", "{\"path\":\"" + (fx.new_dir / "nope.mp3").generic_string() + "\"}", 400);
  fails("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/987654/relink/siblings", sib.dump(), 404);
  fails("POST", "/v1/library/tracks/{id}/relink/siblings", "/v1/library/tracks/x/relink/siblings", sib.dump(), 404);

  // relink B by hand, once; another track cannot take the same file
  const int lib0 = host.library_changed;
  h = ok("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/" + std::to_string(fx.b) + "/relink", sib.dump(), 200);
  CHECK(h.json()["applied"].as_int() == 1 && h.json()["batch_id"].as_int() > 0 && host.library_changed > lib0);
  fails("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/" + std::to_string(fx.c) + "/relink", sib.dump(), 409);
  fails("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/" + std::to_string(fx.c) + "/relink", "{}", 400);
  fails("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/" + std::to_string(fx.c) + "/relink", "{\"path\":\"relative.mp3\"}", 400);
  fails("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/987654/relink", sib.dump(), 404);
  fails("POST", "/v1/library/tracks/{id}/relink", "/v1/library/tracks/x/relink", sib.dump(), 404);

  // apply what the scan found for the others: B is no longer missing and is skipped
  Json ap = Json::object();
  ap.set("matches", found);
  h = ok("POST", "/v1/library/relocate/apply", "/v1/library/relocate/apply", ap.dump(), 200);
  CHECK(h.json()["applied"].as_int() == 3 && h.json()["skipped"].as_int() == 1 && h.json()["batch_id"].as_int() > 0);
  h = call("POST", "/v1/library/relocate/scan", g_tok.admin, scan.dump());
  CHECK(h.status == 200 && h.json()["missing"].as_int() == 0);
  fails("POST", "/v1/library/relocate/apply", "/v1/library/relocate/apply", "{}", 400);
  fails("POST", "/v1/library/relocate/apply", "/v1/library/relocate/apply", "{\"matches\":[{\"track_id\":1}]}", 400);
  fails("POST", "/v1/library/relocate/apply", "/v1/library/relocate/apply", "{\"matches\":[{\"track_id\":\"1\",\"new_path\":\"/x\"}]}", 400);
  h = call("POST", "/v1/library/relocate/apply", token_for("library:write"), "{\"matches\":[]}");
  CHECK(h.status == 200 && h.json()["applied"].as_int() == 0);
  h = call("POST", "/v1/library/relocate/apply", token_for("library:write"), "{\"matches\":[{\"track_id\":987654,\"new_path\":\"" + newb + "\"}]}");
  CHECK(h.status == 200 && h.json()["applied"].as_int() == 0 && h.json()["skipped"].as_int() == 1);  // no such track: nothing changes
  CHECK(call("GET", "/v1/library/tracks/" + std::to_string(fx.a), token_for("library:read")).json()["path"].as_string() == (fx.new_dir / "a.mp3").string());
}

void test_trash(const Fixture& fx) {
  // one track is in the trash (removed just now): a purge of anything older than 30 days leaves it, a full purge takes it
  fails("DELETE", "/v1/library/trash", "/v1/library/trash?older_than_days=abc", "", 400);
  fails("DELETE", "/v1/library/trash", "/v1/library/trash?older_than_days=-1", "", 400);
  Http h = ok("DELETE", "/v1/library/trash", "/v1/library/trash?older_than_days=30", "", 200);
  CHECK(has_key(h.json(), "removed") && h.json()["removed"].as_int() == 0);
  h = call("GET", "/v1/library/trash", token_for("library:read"));
  CHECK(h.status == 200);
  h = ok("DELETE", "/v1/library/trash", "/v1/library/trash", "", 200);
  CHECK(h.json()["removed"].as_int() == 1);
  h = ok("DELETE", "/v1/library/trash", "/v1/library/trash", "", 200);
  CHECK(h.json()["removed"].as_int() == 0);
  (void)fx;
}


// ---- playlist trash ----------------------------------------------------------------------------------------------------------
void test_playlist_trash(const Fixture& fx) {
  Http h = ok("GET", "/v1/library/playlist-trash", "/v1/library/playlist-trash", "", 200);
  CHECK(h.json()["items"].is_array());
  // deleting the folder takes its list along: one entry, restorable
  CHECK(call("DELETE", "/v1/library/playlists/" + std::to_string(fx.folder), token_for("library:write")).status == 204);
  h = call("GET", "/v1/library/playlist-trash", token_for("library:read"));
  CHECK(h.status == 200 && h.json()["items"].size() == 1);
  const Json item = h.json()["items"].at(0);
  for (const char* k : {"id", "kind", "name", "old_id", "parent", "deleted_at", "playlists", "folders", "tracks", "shared"}) CHECK(has_key(item, k));
  CHECK(item["kind"].as_string() == "folder" && item["name"].as_string() == "Folder" && item["old_id"].as_int() == fx.folder && item["playlists"].as_int() == 1 && item["tracks"].as_int() == 1);
  const std::string id = std::to_string(item["id"].as_int());
  fails("POST", "/v1/library/playlist-trash/{id}/restore", "/v1/library/playlist-trash/987654/restore", "", 404);
  fails("POST", "/v1/library/playlist-trash/{id}/restore", "/v1/library/playlist-trash/abc/restore", "", 404);
  h = ok("POST", "/v1/library/playlist-trash/{id}/restore", "/v1/library/playlist-trash/" + id + "/restore", "", 200);
  CHECK(h.json()["id"].as_int() == fx.folder);
  CHECK(call("GET", "/v1/library/playlist-trash", token_for("library:read")).json()["items"].size() == 0);
  fails("POST", "/v1/library/playlist-trash/{id}/restore", "/v1/library/playlist-trash/" + id + "/restore", "", 404);  // gone from the trash
  // purge one, purge by age, purge all
  CHECK(call("DELETE", "/v1/library/playlists/" + std::to_string(fx.list), token_for("library:write")).status == 204);
  h = call("GET", "/v1/library/playlist-trash", token_for("library:read"));
  CHECK(h.json()["items"].size() == 1 && h.json()["items"].at(0)["kind"].as_string() == "playlist");
  const std::string id2 = std::to_string(h.json()["items"].at(0)["id"].as_int());
  fails("DELETE", "/v1/library/playlist-trash/{id}", "/v1/library/playlist-trash/987654", "", 404);
  ok("DELETE", "/v1/library/playlist-trash/{id}", "/v1/library/playlist-trash/" + id2, "", 204);
  CHECK(call("GET", "/v1/library/playlist-trash", token_for("library:read")).json()["items"].size() == 0);
  fails("DELETE", "/v1/library/playlist-trash", "/v1/library/playlist-trash?older_than_days=abc", "", 400);
  fails("DELETE", "/v1/library/playlist-trash", "/v1/library/playlist-trash?older_than_days=-1", "", 400);
  const std::int64_t fresh = call("POST", "/v1/library/playlists", token_for("library:write"), "{\"name\":\"Scratch\"}").json()["id"].as_int();
  CHECK(fresh > 0 && call("DELETE", "/v1/library/playlists/" + std::to_string(fresh), token_for("library:write")).status == 204);
  h = ok("DELETE", "/v1/library/playlist-trash", "/v1/library/playlist-trash?older_than_days=30", "", 200);
  CHECK(h.json()["removed"].as_int() == 0);  // deleted just now
  h = ok("DELETE", "/v1/library/playlist-trash", "/v1/library/playlist-trash", "", 200);
  CHECK(h.json()["removed"].as_int() == 1);
}


// ---- sequencer slices ----------------------------------------------------------------------------------------------------------
void test_sequencer_slice(StubHost& host) {
  float buf[512];
  const auto settle = [&] { host.engine.render(buf, 128); };
  ok("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"on\":true,\"slice\":5}", 200);
  settle();
  StepSeqView v = host.engine.step_seq();
  CHECK((v.pattern[0].cell[1][2] & 0x80) != 0 && v.pattern[0].slice[1][2] == 5);  // slice 5 (1-based) is stored as 5
  ok("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"on\":true}", 200);  // no slice: the cell keeps it
  settle();
  CHECK(host.engine.step_seq().pattern[0].slice[1][2] == 5);
  ok("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"on\":true,\"slice\":0}", 200);  // 0 = the whole sample
  settle();
  CHECK(host.engine.step_seq().pattern[0].slice[1][2] == 0);
  fails("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"slice\":17}", 400);
  fails("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"slice\":-1}", 400);
  fails("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"slice\":\"3\"}", 400);
  fails("POST", "/v1/sequencer/step", "/v1/sequencer/step", "{\"pattern\":1,\"step\":2,\"slot\":3,\"slice\":1.5}", 400);
}

// ---- recording from an input -------------------------------------------------------------------------------------------------
void wait_frames(StubHost& host, std::uint64_t at_least) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (host.capture->status().frames < at_least && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(host.capture->status().frames >= at_least);
}

void test_capture(StubHost& host, const fs::path& dir) {
  host.init_capture((dir / "captures").string());
  Http h = ok("GET", "/v1/capture/inputs", "/v1/capture/inputs", "", 200);
  CHECK(h.json()["inputs"].is_array() && has_key(h.json()["current"], "open") && h.json()["current"]["open"].as_bool());
  h = ok("GET", "/v1/capture", "/v1/capture", "", 200);
  CHECK(h.json()["state"].as_string() == "idle" && !h.json()["analysed"].as_bool());
  h = ok("GET", "/v1/capture/meter", "/v1/capture/meter", "", 200);
  CHECK(has_key(h.json(), "peak_db") && has_key(h.json(), "clips"));
  // nothing recorded yet
  fails("POST", "/v1/capture/stop", "/v1/capture/stop", "", 409);
  fails("POST", "/v1/capture/pause", "/v1/capture/pause", "", 409);
  fails("POST", "/v1/capture/analyse", "/v1/capture/analyse", "", 409);
  fails("GET", "/v1/capture/splits", "/v1/capture/splits", "", 409);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + (dir / "no").generic_string() + "\"}", 409);
  fails("POST", "/v1/capture/start", "/v1/capture/start", "{\"format\":\"mp3\"}", 400);
  fails("POST", "/v1/capture/start", "/v1/capture/start", "{\"channel\":99}", 400);
  fails("POST", "/v1/capture/start", "/v1/capture/start", "{\"stereo\":\"yes\"}", 400);
  fails("POST", "/v1/capture/start", "/v1/capture/start", "{\"device\":5}", 400);
  // record a synthetic side through the engine's (offline) input
  synth::SideSpec sp;
  sp.tracks = {{12, 0.2, 0, 0, false, 220, 0.05}, {12, 0.25, 0, 0, false, 330, 0.05}, {12, 0.15, 0, 0, false, 247, 0.05}};
  const synth::Side side = synth::make_side(sp);
  CHECK(side.rate == 44100);
  h = ok("POST", "/v1/capture/start", "/v1/capture/start", "{\"name\":\"side-a\",\"format\":\"pcm16\"}", 200);
  CHECK(h.json()["state"].as_string() == "recording" && h.json()["sample_rate"].as_int() == 44100);
  const std::string rec_path = h.json()["path"].as_string();
  CHECK(fs::exists(fs::u8path(rec_path)));
  fails("POST", "/v1/capture/start", "/v1/capture/start", "{}", 409);  // already recording
  fails("POST", "/v1/capture/load", "/v1/capture/load", "{\"path\":\"" + rec_path + "\"}", 409);
  std::uint64_t fed = 0;
  const std::uint64_t total = side.frames();
  const auto feed = [&](std::uint64_t upto) {
    while (fed < upto) {
      const std::uint64_t n = std::min<std::uint64_t>(30000, upto - fed);
      host.engine.feed_input(side.pcm.data() + fed * 2, static_cast<std::size_t>(n));
      fed += n;
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
  };
  feed(44100 * 2);
  wait_frames(host, 44100 * 2 - 100);
  h = ok("GET", "/v1/capture/meter", "/v1/capture/meter", "", 200);
  CHECK(h.json()["state"].as_string() == "recording" && h.json()["seconds"].as_double() > 1.0 && h.json()["hold_db"].as_double() > -90);
  // paused: what arrives is thrown away
  h = ok("POST", "/v1/capture/pause", "/v1/capture/pause", "{}", 200);
  CHECK(h.json()["state"].as_string() == "paused");
  const std::uint64_t before = host.capture->status().frames;
  feed(fed + 44100);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(host.capture->status().frames <= before + 2048);
  fails("POST", "/v1/capture/pause", "/v1/capture/pause", "{\"paused\":1}", 400);
  h = ok("POST", "/v1/capture/pause", "/v1/capture/pause", "{\"paused\":false}", 200);
  CHECK(h.json()["state"].as_string() == "recording");
  const std::uint64_t kept = host.capture->status().frames;
  const std::uint64_t resume_at = fed;
  const std::uint64_t remain = total - resume_at;
  feed(total);
  wait_frames(host, kept + remain - 3000);
  h = ok("POST", "/v1/capture/stop", "/v1/capture/stop", "", 200);
  CHECK(h.json()["state"].as_string() == "stopped" && h.json()["frames"].as_int() >= static_cast<std::int64_t>(kept + remain - 3000));
  CHECK(h.json()["dropped_frames"].as_int() == 0);
  // analyse and the split points
  fails("POST", "/v1/capture/analyse", "/v1/capture/analyse", "[1]", 400);
  fails("POST", "/v1/capture/analyse", "/v1/capture/analyse", "{\"min_gap_s\":\"long\"}", 400);
  fails("POST", "/v1/capture/analyse", "/v1/capture/analyse", "{\"min_track_s\":0}", 400);
  fails("POST", "/v1/capture/analyse", "/v1/capture/analyse", "{\"min_depth_db\":500}", 400);
  h = ok("POST", "/v1/capture/analyse", "/v1/capture/analyse", "{\"min_track_s\":5}", 200);
  CHECK(h.json()["analysed"].as_bool() && h.json()["points"].as_int() == 2);
  h = ok("GET", "/v1/capture/splits", "/v1/capture/splits?envelope=100", "", 200);
  CHECK(h.json()["points"].size() == 2 && h.json()["pieces"].size() == 3 && h.json()["envelope"].size() == 100 && has_key(h.json()["settings"], "min_gap_s"));
  CHECK(!h.json()["points"].at(0)["user"].as_bool() && h.json()["points"].at(0)["seconds"].as_double() > 5);
  fails("PUT", "/v1/capture/splits", "/v1/capture/splits", "[]", 400);
  fails("PUT", "/v1/capture/splits", "/v1/capture/splits", "{\"min_confidence\":3}", 400);
  ok("PUT", "/v1/capture/splits", "/v1/capture/splits", "{\"min_gap_s\":1.5}", 200);
  h = ok("POST", "/v1/capture/splits", "/v1/capture/splits", "{\"seconds\":5.5}", 200);
  const int added = static_cast<int>(h.json()["index"].as_int());
  fails("POST", "/v1/capture/splits", "/v1/capture/splits", "{}", 400);
  fails("POST", "/v1/capture/splits", "/v1/capture/splits", "{\"seconds\":-1}", 400);
  fails("POST", "/v1/capture/splits", "/v1/capture/splits", "{\"frame\":99999999999}", 409);  // beyond the recording
  h = call("GET", "/v1/capture/splits", g_tok.reader);
  CHECK(h.json()["points"].size() == 3 && h.json()["points"].at(static_cast<std::size_t>(added))["user"].as_bool());
  ok("PUT", "/v1/capture/splits/{index}", "/v1/capture/splits/" + std::to_string(added), "{\"seconds\":6.0}", 200);
  fails("PUT", "/v1/capture/splits/{index}", "/v1/capture/splits/77", "{\"seconds\":6.0}", 404);
  fails("PUT", "/v1/capture/splits/{index}", "/v1/capture/splits/" + std::to_string(added), "{}", 400);
  ok("DELETE", "/v1/capture/splits/{index}", "/v1/capture/splits/" + std::to_string(added), "", 204);
  fails("DELETE", "/v1/capture/splits/{index}", "/v1/capture/splits/77", "", 404);
  CHECK(call("GET", "/v1/capture/splits", g_tok.reader).json()["points"].size() == 2);
  // names and export
  fails("PUT", "/v1/capture/tags", "/v1/capture/tags", "[]", 400);
  fails("PUT", "/v1/capture/tags", "/v1/capture/tags", "{\"titles\":[1]}", 400);
  fails("PUT", "/v1/capture/tags", "/v1/capture/tags", "{\"artist\":5}", 400);
  ok("PUT", "/v1/capture/tags", "/v1/capture/tags", "{\"artist\":\"Band\",\"album\":\"Side A\",\"titles\":[\"One\",\"Two\",\"Three\"]}", 200);
  CHECK(call("GET", "/v1/capture/splits", g_tok.reader).json()["pieces"].at(1)["title"].as_string() == "Two");
  const fs::path out = dir / "capture-out";
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"relative\"}", 400);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"container\":\"flac\"}", 400);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"bits\":20}", 400);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"container\":\"aiff\",\"bits\":32}", 400);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"cleanup\":{\"normalise\":1}}", 400);
  fails("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"cleanup\":{\"fade_in_ms\":-3}}", 400);
  h = ok("POST", "/v1/capture/export", "/v1/capture/export", "{\"dir\":\"" + out.generic_string() + "\",\"bits\":16,\"cleanup\":{\"remove_dc\":true,\"fade_in_ms\":10,\"fade_out_ms\":10}}", 200);
  CHECK(h.json()["ok"].as_bool() && h.json()["files"].size() == 3);
  std::size_t wavs = 0;
  for (const auto& e : fs::directory_iterator(out)) wavs += e.path().extension() == ".wav" ? 1 : 0;
  CHECK(wavs == 3);
  h = call("POST", "/v1/capture/export", token_for("export"), "{\"dir\":\"" + out.generic_string() + "\"}");  // the files exist now: not overwritten
  CHECK(h.status == 200 && !h.json()["ok"].as_bool());
  // an existing recording can be loaded
  fails("POST", "/v1/capture/load", "/v1/capture/load", "{\"path\":\"relative.wav\"}", 400);
  fails("POST", "/v1/capture/load", "/v1/capture/load", "{\"path\":\"" + (dir / "none.wav").generic_string() + "\"}", 400);
  h = ok("POST", "/v1/capture/load", "/v1/capture/load", "{\"path\":\"" + rec_path + "\"}", 200);
  CHECK(h.json()["state"].as_string() == "stopped" && !h.json()["analysed"].as_bool());
  // an edited copy of a library track (the recording stands in for a track); the original is never written
  std::int64_t tid = 0;
  {
    Library lib2((dir / "library.db").string());
    tid = add(lib2, fs::u8path(rec_path), "Rec");
  }
  CHECK(tid > 0);
  const fs::path edit = dir / "edited.wav";
  h = ok("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/" + std::to_string(tid) + "/export-edit",
         "{\"out\":\"" + edit.generic_string() + "\",\"start\":1,\"end\":3,\"cleanup\":{\"fade_in_ms\":50,\"fade_out_ms\":50}}", 200);
  CHECK(h.json()["ok"].as_bool() && h.json()["frames"].as_int() > 80000 && h.json()["frames"].as_int() < 96000 && fs::exists(edit));
  const auto source_size = fs::file_size(fs::u8path(rec_path));
  fails("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/" + std::to_string(tid) + "/export-edit", "{\"out\":\"" + rec_path + "\"}", 409);  // never over the original
  CHECK(fs::file_size(fs::u8path(rec_path)) == source_size);
  fails("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/" + std::to_string(tid) + "/export-edit", "{\"out\":\"" + edit.generic_string() + "\"}", 409);  // not over another file
  fails("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/" + std::to_string(tid) + "/export-edit", "{\"out\":\"x.wav\"}", 400);
  fails("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/" + std::to_string(tid) + "/export-edit", "{\"out\":\"" + (dir / "e2.wav").generic_string() + "\",\"start\":5,\"end\":2}", 400);
  fails("POST", "/v1/library/tracks/{id}/export-edit", "/v1/library/tracks/987654/export-edit", "{\"out\":\"" + (dir / "e3.wav").generic_string() + "\"}", 404);
}

// ---- sync ------------------------------------------------------------------------------------------------------------------------
void test_sync(StubHost& host, const Fixture& fx) {
  Http h = ok("GET", "/v1/sync", "/v1/sync", "", 200);
  CHECK(has_key(h.json(), "config") && has_key(h.json(), "last") && h.json()["last"]["state"].as_string() == "idle");
  for (const char* k : {"folder_set", "device_name", "mix_links", "analysis", "audio_playlists", "music_folder", "audio_budget_mb", "audio_import_budget_mb", "allow_unencrypted_audio", "encryption_available"})
    CHECK(has_key(h.json()["config"], k));
  CHECK(!h.json()["config"]["folder_set"].as_bool() && h.json()["config"]["mix_links"].as_bool() && h.json()["config"]["audio_playlists"].is_array());

  // no folder yet: nothing is queued
  fails("POST", "/v1/sync/run", "/v1/sync/run", "", 409);
  CHECK(host.sync_runs == 0);
  CHECK(call("PUT", "/v1/settings/sync.folder", g_tok.admin, "{\"value\":\"" + fx.sync_dir.generic_string() + "\"}").status == 204);
  h = ok("POST", "/v1/sync/run", "/v1/sync/run", "", 202);
  CHECK(has_key(h.json(), "config") && has_key(h.json(), "last") && host.sync_runs >= 1 && h.json()["config"]["folder_set"].as_bool());

  Json cfg = Json::object();
  cfg.set("mix_links", false).set("analysis", true).set("allow_unencrypted_audio", true).set("audio_budget_mb", 512).set("audio_import_budget_mb", 0);
  cfg.set("music_folder", fx.keep_dir.string()).set("audio_playlists", Json::array().push_back(fx.list).push_back(fx.folder));
  h = ok("PUT", "/v1/sync/config", "/v1/sync/config", cfg.dump(), 200);
  const Json c = h.json()["config"];
  CHECK(!c["mix_links"].as_bool() && c["analysis"].as_bool() && c["allow_unencrypted_audio"].as_bool());
  CHECK(c["audio_budget_mb"].as_int() == 512 && c["audio_import_budget_mb"].as_int() == 0 && c["music_folder"].as_string() == fx.keep_dir.string());
  CHECK(c["audio_playlists"].size() == 2 && c["audio_playlists"].at(0).as_int() == fx.list);
  ok("PUT", "/v1/sync/config", "/v1/sync/config", "{\"music_folder\":\"\",\"audio_playlists\":[]}", 200);
  h = call("GET", "/v1/sync", g_tok.reader);
  CHECK(h.json()["config"]["music_folder"].as_string().empty() && h.json()["config"]["audio_playlists"].size() == 0 && h.json()["config"]["analysis"].as_bool());  // untouched keys stay

  CHECK(c["audio_cleanup_days"].as_int() == 30);  // the default
  for (const auto& kv : std::vector<std::pair<const char*, int>>{{"0", 0}, {"7", 7}, {"-1", -1}, {"-9", -1}}) {
    h = ok("PUT", "/v1/sync/config", "/v1/sync/config", std::string("{\"audio_cleanup_days\":") + kv.first + "}", 200);
    CHECK(h.json()["config"]["audio_cleanup_days"].as_int() == kv.second);
  }
  ok("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_cleanup_days\":30}", 200);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_cleanup_days\":36501}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_cleanup_days\":1.5}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_cleanup_days\":\"3\"}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "[1]", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"mix_links\":\"yes\"}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"analysis\":1}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_budget_mb\":-1}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_import_budget_mb\":1.5}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_budget_mb\":\"5\"}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"music_folder\":\"relative\"}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"music_folder\":\"" + (fx.dir / "no-such-dir").generic_string() + "\"}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"music_folder\":5}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_playlists\":5}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_playlists\":[987654]}", 400);
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"audio_playlists\":[\"1\"]}", 400);
  // a refused body changes nothing, even the valid keys in it
  fails("PUT", "/v1/sync/config", "/v1/sync/config", "{\"mix_links\":true,\"audio_budget_mb\":-5}", 400);
  CHECK(!call("GET", "/v1/sync", g_tok.reader).json()["config"]["mix_links"].as_bool());
  // control (not admin) may not change the configuration
  CHECK(call("PUT", "/v1/sync/config", token_for("control"), "{\"analysis\":false}").status == 403);
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / ("dh-api-routes-" + std::to_string(reinterpret_cast<std::uintptr_t>(&g_port) % 1000003));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  Fixture fx;
  fx.dir = dir;
  fx.old_dir = dir / "music" / "old";
  fx.new_dir = dir / "music" / "new";
  fx.keep_dir = dir / "music" / "keep";
  fx.sync_dir = dir / "sync";
  fs::create_directories(fx.sync_dir, ec);
  {
    Library lib((dir / "library.db").string());
    lib.set_setting("api.port", "0");  // any free port; never the shipped one
    lib.set_setting("api.enabled", "1");
    // A to D are in old/, E in keep/; then old/ is emptied and the first four appear in new/ under the same names
    const char* names[] = {"a.mp3", "b.mp3", "c.mp3", "d.mp3"};
    std::int64_t* ids[] = {&fx.a, &fx.b, &fx.c, &fx.d};
    for (int i = 0; i < 4; ++i) {
      put(fx.old_dir / names[i], std::string(100 + static_cast<std::size_t>(i) * 17, static_cast<char>('a' + i)));
      *ids[i] = add(lib, fx.old_dir / names[i], names[i]);
    }
    put(fx.keep_dir / "e.mp3", "eeeeeeeeeeeeeeeeeeeee");
    fx.e = add(lib, fx.keep_dir / "e.mp3", "E");
    put(fx.keep_dir / "gone.mp3", "gone-gone-gone-gone-gone");
    fx.gone = add(lib, fx.keep_dir / "gone.mp3", "Gone");
    for (int guard = 0; lib.hash_pending(50).remaining > 0 && guard < 20; ++guard) {}  // sizes and hashes, so moved files are recognised
    for (int i = 0; i < 4; ++i) put(fx.new_dir / names[i], std::string(100 + static_cast<std::size_t>(i) * 17, static_cast<char>('a' + i)));
    fs::remove_all(fx.old_dir, ec);
    lib.remove_track(fx.gone);  // into the trash
    fx.folder = lib.create_playlist(0, "Folder", true);
    fx.list = lib.create_playlist(fx.folder, "List", false);
    lib.add_to_playlist(fx.list, fx.e);
    CHECK(fx.a > 0 && fx.b > 0 && fx.c > 0 && fx.d > 0 && fx.e > 0 && fx.folder > 0 && fx.list > 0);
  }
  // tokens: the store is filled before the bridge starts, so it makes no owner token of its own
  {
    api::TokenStore ts((dir / "api-tokens.json").string());
    api::TokenStore::Created cr;
    std::string err;
    CHECK(ts.create("admin", {"admin"}, 0, {}, &cr, &err));
    g_tok.admin = cr.secret;
    CHECK(ts.create("reader", {"read"}, 0, {}, &cr, &err));
    g_tok.reader = cr.secret;
    CHECK(ts.create("nothing", {"nothing"}, 0, {}, &cr, &err));
    g_tok.nothing = cr.secret;
  }
  load_spec();

  StubHost host;
  int rc = 0;
  {
    ApiBridge bridge(host, dir.string());
    g_port = bridge.port();
    if (g_port == 0) {
      std::printf("the API is not part of this build: skipped\n");
    } else {
      CHECK(g_port != 7373);
      test_chains(host, dir);
      test_browser(host, fx);
      test_folders(fx);
      test_relocation(host, fx);
      test_trash(fx);
      test_sync(host, fx);
      test_playlist_trash(fx);
      test_sequencer_slice(host);
      test_capture(host, dir);
      // every operation this test is about was called
      for (const char* k : {"GET /v1/fx/chains/{target}/units/{id}/plugin", "GET /v1/fx/plugins", "POST /v1/fx/plugins/rescan", "GET /v1/fx/presets", "POST /v1/fx/presets/delete",
                            "POST /v1/fx/chains/{target}/presets", "POST /v1/fx/chains/{target}/presets/load", "GET /v1/browser", "PUT /v1/browser/split",
                            "PUT /v1/browser/panes/{slot}", "POST /v1/browser/layouts", "POST /v1/browser/layouts/delete", "PUT /v1/browser/layout",
                            "GET /v1/library/folders/{id}/tracks", "GET /v1/library/disk-folders", "GET /v1/library/disk-folders/tracks", "DELETE /v1/library/trash",
                            "POST /v1/library/relocate/scan", "POST /v1/library/relocate/apply", "POST /v1/library/tracks/{id}/relink",
                            "POST /v1/library/tracks/{id}/relink/siblings", "GET /v1/sync", "POST /v1/sync/run", "PUT /v1/sync/config",
                            "GET /v1/library/playlist-trash", "POST /v1/library/playlist-trash/{id}/restore", "DELETE /v1/library/playlist-trash/{id}", "DELETE /v1/library/playlist-trash",
                            "POST /v1/sequencer/step", "GET /v1/capture/inputs", "GET /v1/capture", "GET /v1/capture/meter", "POST /v1/capture/start", "POST /v1/capture/pause",
                            "POST /v1/capture/stop", "POST /v1/capture/load", "POST /v1/capture/analyse", "GET /v1/capture/splits", "PUT /v1/capture/splits", "POST /v1/capture/splits",
                            "PUT /v1/capture/splits/{index}", "DELETE /v1/capture/splits/{index}", "PUT /v1/capture/tags", "POST /v1/capture/export",
                            "POST /v1/library/tracks/{id}/export-edit"})
        CHECK(g_called.count(k));
      std::printf("api routes ok\n");
    }
  }
  plugin_service().wait();
  fs::remove_all(dir, ec);
  return rc;
}
