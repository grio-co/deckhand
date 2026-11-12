#include "deckhand/broadcast/obs.hpp"

#include <algorithm>
#include <cmath>

#include "deckhand/broadcast/crypto.hpp"
#include "deckhand/broadcast/websocket.hpp"
#include "deckhand/log/log.hpp"
#include "sock.hpp"

namespace deckhand::broadcast {

struct ObsClient::Conn {
  int fd = -1;
  WsParser parser;
  double last_rx = 0;
  double last_ping = 0;
  ~Conn() { net::close_fd(fd); }
};

namespace {

constexpr double kPingEvery = 15.0;
constexpr double kDeadAfter = 45.0;

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string close_reason(const std::string& payload) {
  if (payload.size() >= 2) {
    const int code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
    if (code == 4009) return "authentication failed";
    return "closed by OBS (code " + std::to_string(code) + ")";
  }
  return "closed by OBS";
}

}  // namespace

ObsClient::ObsClient(ObsConfig cfg)
    : cfg_(std::move(cfg)), wake_(std::make_unique<net::Wake>()), abort_(std::make_unique<net::Wake>()) {}

ObsClient::~ObsClient() { stop(); }

void ObsClient::configure(const ObsConfig& cfg) {
  bool restart;
  {
    std::lock_guard<std::mutex> lk(m_);
    cfg_ = cfg;
    restart = thread_.joinable() && !stop_;
  }
  if (restart) {
    stop();
    start();
  }
}

void ObsClient::start() {
  std::lock_guard<std::mutex> lk(m_);
  if (thread_.joinable()) return;
  accepting_ = true;
  stop_ = false;
  abort_->drain();
  wake_->drain();
  state_ = ObsState::Connecting;
  thread_ = std::thread([this] { run(); });
  thread_id_ = thread_.get_id();
}

void ObsClient::stop() {
  std::thread t;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!thread_.joinable()) return;
    stop_ = true;
    abort_->poke();
    if (std::this_thread::get_id() == thread_id_) return;  // called from a callback: the thread winds down itself
    t = std::move(thread_);
  }
  t.join();
  std::lock_guard<std::mutex> lk(m_);
  state_ = ObsState::Stopped;
}

bool ObsClient::connected() const {
  std::lock_guard<std::mutex> lk(m_);
  return state_ == ObsState::Connected;
}

ObsState ObsClient::state() const {
  std::lock_guard<std::mutex> lk(m_);
  return state_;
}

std::string ObsClient::last_error() const {
  std::lock_guard<std::mutex> lk(m_);
  return last_error_;
}

void ObsClient::on_connection(ObsConnectionCallback cb) {
  std::lock_guard<std::mutex> lk(m_);
  on_conn_ = std::move(cb);
}

void ObsClient::on_event(ObsEventCallback cb) {
  std::lock_guard<std::mutex> lk(m_);
  on_event_ = std::move(cb);
}

void ObsClient::set_state(ObsState s) {
  std::lock_guard<std::mutex> lk(m_);
  if (state_ != ObsState::Stopped || s == ObsState::Stopped) state_ = s;
}

void ObsClient::finish(const ObsCallback& cb, ObsResponse r) {
  if (!cb) return;
  try {
    cb(r);
  } catch (...) {
  }
}

void ObsClient::request(const std::string& type, const std::string& json_text, ObsCallback cb) {
  Queued q;
  q.type = type;
  q.cb = std::move(cb);
  if (!json_text.empty()) {
    auto parsed = Json::parse(json_text);
    if (!parsed || !parsed->is_object()) q.bad = true;
    else q.data = std::move(*parsed);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    q.deadline = net::now_seconds() + cfg_.request_timeout;
    if (accepting_) {
      queue_.push_back(std::move(q));
      q.cb = nullptr;
    }
  }
  if (!q.cb) {
    wake_->poke();
    return;
  }
  ObsResponse r;
  r.error = "not started";
  finish(q.cb, r);  // client thread is not running: report on the caller's thread
}

void ObsClient::fail_pending(const char* reason) {
  std::map<std::string, Pending> p;
  p.swap(pending_);
  for (auto& kv : p) {
    ObsResponse r;
    r.error = reason;
    finish(kv.second.cb, r);
  }
}

