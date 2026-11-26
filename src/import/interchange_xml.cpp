// DJ library XML ("DJ_PLAYLISTS") export and import. Written from the publicly documented structure: COLLECTION/TRACK
// entries (Location is a file URL), TEMPO grid sections, POSITION_MARK cues and loops, and a PLAYLISTS tree of NODE
// folders and playlists that reference tracks by key. Includes a small self-contained XML reader/writer.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "deckhand/import/interchange.hpp"
#include "deckhand/log/log.hpp"

namespace deckhand {
namespace fs = std::filesystem;

namespace {

// ---- strings and paths ---------------------------------------------------------------------------------------------
fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
bool looks_windows(const std::string& p) {
  return (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') || p.rfind("\\\\", 0) == 0;
}
std::string base_name(const std::string& p) {
  const auto at = p.find_last_of("/\\");
  return at == std::string::npos ? p : p.substr(at + 1);
}
std::string stem_of(const std::string& p) {
  const std::string b = base_name(p);
  const auto dot = b.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}

// Length of the valid UTF-8 sequence at s[i], 0 when invalid.
int utf8_len(const std::string& s, std::size_t i) {
  const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned c = b(i);
  if (c < 0x80) return 1;
  const auto cont = [&](std::size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(i + 1) || !cont(i + 2)) return 0;
    if (c == 0xE0 && b(i + 1) < 0xA0) return 0;   // overlong
    if (c == 0xED && b(i + 1) >= 0xA0) return 0;  // surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
    if (c == 0xF0 && b(i + 1) < 0x90) return 0;
    if (c == 0xF4 && b(i + 1) >= 0x90) return 0;
    return 4;
  }
  return 0;
}
bool valid_utf8(const std::string& s) {
  for (std::size_t i = 0; i < s.size();) {
    const int n = utf8_len(s, i);
    if (!n) return false;
    i += static_cast<std::size_t>(n);
  }
  return true;
}

void append_utf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Attribute-value escaping; invalid UTF-8 becomes U+FFFD and control characters XML cannot hold are dropped.
std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (std::size_t i = 0; i < s.size();) {
    const int n = utf8_len(s, i);
    if (!n) {
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    if (n > 1) {
      out.append(s, i, static_cast<std::size_t>(n));
      i += static_cast<std::size_t>(n);
      continue;
    }
    const char c = s[i++];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\t': out += "&#9;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out += c;
    }
  }
  return out;
}

std::string fixed(double v, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  for (char* p = buf; *p; ++p)
    if (*p == ',') *p = '.';  // a locale with a decimal comma
  std::string out = buf;
  if (out[0] == '-' && out.find_first_of("123456789") == std::string::npos) out.erase(0, 1);  // "-0.00"
  return out;
}

bool parse_num(const std::string* s, double& out) {
  if (!s || s->empty()) return false;
  std::string t = *s;
  for (char& c : t)
    if (c == ',') c = '.';
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str()) return false;
  while (*end == ' ') ++end;
  if (*end || !std::isfinite(v)) return false;
  out = v;
  return true;
}
double round_ms(double ms) { return std::nearbyint(ms * 1000.0) / 1000.0; }

// ---- dates (UTC, day resolution) -----------------------------------------------------------------------------------
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}
std::string date_text(std::int64_t secs) {
  int y;
  unsigned m, d;
  civil_from_days(secs / 86400, y, m, d);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
  return buf;
}
std::int64_t parse_date(const std::string& s) {  // "YYYY-MM-DD..." -> unix seconds, 0 when not a date
  int y = 0, m = 0, d = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
  if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)) * 86400;
}

// ---- a small XML reader ----------------------------------------------------------------------------------------------
struct XNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<XNode> kids;
  const std::string* attr(const char* key) const {
    for (const auto& a : attrs)
      if (a.first == key) return &a.second;
    return nullptr;
  }
  std::string str(const char* key) const {
    const auto* a = attr(key);
    return a ? *a : std::string();
  }
};

class XmlParser {
 public:
  explicit XmlParser(const std::string& text) : s_(text) {}

  // Fills root; on failure returns false and error() says where.
  bool parse(XNode& root) {
    if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
    if (s_.compare(0, 2, "\xFF\xFE") == 0 || s_.compare(0, 2, "\xFE\xFF") == 0) return fail("UTF-16 is not supported; save the file as UTF-8");
    if (!valid_utf8(s_)) return fail("the file is not valid UTF-8");
    bool have_root = false;
    for (;;) {
      skip_ws();
      if (i_ >= s_.size()) break;
      if (s_[i_] != '<') return fail("text outside the root element");
      if (starts("<?")) {
        if (!skip_pi()) return false;
      } else if (starts("<!--")) {
        if (!skip_to("-->", "unterminated comment")) return false;
      } else if (starts("<!DOCTYPE")) {
        if (!skip_doctype()) return false;
      } else {
        if (have_root) return fail("more than one root element");
        if (!element(root, 0)) return false;
        have_root = true;
      }
    }
    if (!have_root) return fail("no root element");
    return true;
  }
  const std::string& error() const { return err_; }

