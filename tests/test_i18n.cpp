// i18n: fallback chain, plurals, number formats, placeholder validation, shipped packs, runtime switch.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "check.hpp"
#include "deckhand/i18n/i18n.hpp"

using namespace deckhand::i18n;

static Pack make(const char* tag, std::initializer_list<std::pair<const char*, const char*>> kv,
                 std::vector<std::string> fb = {}) {
  Pack p;
  p.manifest.tag = tag;
  p.manifest.name = tag;
  p.manifest.fallback = std::move(fb);
  for (auto& e : kv) p.strings[e.first] = e.second;
  return p;
}

static std::string plural_of(const char* lang, double n) { return plural_name(plural_category(lang, n)); }

int main() {
  // ---- plural rules
  CHECK(plural_of("en", 1) == "one" && plural_of("en", 0) == "other" && plural_of("en", 2) == "other");
  CHECK(plural_of("fr", 0) == "one" && plural_of("fr", 1) == "one" && plural_of("fr", 2) == "other");
  CHECK(plural_of("fr", 1000000) == "many");
  CHECK(plural_of("de", 1) == "one" && plural_of("es", 1) == "one" && plural_of("es", 0) == "other");
  CHECK(plural_of("it", 1) == "one" && plural_of("nl", 5) == "other");
  CHECK(plural_of("pt", 0) == "one" && plural_of("pt-BR", 1) == "one" && plural_of("pt", 2) == "other");
  CHECK(plural_of("ru", 1) == "one" && plural_of("ru", 21) == "one" && plural_of("ru", 11) == "many");
  CHECK(plural_of("ru", 3) == "few" && plural_of("ru", 13) == "many" && plural_of("ru", 25) == "many");
  CHECK(plural_of("pl", 1) == "one" && plural_of("pl", 22) == "few" && plural_of("pl", 12) == "many" && plural_of("pl", 5) == "many");
  CHECK(plural_of("cs", 1) == "one" && plural_of("cs", 3) == "few" && plural_of("cs", 5) == "other" && plural_of("cs", 1.5) == "many");
  CHECK(plural_of("ja", 1) == "other" && plural_of("zh", 1) == "other" && plural_of("ko", 7) == "other");
  CHECK(plural_of("tr", 1) == "one" && plural_of("tr", 2) == "other");
  CHECK(plural_of("ar", 0) == "zero" && plural_of("ar", 1) == "one" && plural_of("ar", 2) == "two");
  CHECK(plural_of("ar", 5) == "few" && plural_of("ar", 11) == "many" && plural_of("ar", 100) == "other");
  CHECK(plural_of("he", 1) == "one" && plural_of("he", 2) == "two" && plural_of("he", 5) == "other");

  // ---- number formatting
  CHECK(format_number(1234567.5, "en") == "1,234,567.5");
  CHECK(format_number(1234567.5, "de") == "1.234.567,5");
  CHECK(format_number(1234.5, "fr") == "1\xE2\x80\xAF" "234,5");
  CHECK(format_number(1234, "es") == "1234" && format_number(12345, "es") == "12.345");
  CHECK(format_number(-0.25, "en") == "-0.25" && format_number(3, "en", 2, 2) == "3.00");
  CHECK(format_number(12.3456, "ja") == "12.346");

  // ---- message format
  CHECK(format_message("Hi {name}!", {{"name", "Bo"}}, "en") == "Hi Bo!");
  CHECK(format_message("Hi {name}!", {}, "en") == "Hi {name}!");
  const char* pl = "{n, plural, =0 {none} one {# file} other {# files}}";
  CHECK(format_message(pl, {{"n", 0}}, "en") == "none");
  CHECK(format_message(pl, {{"n", 1}}, "en") == "1 file");
  CHECK(format_message(pl, {{"n", 1500}}, "en") == "1,500 files");
  CHECK(format_message(pl, {{"n", 1500}}, "de") == "1.500 files");
  CHECK(format_message("{n, plural, one {# книга} few {# книги} many {# книг} other {# книги}}", {{"n", 22}}, "ru") == "22 книги");
  CHECK(format_message("{g, select, m {he} f {she} other {they}}", {{"g", "f"}}, "en") == "she");
  CHECK(format_message("{g, select, m {he} other {they}}", {{"g", "x"}}, "en") == "they");
  CHECK(format_message("a \\{b\\} {x}", {{"x", 2}}, "en") == "a {b} 2");
  CHECK(format_message("{a} {n, plural, one {{a} x} other {{a} xs}}", {{"a", "k"}, {"n", 1}}, "en") == "k k x");
  CHECK(format_message("{v, number, 1}", {{"v", 44.1}}, "de") == "44,1");
  CHECK(format_message("broken {oops", {}, "en") == "broken {oops");  // malformed: literal, no crash

  // ---- placeholders and syntax
  CHECK(placeholders("{a} and {n, plural, one {{b}} other {#}}") == (std::set<std::string>{"a", "n", "b"}));
  CHECK(check_message_syntax("ok {a}").empty());
  CHECK(!check_message_syntax("{a").empty());
  CHECK(!check_message_syntax("a}").empty());
  CHECK(!check_message_syntax("{n, plural, one {x}}").empty());  // lacks other
  CHECK(!check_message_syntax("{n, wibble}").empty());

  // ---- fallback chain: regional -> base -> manifest fallback -> English -> id
  {
    Catalog c;
    c.add_pack(make("en", {{"a", "en-a"}, {"b", "en-b"}, {"c", "en-c"}, {"d", "en-d"}}));
    c.add_pack(make("pt", {{"a", "pt-a"}, {"b", "pt-b"}}));
    c.add_pack(make("pt-BR", {{"a", "br-a"}}));
    c.add_pack(make("gl", {{"a", "gl-a"}, {"c", "gl-c"}}, {"es"}));
    c.add_pack(make("es", {{"a", "es-a"}, {"c", "es-c"}, {"d", "es-d"}}));
    CHECK(c.language() == "en" && c.tr("a") == "en-a");
    CHECK(c.set_language("pt_br"));
    CHECK(c.language() == "pt-BR");
    CHECK(c.tr("a") == "br-a" && c.tr("b") == "pt-b" && c.tr("c") == "en-c");
    CHECK(c.tr("missing.id") == "missing.id");
    CHECK(c.set_language("pt-PT"));  // no pt-PT pack: parent pt serves
    CHECK(c.tr("a") == "pt-a");
    CHECK(c.set_language("gl"));
    CHECK(c.tr("a") == "gl-a" && c.tr("d") == "es-d" && c.tr("b") == "en-b");
    CHECK(!c.set_language("xx") && c.language() == "gl");
    CHECK(c.has("a") && !c.has("zzz"));
    CHECK(c.available().size() == 5);
  }

  // ---- shipped packs
  Catalog cat;
  CHECK(cat.load_builtin("resources/lang/en.dhlang"));
  std::vector<std::string> errs;
  CHECK(cat.load_directory("resources/lang", &errs) >= 5);
  CHECK(errs.empty());
  auto en = cat.pack("en");
  CHECK(en && en->strings.size() > 40);
  CHECK(!cat.help("getting-started").empty());
  for (const char* tag : {"de", "fr", "es", "ja"}) {
    auto p = cat.pack(tag);
    CHECK(p);
    Report r = validate(*p, *en);
    for (auto& s : r.missing) std::fprintf(stderr, "%s missing %s\n", tag, s.c_str());
    for (auto& s : r.extra) std::fprintf(stderr, "%s extra %s\n", tag, s.c_str());
    for (auto& s : r.placeholder_mismatch) std::fprintf(stderr, "%s %s\n", tag, s.c_str());
    for (auto& s : r.syntax_errors) std::fprintf(stderr, "%s %s\n", tag, s.c_str());
    CHECK(r.ok());
    CHECK(p->warnings.empty());
  }
  // English validates against itself
  CHECK(validate(*en, *en).ok());

  // validator catches defects
  {
    Pack bad = make("xx", {{"deck.empty", "Leer"}, {"browser.tracks", "{n, plural, one {#} other {#}}"}, {"bogus.id", "x"}});
    Report r = validate(bad, *en);
    CHECK(!r.ok());
    CHECK(!r.missing.empty());
    CHECK(r.extra.size() == 1 && r.extra[0] == "bogus.id");
    CHECK(r.placeholder_mismatch.size() == 2);  // deck.empty lacks {deck}; tracks uses n not count
  }

  // ---- rendering through shipped packs
  Args five{{"count", 5}};
  CHECK(cat.tr("browser.tracks", {{"count", 1}}) == "1 track");
  CHECK(cat.tr("browser.tracks", five) == "5 tracks");
  CHECK(cat.set_language("de"));
  CHECK(cat.tr("browser.col.bpm") == "BPM" && cat.tr("browser.col.artist") == "KÜNSTLER");
  CHECK(cat.tr("deck.empty", {{"deck", "A"}}) == "Deck A ist leer");
  CHECK(cat.tr("app.audio", {{"device", "X"}, {"channels", 2}, {"khz", 44.1}}) == "X  2 Kanäle  44,1 kHz");
  CHECK(cat.set_language("fr"));
  CHECK(cat.tr("browser.tracks", {{"count", 0}}) == "0 morceau");  // CLDR: 0 is singular in French
  CHECK(cat.tr("browser.tracks", {{"count", 1234}}) == "1\xE2\x80\xAF" "234 morceaux");
  CHECK(cat.set_language("ja"));
  CHECK(cat.tr("browser.tracks", {{"count", 3}}) == "3曲");
  CHECK(cat.tr("deck.play") == "再生");
  CHECK(cat.set_language("es"));
  CHECK(cat.tr("browser.tracks", {{"count", 0}}) == "0 pistas");
  CHECK(cat.direction() == Direction::Ltr);

  // ---- RTL manifest
  {
    Pack ar = make("ar", {{"deck.play", "تشغيل"}});
    ar.manifest.dir = Direction::Rtl;
    cat.add_pack(ar);
    CHECK(cat.set_language("ar"));
    CHECK(cat.direction() == Direction::Rtl);
    CHECK(cat.tr("deck.play") == "تشغيل" && cat.tr("deck.pause") == "PAUSE");  // falls back to English
  }

  // ---- parse errors
  {
    std::string e;
    CHECK(!parse_pack("deck.play = x\n", &e) && !e.empty());          // no @lang
    CHECK(!parse_pack("@lang en\nno equals here\n", &e));
    CHECK(!parse_pack("@lang en\nba d = x\n", &e));
    CHECK(!load_pack("resources/lang/nonexistent.dhlang", &e));
    auto ok = parse_pack("@lang pt_br\n# c\nx.y = a\\nb\n", &e);
    CHECK(ok && ok->manifest.tag == "pt-BR" && ok->strings["x.y"] == "a\nb");
  }

  // ---- runtime switch while readers run
  {
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread reader([&] {
      while (!stop) {
        std::string s = cat.tr("browser.col.bpm");
        std::string p = cat.tr("deck.play");
        if (s != "BPM" || p.empty() || p == "deck.play") ++bad;
      }
    });
    for (int i = 0; i < 2000; ++i) cat.set_language(i % 2 ? "de" : "ja");
    stop = true;
    reader.join();
    CHECK(bad == 0);
  }
  std::puts("test_i18n OK");
  return 0;
}