// Sends queued requests (c != nullptr) or fails them with fail_reason. False when the link broke.
bool ObsClient::flush_queue(Conn* c, const char* fail_reason) {
  std::deque<Queued> q;
  {
    std::lock_guard<std::mutex> lk(m_);
    q.swap(queue_);
  }
  bool broken = false;
  for (auto& item : q) {
    ObsResponse r;
    if (item.bad) {
      r.error = "bad request json";
      finish(item.cb, r);
    } else if (!c || broken) {
      r.error = broken ? "disconnected" : fail_reason;
      finish(item.cb, r);
    } else if (net::now_seconds() > item.deadline) {
      r.error = "timeout";
      finish(item.cb, r);
    } else {
      const std::string id = "dh-" + std::to_string(next_id_++);
      Json d = Json::object();
      d.set("requestType", Json(item.type));
      d.set("requestId", Json(id));
      if (item.data.is_object()) d.set("requestData", item.data);
      Json msg = Json::object();
      msg.set("op", Json(6));
      msg.set("d", std::move(d));
      pending_[id] = Pending{item.cb, item.deadline, item.type};
      if (!net::send_all(c->fd, ws_encode(WsOpcode::Text, msg.dump(), true), 3000, abort_->fd())) {
        broken = true;
        pending_.erase(id);
        r.error = "disconnected";
        finish(item.cb, r);
      }
    }
  }
  return !broken;
}

bool ObsClient::handle_message(Conn& c, const WsMessage& m) {
  switch (m.opcode) {
    case WsOpcode::Ping:
      return net::send_all(c.fd, ws_encode(WsOpcode::Pong, m.payload, true), 3000, abort_->fd());
    case WsOpcode::Close:
      {
        std::lock_guard<std::mutex> lk(m_);
        last_error_ = close_reason(m.payload);
      }
      return false;
    case WsOpcode::Text: break;
    default: return true;
  }
  auto j = Json::parse(m.payload);
  if (!j) return true;
  const int op = static_cast<int>((*j)["op"].as_number(-1));
  const Json& d = (*j)["d"];
  if (op == 7) {
    const std::string id = d["requestId"].as_string();
    auto it = pending_.find(id);
    if (it == pending_.end()) return true;
    ObsCallback cb = it->second.cb;
    pending_.erase(it);
    ObsResponse r;
    const Json& st = d["requestStatus"];
    r.ok = st["result"].as_bool();
    r.code = static_cast<int>(st["code"].as_number());
    if (!r.ok) r.error = st["comment"].as_string("request failed");
    if (!r.ok && r.error.empty()) r.error = "request failed";
    r.data = d["responseData"];
    finish(cb, std::move(r));
  } else if (op == 5) {
    ObsEventCallback cb;
    {
      std::lock_guard<std::mutex> lk(m_);
      cb = on_event_;
    }
    if (cb) {
      try {
        cb(d["eventType"].as_string(), d["eventData"]);
      } catch (...) {
      }
    }
  }
  return true;
}

// Waits for the next complete message; false on timeout, error, close or stop (err says why).
static bool read_message(int fd, WsParser& parser, net::Wake& abort, double deadline, WsMessage* out,
                         std::string* err, const std::function<bool(const WsMessage&)>& on_control) {
  for (;;) {
    while (parser.next(out)) {
      if (out->opcode == WsOpcode::Ping || out->opcode == WsOpcode::Pong) {
        if (out->opcode == WsOpcode::Ping && on_control && !on_control(*out)) { *err = "send failed"; return false; }
        continue;
      }
      if (out->opcode == WsOpcode::Close) { *err = close_reason(out->payload); return false; }
      return true;
    }
    if (parser.failed()) { *err = "protocol error"; return false; }
    const int left = static_cast<int>((deadline - net::now_seconds()) * 1000);
    if (left <= 0) { *err = "handshake timeout"; return false; }
    std::vector<net::PollItem> it{{fd, true, false}, {abort.fd(), true, false}};
    if (net::poll(it, left) < 0) { *err = "poll failed"; return false; }
    if (it[1].readable) { *err = "stopped"; return false; }
    if (it[0].readable) {
      char buf[4096];
      const long n = net::recv_some(fd, buf, sizeof buf);
      if (n < 0) { *err = "connection closed"; return false; }
      if (n > 0) parser.feed(buf, static_cast<std::size_t>(n));
    }
  }
}