 private:
  const std::string& s_;
  std::size_t i_ = 0;
  std::string err_;

  bool fail(const std::string& what) {
    if (!err_.empty()) return false;
    int line = 1;
    for (std::size_t k = 0; k < i_ && k < s_.size(); ++k)
      if (s_[k] == '\n') ++line;
    err_ = "malformed XML (line " + std::to_string(line) + "): " + what;
    return false;
  }
  bool starts(const char* t) const { return s_.compare(i_, std::char_traits<char>::length(t), t) == 0; }
  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool skip_to(const char* end, const char* what) {
    const auto at = s_.find(end, i_);
    if (at == std::string::npos) {
      i_ = s_.size();
      return fail(what);
    }
    i_ = at + std::char_traits<char>::length(end);
    return true;
  }
  bool skip_pi() {
    const std::size_t from = i_;
    if (!skip_to("?>", "unterminated processing instruction")) return false;
    const std::string decl = lower(s_.substr(from, i_ - from));
    if (decl.rfind("<?xml", 0) == 0) {
      const auto e = decl.find("encoding");
      if (e != std::string::npos) {
        const auto q = decl.find_first_of("\"'", e);
        if (q != std::string::npos) {
          const auto q2 = decl.find(decl[q], q + 1);
          const std::string enc = q2 == std::string::npos ? "" : decl.substr(q + 1, q2 - q - 1);
          if (enc != "utf-8" && enc != "utf8" && enc != "us-ascii") {
            i_ = from;
            return fail("unsupported encoding '" + enc + "'; save the file as UTF-8");
          }
        }
      }
    }
    return true;
  }
  bool skip_doctype() {
    int depth = 0;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '[') ++depth;
      else if (c == ']') --depth;
      else if (c == '>' && depth <= 0) return true;
    }
    return fail("unterminated DOCTYPE");
  }
  static bool name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.' || c == '-' || (c & 0x80);
  }
  bool read_name(std::string& out) {
    const std::size_t from = i_;
    while (i_ < s_.size() && name_char(s_[i_])) ++i_;
    if (i_ == from) return fail("expected a name");
    out = s_.substr(from, i_ - from);
    return true;
  }
  // Appends the text of s_[from, to) with character references resolved. Only the five predefined entities exist.
  bool decode(std::size_t from, std::size_t to, std::string& out) {
    for (std::size_t k = from; k < to; ++k) {
      if (s_[k] != '&') {
        out += s_[k];
        continue;
      }
      const auto semi = s_.find(';', k);
      if (semi == std::string::npos || semi >= to || semi - k > 12) {
        i_ = k;
        return fail("unterminated entity reference");
      }
      const std::string ent = s_.substr(k + 1, semi - k - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (!ent.empty() && ent[0] == '#') {
        char* end = nullptr;
        const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
        const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
        if (end == ent.c_str() + (hex ? 2 : 1) || *end || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
          i_ = k;
          return fail("invalid character reference &" + ent + ";");
        }
        append_utf8(out, static_cast<unsigned>(cp));
      } else {
        i_ = k;
        return fail("unknown entity &" + ent + ";");
      }
      k = semi;
    }
    return true;
  }

  bool element(XNode& n, int depth) {
    if (depth > 128) return fail("elements nested too deeply");
    ++i_;  // '<'
    if (!read_name(n.name)) return false;
    for (;;) {
      skip_ws();
      if (i_ >= s_.size()) return fail("unexpected end of file inside a tag");
      if (s_[i_] == '/') {
        if (!starts("/>")) return fail("stray '/' in a tag");
        i_ += 2;
        return true;
      }
      if (s_[i_] == '>') {
        ++i_;
        break;
      }
      std::string key, val;
      if (!read_name(key)) return false;
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != '=') return fail("attribute '" + key + "' has no value");
      ++i_;
      skip_ws();
      if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return fail("attribute value of '" + key + "' is not quoted");
      const char q = s_[i_++];
      const auto end = s_.find(q, i_);
      if (end == std::string::npos) return fail("unterminated attribute value");
      if (s_.find('<', i_) < end) return fail("'<' inside an attribute value");
      if (!decode(i_, end, val)) return false;
      i_ = end + 1;
      n.attrs.emplace_back(std::move(key), std::move(val));
    }
    for (;;) {
      const auto lt = s_.find('<', i_);
      if (lt == std::string::npos) return fail("element <" + n.name + "> is never closed");
      // text content is not used by this format; entity syntax inside it is still checked
      std::string ignored;
      if (!decode(i_, lt, ignored)) return false;
      i_ = lt;
      if (starts("</")) {
        i_ += 2;
        std::string close;
        if (!read_name(close)) return false;
        skip_ws();
        if (i_ >= s_.size() || s_[i_] != '>') return fail("malformed closing tag");
        ++i_;
        if (close != n.name) return fail("</" + close + "> closes <" + n.name + ">");
        return true;
      }
      if (starts("<!--")) {
        if (!skip_to("-->", "unterminated comment")) return false;
      } else if (starts("<![CDATA[")) {
        if (!skip_to("]]>", "unterminated CDATA section")) return false;
      } else if (starts("<?")) {
        if (!skip_to("?>", "unterminated processing instruction")) return false;
      } else if (starts("<!")) {
        return fail("unexpected markup declaration");
      } else {
        n.kids.emplace_back();
        if (!element(n.kids.back(), depth + 1)) return false;
      }
    }
  }
};

bool read_file(const std::string& path, std::string& out, std::string& error) {
  std::ifstream f(from_utf8(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  if (f.bad()) {
    error = "cannot read " + path;
    return false;
  }
  return true;
}

// Writes next to the target and renames, so a failed export never leaves half a file in place.
bool write_file_atomic(const std::string& path, const std::string& data, std::string& error) {
  std::error_code ec;
  const fs::path target = from_utf8(path);
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      error = "cannot write " + path;
      return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) {
      error = "cannot write " + path;
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    error = "cannot replace " + path;
    return false;
  }
  return true;
}

// ---- export ----------------------------------------------------------------------------------------------------------
void put_attr(std::string& out, const char* key, const std::string& value) {
  out += ' ';
  out += key;
  out += "=\"";
  out += xml_escape(value);
  out += '"';
}

std::string kind_text(const std::string& format) {
  std::string f;
  for (char c : format)
    if (std::isalnum(static_cast<unsigned char>(c))) f += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return f.empty() ? std::string() : f + " File";
}

struct ChildIndex {
  std::map<std::int64_t, std::vector<const PlaylistNode*>> kids;  // parent -> children in library order
};

void write_node(std::string& out, const PlaylistNode& n, const ChildIndex& idx, Library& lib,
                const std::unordered_map<std::int64_t, int>& key_of, int depth, InterchangeExportStats& st) {
  const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
  ++st.playlists;
  if (n.is_folder) {
    const auto it = idx.kids.find(n.id);
    const std::size_t count = it == idx.kids.end() ? 0 : it->second.size();
    out += pad + "<NODE Type=\"0\"";
    put_attr(out, "Name", n.name);
    out += " Count=\"" + std::to_string(count) + "\"";
    if (!count) {
      out += "/>\n";
      return;
    }
    out += ">\n";
    for (const PlaylistNode* c : it->second) write_node(out, *c, idx, lib, key_of, depth + 1, st);
    out += pad + "</NODE>\n";
    return;
  }
  std::vector<int> keys;
  for (const auto tid : lib.playlist_track_ids(n.id)) {
    const auto k = key_of.find(tid);
    if (k != key_of.end()) keys.push_back(k->second);
  }
  out += pad + "<NODE Type=\"1\"";
  put_attr(out, "Name", n.name);
  out += " KeyType=\"0\" Entries=\"" + std::to_string(keys.size()) + "\"";
  if (keys.empty()) {
    out += "/>\n";
    return;
  }
  out += ">\n";
  for (int k : keys) out += pad + "  <TRACK Key=\"" + std::to_string(k) + "\"/>\n";
  out += pad + "</NODE>\n";
}

// ---- import: parsed form ---------------------------------------------------------------------------------------------
struct XMark {
  int type = 0, num = -1;
  double start_ms = 0, end_ms = -1;
  int color = -1;
  std::string name;
};
struct XTrack {
  std::string id, path;
  std::string title, artist, album, genre, key, comment;
  double bpm = 0;
  int rating = 0, color = 0, bitrate = 0, play_count = 0;
  std::int64_t date_added = 0, duration_ms = 0;
  std::vector<BeatSection> grid;
  std::vector<XMark> marks;
};

struct Collector {
  InterchangeStats& st;
  void warn(const std::string& w) {
    if (st.warnings.size() < 50) st.warnings.push_back(w);
  }
};

bool parse_color(const std::string& s, int& out) {
  std::string t = s;
  if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) t = t.substr(2);
  else if (!t.empty() && t[0] == '#') t = t.substr(1);
  else return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(t.c_str(), &end, 16);
  if (t.empty() || *end || v > 0xFFFFFF) return false;
  out = static_cast<int>(v);
  return true;
}