bool ObsClient::open_session(Conn& c, std::string* err) {
  ObsConfig cfg;
  {
    std::lock_guard<std::mutex> lk(m_);
    cfg = cfg_;
  }
  c.fd = net::connect_tcp(cfg.host, cfg.port, 3000, abort_->fd());
  if (c.fd < 0) { *err = "cannot connect to " + cfg.host + ":" + std::to_string(cfg.port); return false; }
  const std::string key = ws_random_key();
  if (!net::send_all(c.fd, ws_client_request(cfg.host + ":" + std::to_string(cfg.port), "/", key, "obswebsocket.json"),
                     3000, abort_->fd())) {
    *err = "handshake send failed";
    return false;
  }
  // HTTP upgrade response.
  std::string head;
  const double deadline = net::now_seconds() + 5.0;
  std::size_t end;
  while ((end = head.find("\r\n\r\n")) == std::string::npos) {
    if (head.size() > 8192) { *err = "bad handshake response"; return false; }
    const int left = static_cast<int>((deadline - net::now_seconds()) * 1000);
    if (left <= 0) { *err = "handshake timeout"; return false; }
    std::vector<net::PollItem> it{{c.fd, true, false}, {abort_->fd(), true, false}};
    if (net::poll(it, left) < 0 || it[1].readable) { *err = "stopped"; return false; }
    if (it[0].readable) {
      char buf[2048];
      const long n = net::recv_some(c.fd, buf, sizeof buf);
      if (n < 0) { *err = "connection closed during handshake"; return false; }
      head.append(buf, static_cast<std::size_t>(n > 0 ? n : 0));
    }
  }
  const std::string leftover = head.substr(end + 4);
  head.resize(end);
  if (head.compare(0, 12, "HTTP/1.1 101") != 0) { *err = "server did not upgrade to WebSocket"; return false; }
  bool accepted = false;
  std::size_t pos = head.find("\r\n");
  while (pos != std::string::npos) {
    const std::size_t e = head.find("\r\n", pos + 2);
    const std::string line = head.substr(pos + 2, e == std::string::npos ? std::string::npos : e - pos - 2);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == "sec-websocket-accept") {
      std::string v = line.substr(colon + 1);
      v.erase(0, v.find_first_not_of(" \t"));
      v.erase(v.find_last_not_of(" \t") + 1);
      accepted = v == websocket_accept_key(key);
    }
    pos = e;
  }
  if (!accepted) { *err = "bad Sec-WebSocket-Accept"; return false; }
  c.parser.feed(leftover.data(), leftover.size());

  auto pong = [&](const WsMessage& m) {
    return net::send_all(c.fd, ws_encode(WsOpcode::Pong, m.payload, true), 3000, abort_->fd());
  };
  // Hello (op 0) -> Identify (op 1) -> Identified (op 2).
  WsMessage m;
  if (!read_message(c.fd, c.parser, *abort_, deadline + 5.0, &m, err, pong)) return false;
  auto hello = Json::parse(m.payload);
  if (!hello || static_cast<int>((*hello)["op"].as_number(-1)) != 0) { *err = "unexpected message instead of Hello"; return false; }
  Json d = Json::object();
  d.set("rpcVersion", Json(1));
  const Json& auth = (*hello)["d"]["authentication"];
  if (auth.is_object()) {
    if (cfg.password.empty()) { *err = "OBS requires a password"; return false; }
    d.set("authentication", Json(obs_auth_string(cfg.password, auth["salt"].as_string(), auth["challenge"].as_string())));
  }
  d.set("eventSubscriptions", Json(static_cast<long long>(cfg.event_subscriptions)));
  Json ident = Json::object();
  ident.set("op", Json(1));
  ident.set("d", std::move(d));
  if (!net::send_all(c.fd, ws_encode(WsOpcode::Text, ident.dump(), true), 3000, abort_->fd())) {
    *err = "identify send failed";
    return false;
  }
  const double id_deadline = net::now_seconds() + 5.0;
  for (;;) {
    if (!read_message(c.fd, c.parser, *abort_, id_deadline, &m, err, pong)) return false;
    auto j = Json::parse(m.payload);
    if (j && static_cast<int>((*j)["op"].as_number(-1)) == 2) break;
  }
  c.last_rx = c.last_ping = net::now_seconds();
  return true;
}