XTrack read_track(const XNode& n, Collector& c, int& ignored) {
  XTrack t;
  t.id = n.str("TrackID");
  t.path = interchange_url_to_path(n.str("Location"));
  t.title = n.str("Name");
  t.artist = n.str("Artist");
  t.album = n.str("Album");
  t.genre = n.str("Genre");
  t.key = n.str("Tonality");
  t.comment = n.str("Comments");
  double v;
  if (parse_num(n.attr("AverageBpm"), v) && v > 0 && v < 1000) t.bpm = v;
  if (parse_num(n.attr("Rating"), v) && v > 0) t.rating = std::clamp(static_cast<int>(std::lround(v / 51.0)), 0, 5);
  if (parse_num(n.attr("TotalTime"), v) && v > 0 && v < 1e6) t.duration_ms = std::llround(v * 1000.0);
  if (parse_num(n.attr("BitRate"), v) && v > 0 && v < 1e7) t.bitrate = static_cast<int>(v);
  if (parse_num(n.attr("PlayCount"), v) && v > 0 && v < 1e7) t.play_count = static_cast<int>(v);
  t.date_added = parse_date(n.str("DateAdded"));
  parse_color(n.str("Colour"), t.color);
  for (const XNode& k : n.kids) {
    if (k.name == "TEMPO") {
      double inizio = 0, bpm = 0, beat = 1;
      if (!parse_num(k.attr("Inizio"), inizio) || !parse_num(k.attr("Bpm"), bpm) || bpm <= 0 || bpm >= 1000 || inizio < -3600 ||
          inizio > 1e6) {
        c.warn("track " + n.str("TrackID") + ": a TEMPO entry was malformed and skipped");
        continue;
      }
      parse_num(k.attr("Battito"), beat);
      BeatSection b;
      b.first_beat_ms = round_ms(inizio * 1000.0);
      b.bpm = bpm;
      b.bar_beat = std::clamp(static_cast<int>(beat), 1, 4);
      t.grid.push_back(b);
    } else if (k.name == "POSITION_MARK") {
      XMark m;
      double s = 0, e = 0, num = -1, ty = 0;
      parse_num(k.attr("Type"), ty);
      parse_num(k.attr("Num"), num);
      if (!parse_num(k.attr("Start"), s) || s < 0 || s > 1e6) {
        ++ignored;
        continue;
      }
      m.type = static_cast<int>(ty);
      m.num = static_cast<int>(num);
      m.start_ms = round_ms(s * 1000.0);
      if (parse_num(k.attr("End"), e) && e > s && e < 1e6) m.end_ms = round_ms(e * 1000.0);
      m.name = k.str("Name");
      double r, g, b;
      if (parse_num(k.attr("Red"), r) && parse_num(k.attr("Green"), g) && parse_num(k.attr("Blue"), b) && r >= 0 && r <= 255 &&
          g >= 0 && g <= 255 && b >= 0 && b <= 255)
        m.color = (static_cast<int>(r) << 16) | (static_cast<int>(g) << 8) | static_cast<int>(b);
      // only plain cues (0) and loops (4) are carried; fade-in / fade-out / load marks have no DeckHand equivalent
      if ((m.type != 0 && m.type != 4) || m.num < -1 || m.num > 63) {
        ++ignored;
        continue;
      }
      t.marks.push_back(std::move(m));
    }
  }
  return t;
}

struct ParsedDoc {
  std::vector<XTrack> tracks;
  const XNode* playlists = nullptr;
  int ignored_marks = 0;
};

// ---- import: apply ---------------------------------------------------------------------------------------------------
struct Importer {
  Importer(Library& l, const InterchangeImportOptions& o, InterchangeStats& s) : lib(l), opt(o), st(s) {}
  Library& lib;
  const InterchangeImportOptions& opt;
  InterchangeStats& st;
  Collector col{st};
  std::unordered_map<std::string, std::int64_t> id_of_key;    // TrackID -> library id
  std::unordered_map<std::string, std::int64_t> id_of_path;   // Location path -> library id
  std::set<std::int64_t> claimed;                             // library tracks matched by an entry of this file
  struct Cand { std::int64_t id; std::int64_t duration_ms; std::string path; };
  std::unordered_map<std::string, std::vector<Cand>> by_name;
  bool by_name_built = false;
  std::int64_t history_session = 0;
  std::vector<PlaylistNode> pls;
  std::set<std::int64_t> claimed_pl;

  void build_name_index() {
    by_name_built = true;
    for (const TrackRow& r : lib.tracks(0, ""))
      by_name[lower(base_name(r.path))].push_back({r.id, r.duration_ms, r.path});
  }

  std::optional<std::int64_t> match_by_name(const XTrack& x) {
    if (!by_name_built) build_name_index();
    const auto it = by_name.find(lower(base_name(x.path)));
    if (it == by_name.end()) return std::nullopt;
    std::optional<std::int64_t> found;
    for (const Cand& c : it->second) {
      if (claimed.count(c.id)) continue;
      if (x.duration_ms && c.duration_ms && std::llabs(c.duration_ms - x.duration_ms) > 1000) continue;
      std::error_code ec;
      if (fs::exists(from_utf8(c.path), ec)) continue;  // that file is still where it was: a different track
      if (found) return std::nullopt;                    // ambiguous
      found = c.id;
    }
    return found;
  }

  // Fills (or with the overwrite options replaces) what the file carries. Returns true when something was written.
  bool fill(std::int64_t id, const XTrack& x, bool is_new) {
    const TrackRow row = *lib.track(id);
    bool changed = false;
    const bool ow = opt.overwrite_metadata;
    // A field the user or the API edited since it was last imported is left alone, unless overwrite_metadata is set.
    const std::set<std::string> keep = ow ? std::set<std::string>() : lib.protected_fields(id);
    const auto text = [&](const std::string& have, const std::string& want, const char* field, std::optional<std::string> TrackEdit::*member) {
      if (want.empty() || want == have || !(ow || have.empty()) || keep.count(field)) return;
      TrackEdit e;
      e.*member = want;
      lib.edit_track(id, e);
      changed = true;
    };
    if (!is_new) {
      text(row.title, x.title, "title", &TrackEdit::title);
      text(row.artist, x.artist, "artist", &TrackEdit::artist);
      text(row.album, x.album, "album", &TrackEdit::album);
      text(row.genre, x.genre, "genre", &TrackEdit::genre);
      text(row.comment, x.comment, "comment", &TrackEdit::comment);
      if (x.rating > 0 && x.rating != row.rating && (ow || row.rating == 0) && !keep.count("rating")) {
        TrackEdit e;
        e.rating = x.rating;
        lib.edit_track(id, e);
        changed = true;
      }
      if (x.bpm > 0 && x.bpm != row.bpm && (ow || row.bpm <= 0) && !keep.count("bpm")) {
        lib.set_bpm(id, x.bpm);
        changed = true;
      }
    }
    if (!x.key.empty() && x.key != row.key && (ow || row.key.empty()) && !keep.count("key")) {
      lib.set_key(id, x.key);
      changed = true;
    }
    if (x.color > 0 && x.color != row.color && (ow || row.color == 0) && !keep.count("color")) {
      lib.set_track_color(id, x.color);
      changed = true;
    }
    if (x.date_added > 0 && x.date_added != row.date_added && (is_new || ow)) {
      lib.set_date_added(id, x.date_added);
      changed = true;
    }
    if (!x.grid.empty() && (opt.overwrite_grid || lib.beatgrid(id).empty())) {
      lib.replace_beatgrid(id, x.grid);
      ++st.grids;
      changed = true;
    }
    if (!x.marks.empty() && apply_marks(id, x)) changed = true;
    if (opt.import_play_counts && x.play_count > row.play_count) {
      if (!history_session) history_session = lib.begin_history_session();
      for (int i = row.play_count; i < x.play_count; ++i) {
        lib.add_history(history_session, id, "import", 0);
        ++st.history_entries;
      }
      changed = true;
    }
    return changed;
  }

  bool apply_marks(std::int64_t id, const XTrack& x) {
    std::map<int, const XMark*> hot;  // slot -> mark (the last one of a slot wins)
    std::vector<Cue> memory;
    for (const XMark& m : x.marks) {
      if (m.num >= 0) {
        hot[m.num] = &m;
        continue;
      }
      Cue c;
      c.kind = m.type == 4 && m.end_ms > m.start_ms ? CueKind::Loop : CueKind::Memory;
      c.start_ms = m.start_ms;
      c.end_ms = c.kind == CueKind::Loop ? m.end_ms : -1;
      c.label = m.name;
      c.color = m.color;
      memory.push_back(std::move(c));
    }
    const auto have = lib.cues(id);
    std::set<int> slots;
    bool has_memory = false;
    for (const Cue& c : have) {
      if (c.slot >= 0) slots.insert(c.slot);
      else has_memory = true;
    }
    bool changed = false;
    if (opt.overwrite_cues && !hot.empty()) {
      for (int s : slots) lib.clear_hot_cue(id, s);
      slots.clear();
    }
    for (const auto& [slot, m] : hot) {
      if (slots.count(slot)) continue;
      lib.set_hot_cue(id, slot, m->start_ms, m->type == 4 && m->end_ms > m->start_ms ? m->end_ms : -1);
      if (!m->name.empty() || m->color >= 0) lib.set_hot_cue_style(id, slot, m->name, m->color);
      ++st.cues;
      changed = true;
    }
    if (!memory.empty() && (opt.overwrite_cues || !has_memory)) {
      lib.replace_memory_cues(id, memory);
      st.cues += static_cast<int>(memory.size());
      changed = true;
    }
    return changed;
  }