void ObsClient::serve(Conn& c) {
  while (!stop_) {
    if (!flush_queue(&c, "not connected")) return;
    double now = net::now_seconds();
    // Expire requests OBS did not answer in time.
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now >= it->second.deadline) {
        ObsResponse r;
        r.error = "timeout";
        ObsCallback cb = it->second.cb;
        it = pending_.erase(it);
        finish(cb, r);
      } else {
        ++it;
      }
    }
    if (now - c.last_rx > kDeadAfter) return;
    if (now - c.last_ping >= kPingEvery) {
      c.last_ping = now;
      if (!net::send_all(c.fd, ws_encode(WsOpcode::Ping, "", true), 3000, abort_->fd())) return;
    }
    double wait = 1.0;
    for (const auto& kv : pending_) wait = std::min(wait, kv.second.deadline - now);
    const int ms = static_cast<int>(std::max(0.0, std::ceil(wait * 1000)));
    std::vector<net::PollItem> it{{c.fd, true, false}, {wake_->fd(), true, false}, {abort_->fd(), true, false}};
    if (net::poll(it, ms) < 0) return;
    if (it[2].readable) return;
    if (it[1].readable) wake_->drain();
    if (it[0].readable) {
      char buf[8192];
      for (;;) {
        const long n = net::recv_some(c.fd, buf, sizeof buf);
        if (n < 0) return;
        if (n == 0) break;
        c.last_rx = net::now_seconds();
        c.parser.feed(buf, static_cast<std::size_t>(n));
        if (static_cast<std::size_t>(n) < sizeof buf) break;
      }
      WsMessage m;
      while (c.parser.next(&m))
        if (!handle_message(c, m)) return;
      if (c.parser.failed()) return;
    }
  }
}

void ObsClient::run() {
  ObsConfig cfg;
  {
    std::lock_guard<std::mutex> lk(m_);
    cfg = cfg_;
  }
  double backoff = cfg.reconnect_min;
  while (!stop_) {
    set_state(ObsState::Connecting);
    std::string err;
    bool was_connected = false;
    {
      Conn c;
      if (open_session(c, &err)) {
        was_connected = true;
        set_state(ObsState::Connected);
        backoff = cfg.reconnect_min;
        ObsConnectionCallback cb;
        {
          std::lock_guard<std::mutex> lk(m_);
          cb = on_conn_;
          last_error_.clear();
        }
        if (cb) { try { cb(true); } catch (...) {} }
        serve(c);
      }
    }  // socket closed here
    fail_pending("disconnected");
    if (!was_connected || !err.empty()) {
      std::lock_guard<std::mutex> lk(m_);
      if (!err.empty()) last_error_ = err;
    }
    set_state(ObsState::Disconnected);
    if (was_connected) {
      ObsConnectionCallback cb;
      {
        std::lock_guard<std::mutex> lk(m_);
        cb = on_conn_;
      }
      if (cb) { try { cb(false); } catch (...) {} }
    }
    if (stop_) break;
    // Back off, failing any request that arrives meanwhile.
    const double until = net::now_seconds() + backoff;
    while (!stop_) {
      const int left = static_cast<int>((until - net::now_seconds()) * 1000);
      if (left <= 0) break;
      std::vector<net::PollItem> it{{wake_->fd(), true, false}, {abort_->fd(), true, false}};
      if (net::poll(it, left) < 0) break;
      if (it[1].readable) break;
      if (it[0].readable) {
        wake_->drain();
        flush_queue(nullptr, "not connected");
      }
    }
    backoff = std::min(backoff * 2, cfg.reconnect_max);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    accepting_ = false;
  }
  flush_queue(nullptr, "stopped");
  fail_pending("stopped");
  std::lock_guard<std::mutex> lk(m_);
  state_ = ObsState::Stopped;
}