  void tracks(const std::vector<XTrack>& list) {
    for (const XTrack& x : list) {
      ++st.tracks_in_file;
      if (x.path.empty()) {
        col.warn("entry " + std::to_string(st.tracks_in_file) + " has no Location and was skipped");
        continue;
      }
      std::error_code ec;
      if (!fs::exists(from_utf8(x.path), ec)) {
        ++st.tracks_missing_file;
        if (st.missing_paths.size() < 200) st.missing_paths.push_back(x.path);
      }
      std::optional<std::int64_t> id = lib.track_id_for_path(x.path);
      bool by_path = id.has_value(), by_name = false;
      if (!id && opt.match_by_name) {
        id = match_by_name(x);
        by_name = id.has_value();
      }
      if (id) {
        const bool first_claim = claimed.insert(*id).second;
        if (!first_claim && !by_path) {
          id.reset();  // cannot happen for by-name (claimed ones are skipped); kept for safety
        }
      }
      if (id) {
        by_path ? ++st.tracks_matched_by_path : ++st.tracks_matched_by_name;
        if (!id_of_key.count(x.id)) id_of_key[x.id] = *id;
        id_of_path[x.path] = *id;
        if (!opt.merge) {
          ++st.tracks_skipped;
          continue;
        }
        bool changed = false;
        if (by_name) {
          std::error_code e2;
          if (fs::exists(from_utf8(x.path), e2)) {  // the file moved: follow it
            lib.set_track_path(*id, x.path);
            changed = true;
          }
        }
        if (fill(*id, x, false)) changed = true;
        if (changed) ++st.tracks_updated;
        continue;
      }
      NewTrack nt;
      nt.path = x.path;
      nt.title = x.title.empty() ? stem_of(x.path) : x.title;
      nt.artist = x.artist;
      nt.album = x.album;
      nt.genre = x.genre;
      nt.key = x.key;
      nt.comment = x.comment;
      nt.bpm = x.bpm;
      nt.duration_ms = x.duration_ms;
      nt.rating = x.rating;
      nt.bitrate = x.bitrate;
      const std::int64_t nid = lib.upsert_track(nt);
      claimed.insert(nid);
      ++st.tracks_added;
      if (!id_of_key.count(x.id)) id_of_key[x.id] = nid;
      id_of_path[x.path] = nid;
      fill(nid, x, true);
    }
  }

  std::int64_t find_or_create(std::int64_t parent, const std::string& name, bool folder, bool& existed) {
    for (const PlaylistNode& p : pls)
      if (p.parent == parent && p.name == name && p.is_folder == folder && !claimed_pl.count(p.id)) {
        claimed_pl.insert(p.id);
        existed = true;
        return p.id;
      }
    existed = false;
    const std::int64_t id = lib.create_playlist(parent, name, folder);
    PlaylistNode n;
    n.id = id;
    n.parent = parent;
    n.name = name;
    n.is_folder = folder;
    pls.push_back(n);
    claimed_pl.insert(id);
    return id;
  }

  void node(const XNode& n, std::int64_t parent, int depth) {
    for (const XNode& c : n.kids) {
      if (c.name != "NODE") continue;
      std::string name = c.str("Name");
      if (name.empty()) name = "Untitled";
      const std::string type = c.str("Type");
      bool folder;
      if (type == "0") folder = true;
      else if (type == "1") folder = false;
      else {
        folder = true;
        for (const XNode& k : c.kids)
          if (k.name == "TRACK") folder = false;
      }
      bool existed = false;
      const std::int64_t id = find_or_create(parent, name, folder, existed);
      existed ? ++st.playlists_merged : ++st.playlists_created;
      if (folder) {
        if (depth < 64) node(c, id, depth + 1);
        continue;
      }
      if (existed && !opt.merge) continue;
      const bool by_location = c.str("KeyType") == "1";
      std::vector<std::int64_t> want;
      for (const XNode& t : c.kids) {
        if (t.name != "TRACK") continue;
        const std::string key = t.str("Key");
        std::optional<std::int64_t> tid;
        if (by_location) {
          const std::string p = interchange_url_to_path(key);
          const auto it = id_of_path.find(p);
          if (it != id_of_path.end()) tid = it->second;
          else tid = lib.track_id_for_path(p);
        } else {
          const auto it = id_of_key.find(key);
          if (it != id_of_key.end()) tid = it->second;
        }
        if (!tid) {
          ++st.playlist_entries_unresolved;
          continue;
        }
        want.push_back(*tid);
      }
      if (!existed) {
        lib.set_playlist_tracks(id, want);
        st.playlist_entries_added += static_cast<int>(want.size());
      } else {
        auto have = lib.playlist_track_ids(id);
        std::set<std::int64_t> present(have.begin(), have.end());
        bool any = false;
        for (const auto tid : want)
          if (present.insert(tid).second) {
            have.push_back(tid);
            ++st.playlist_entries_added;
            any = true;
          }
        if (any) lib.set_playlist_tracks(id, have);
      }
    }
  }
};

}  // namespace

// ---- URLs ------------------------------------------------------------------------------------------------------------
std::string interchange_path_to_url(const std::string& path) {
  std::string p = path;
  const bool win = looks_windows(p);
  if (win)
    for (char& c : p)
      if (c == '\\') c = '/';
  std::string out = "file://localhost";
  if (p.empty() || p[0] != '/') out += '/';
  static const char* hex = "0123456789ABCDEF";
  for (std::size_t i = 0; i < p.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    const bool drive_colon = win && i == 1 && c == ':';
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || drive_colon) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string interchange_url_to_path(const std::string& url) {
  std::string rest;
  if (lower(url.substr(0, 7)) != "file://") return url;
  rest = url.substr(7);
  if (!rest.empty() && rest[0] != '/') {  // a host name: drop it
    const auto slash = rest.find('/');
    rest = slash == std::string::npos ? std::string() : rest.substr(slash);
  }
  std::string out;
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() + 0 && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
      out += static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += rest[i];
    }
  }
  if (out.size() >= 3 && out[0] == '/' && std::isalpha(static_cast<unsigned char>(out[1])) && out[2] == ':' &&
      (out.size() == 3 || out[3] == '/'))
    out.erase(0, 1);  // "/C:/Music" -> "C:/Music"
#ifdef _WIN32
  std::replace(out.begin(), out.end(), '/', '\\');  // native separators, so paths match the library's
#endif
  return out;
}

// ---- export ----------------------------------------------------------------------------------------------------------
InterchangeExportStats export_library_xml(Library& lib, const std::string& path, const InterchangeExportOptions& opt) {
  InterchangeExportStats st;
  try {
    const std::vector<PlaylistNode> all = lib.playlists();
    ChildIndex idx;
    for (const PlaylistNode& p : all) idx.kids[p.parent].push_back(&p);

    // the playlist subtree to write, and the tracks to put in the collection
    const PlaylistNode* top = nullptr;
    std::vector<std::int64_t> ids;
    if (opt.playlist_id) {
      for (const PlaylistNode& p : all)
        if (p.id == opt.playlist_id) top = &p;
      if (!top) {
        st.error = "no such playlist";
        return st;
      }
      std::set<std::int64_t> set;
      std::vector<const PlaylistNode*> stack{top};
      while (!stack.empty()) {
        const PlaylistNode* n = stack.back();
        stack.pop_back();
        if (n->is_folder) {
          const auto it = idx.kids.find(n->id);
          if (it != idx.kids.end())
            for (const PlaylistNode* c : it->second) stack.push_back(c);
        } else {
          for (const auto t : lib.playlist_track_ids(n->id)) set.insert(t);
        }
      }
      ids.assign(set.begin(), set.end());
    } else {
      ids = lib.track_ids(0, "");
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    std::string out;
    out.reserve(ids.size() * 600 + 1024);
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DJ_PLAYLISTS Version=\"1.0.0\">\n";
    out += "  <PRODUCT Name=\"DeckHand\" Version=\"1\" Company=\"\"/>\n";
    out += "  <COLLECTION Entries=\"@N@\">\n";
    std::unordered_map<std::int64_t, int> key_of;
    int written = 0;
    for (const std::int64_t id : ids) {
      const auto row = lib.track(id);
      if (!row) continue;
      key_of[id] = ++written;
      out += "    <TRACK TrackID=\"" + std::to_string(written) + "\"";
      put_attr(out, "Name", row->title);
      if (!row->artist.empty()) put_attr(out, "Artist", row->artist);
      if (!row->album.empty()) put_attr(out, "Album", row->album);
      if (!row->genre.empty()) put_attr(out, "Genre", row->genre);
      const std::string kind = kind_text(row->format);
      if (!kind.empty()) put_attr(out, "Kind", kind);
      std::error_code ec;
      const auto size = fs::file_size(from_utf8(row->path), ec);
      if (!ec) out += " Size=\"" + std::to_string(size) + "\"";
      if (row->duration_ms > 0) out += " TotalTime=\"" + std::to_string((row->duration_ms + 500) / 1000) + "\"";
      if (row->bpm > 0) out += " AverageBpm=\"" + fixed(row->bpm, 2) + "\"";
      if (row->date_added > 0) put_attr(out, "DateAdded", date_text(row->date_added));
      if (row->bitrate > 0) out += " BitRate=\"" + std::to_string(row->bitrate) + "\"";
      if (!row->comment.empty()) put_attr(out, "Comments", row->comment);
      if (row->play_count > 0) out += " PlayCount=\"" + std::to_string(row->play_count) + "\"";
      if (row->rating > 0) out += " Rating=\"" + std::to_string(std::clamp(row->rating, 0, 5) * 51) + "\"";
      if (row->color > 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%06X", row->color & 0xFFFFFF);
        out += std::string(" Colour=\"") + buf + "\"";
      }
      if (!row->key.empty()) put_attr(out, "Tonality", row->key);
      put_attr(out, "Location", interchange_path_to_url(row->path));

      std::string kids;
      if (opt.include_grid)
        for (const BeatSection& g : lib.beatgrid(id))
          kids += "      <TEMPO Inizio=\"" + fixed(g.first_beat_ms / 1000.0, 5) + "\" Bpm=\"" + fixed(g.bpm, 4) +
                  "\" Metro=\"4/4\" Battito=\"" + std::to_string(std::clamp(g.bar_beat, 1, 4)) + "\"/>\n";
      if (opt.include_cues) {
        std::vector<Cue> cues = lib.cues(id);
        std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
          const bool ah = a.slot >= 0, bh = b.slot >= 0;
          if (ah != bh) return ah;  // hot cues first, by slot; then memory cues by position
          if (ah) return a.slot < b.slot;
          if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
          return a.end_ms < b.end_ms;
        });
        for (const Cue& c : cues) {
          const bool loop = c.end_ms > c.start_ms;
          kids += "      <POSITION_MARK";
          put_attr(kids, "Name", c.label);
          kids += std::string(" Type=\"") + (loop ? "4" : "0") + "\" Start=\"" + fixed(c.start_ms / 1000.0, 3) + "\"";
          if (loop) kids += " End=\"" + fixed(c.end_ms / 1000.0, 3) + "\"";
          kids += " Num=\"" + std::to_string(c.slot >= 0 ? c.slot : -1) + "\"";
          if (c.color >= 0)
            kids += " Red=\"" + std::to_string((c.color >> 16) & 255) + "\" Green=\"" + std::to_string((c.color >> 8) & 255) +
                    "\" Blue=\"" + std::to_string(c.color & 255) + "\"";
          kids += "/>\n";
          ++st.cues;
        }
      }
      if (kids.empty()) {
        out += "/>\n";
      } else {
        out += ">\n" + kids + "    </TRACK>\n";
      }
    }
    out += "  </COLLECTION>\n";
    st.tracks = written;
    const auto at = out.find("@N@");
    out.replace(at, 3, std::to_string(written));

    out += "  <PLAYLISTS>\n";
    std::string body;
    if (opt.include_playlists) {
      if (top) {
        write_node(body, *top, idx, lib, key_of, 3, st);
      } else {
        const auto it = idx.kids.find(0);
        if (it != idx.kids.end())
          for (const PlaylistNode* c : it->second) write_node(body, *c, idx, lib, key_of, 3, st);
      }
    }
    const int top_count = top ? 1 : static_cast<int>(idx.kids.count(0) ? idx.kids[0].size() : 0);
    out += "    <NODE Type=\"0\" Name=\"ROOT\" Count=\"" + std::to_string(opt.include_playlists ? top_count : 0) + "\"";
    if (body.empty()) {
      out += "/>\n";
    } else {
      out += ">\n" + body + "    </NODE>\n";
    }
    out += "  </PLAYLISTS>\n</DJ_PLAYLISTS>\n";
    if (!write_file_atomic(path, out, st.error)) return st;
    st.ok = true;
  } catch (const std::exception& e) {
    st.ok = false;
    st.error = std::string("export failed: ") + e.what();
  }
  return st;
}