// ---- typed helpers ----

namespace {

std::string obj1(const char* k, const Json& v) {
  Json o = Json::object();
  o.set(k, v);
  return o.dump();
}

}  // namespace

void ObsClient::get_version(ObsCallback cb) { request("GetVersion", "", std::move(cb)); }

void ObsClient::list_scenes(ObsScenesCallback cb) {
  request("GetSceneList", "", [cb](const ObsResponse& r) {
    std::vector<std::string> names;
    const Json& arr = r.data["scenes"];
    for (std::size_t i = 0; i < arr.size(); ++i) names.push_back(arr.at(i)["sceneName"].as_string());
    if (cb) cb(r, names, r.data["currentProgramSceneName"].as_string());
  });
}

void ObsClient::get_current_scene(ObsStringCallback cb) {
  request("GetCurrentProgramScene", "", [cb](const ObsResponse& r) {
    if (cb) cb(r, r.data["currentProgramSceneName"].as_string());
  });
}

void ObsClient::set_current_scene(const std::string& scene, ObsCallback cb) {
  request("SetCurrentProgramScene", obj1("sceneName", Json(scene)), std::move(cb));
}

void ObsClient::set_text(const std::string& input_name, const std::string& text, ObsCallback cb) {
  Json settings = Json::object();
  settings.set("text", Json(text));
  Json d = Json::object();
  d.set("inputName", Json(input_name));
  d.set("inputSettings", std::move(settings));
  d.set("overlay", Json(true));
  request("SetInputSettings", d.dump(), std::move(cb));
}

void ObsClient::get_scene_item_id(const std::string& scene, const std::string& source,
                                  std::function<void(const ObsResponse&, long long id)> cb) {
  Json d = Json::object();
  d.set("sceneName", Json(scene));
  d.set("sourceName", Json(source));
  request("GetSceneItemId", d.dump(), [cb](const ObsResponse& r) {
    if (cb) cb(r, static_cast<long long>(r.data["sceneItemId"].as_number(-1)));
  });
}

void ObsClient::set_scene_item_enabled(const std::string& scene, long long item_id, bool enabled, ObsCallback cb) {
  Json d = Json::object();
  d.set("sceneName", Json(scene));
  d.set("sceneItemId", Json(item_id));
  d.set("sceneItemEnabled", Json(enabled));
  request("SetSceneItemEnabled", d.dump(), std::move(cb));
}

void ObsClient::set_source_visible(const std::string& scene, const std::string& source, bool visible, ObsCallback cb) {
  get_scene_item_id(scene, source, [this, scene, visible, cb](const ObsResponse& r, long long id) {
    if (!r.ok) {
      if (cb) cb(r);
      return;
    }
    set_scene_item_enabled(scene, id, visible, cb);
  });
}

void ObsClient::start_stream(ObsCallback cb) { request("StartStream", "", std::move(cb)); }
void ObsClient::stop_stream(ObsCallback cb) { request("StopStream", "", std::move(cb)); }
void ObsClient::toggle_stream(ObsCallback cb) { request("ToggleStream", "", std::move(cb)); }
void ObsClient::start_record(ObsCallback cb) { request("StartRecord", "", std::move(cb)); }
void ObsClient::stop_record(ObsCallback cb) { request("StopRecord", "", std::move(cb)); }
void ObsClient::toggle_record(ObsCallback cb) { request("ToggleRecord", "", std::move(cb)); }

void ObsClient::stream_status(ObsBoolCallback cb) {
  request("GetStreamStatus", "", [cb](const ObsResponse& r) {
    if (cb) cb(r, r.data["outputActive"].as_bool());
  });
}

void ObsClient::record_status(ObsBoolCallback cb) {
  request("GetRecordStatus", "", [cb](const ObsResponse& r) {
    if (cb) cb(r, r.data["outputActive"].as_bool());
  });
}

}  // namespace deckhand::broadcast