// ---- import ----------------------------------------------------------------------------------------------------------
InterchangeStats import_library_xml(Library& lib, const std::string& path, const InterchangeImportOptions& opt) {
  InterchangeStats st;
  st.dry_run = opt.dry_run;
  try {
    std::string text;
    if (!read_file(path, text, st.error)) return st;
    XNode root;
    {
      XmlParser parser(text);
      if (!parser.parse(root)) {
        st.error = parser.error();
        return st;
      }
    }
    if (root.name != "DJ_PLAYLISTS") {
      st.error = "not a DJ library XML file (the root element is <" + root.name + ">, expected <DJ_PLAYLISTS>)";
      return st;
    }
    Collector col{st};
    ParsedDoc doc;
    for (const XNode& k : root.kids) {
      if (k.name == "COLLECTION") {
        for (const XNode& t : k.kids)
          if (t.name == "TRACK") doc.tracks.push_back(read_track(t, col, doc.ignored_marks));
      } else if (k.name == "PLAYLISTS" && !doc.playlists) {
        doc.playlists = &k;
      }
    }
    st.marks_ignored = doc.ignored_marks;

    EditScope import_scope("import");  // changes land in the edit journal as imports
    Transaction tx(lib.db());  // rolls back on any exception, and on a dry run
    Importer im(lib, opt, st);
    im.pls = lib.playlists();
    im.tracks(doc.tracks);
    if (opt.import_playlists && doc.playlists)
      for (const XNode& top : doc.playlists->kids)  // the ROOT node holds the tree; its own name is not a folder
        if (top.name == "NODE") im.node(top, 0, 0);
    if (!opt.dry_run) tx.commit();
    st.ok = true;
    DH_LOG(Info, "interchange xml: %d added, %d matched, %d playlists%s", st.tracks_added,
           st.tracks_matched_by_path + st.tracks_matched_by_name, st.playlists_created, opt.dry_run ? " (dry run)" : "");
  } catch (const std::exception& e) {
    st = InterchangeStats{};
    st.dry_run = opt.dry_run;
    st.error = std::string("import failed, nothing was changed: ") + e.what();
  }
  return st;
}

}  // namespace deckhand
