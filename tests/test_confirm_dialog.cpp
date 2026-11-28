// The reusable confirmation: requests from any thread, one shown at a time, Enter / Escape, replaced and dismissed
// questions, de-duplication, and the strings every language pack must carry for the dialogs built on it.
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "confirm_dialog.hpp"
#include "deckhand/i18n/i18n.hpp"

using namespace deckhand;

namespace {
ConfirmRequest ask(const std::string& id, int* ok, int* no) {
  ConfirmRequest r;
  r.id = id;
  r.message = "Sure?";
  r.on_ok = [ok] { ++*ok; };
  r.on_cancel = [no] { ++*no; };
  return r;
}
}  // namespace

int main() {
  int ok = 0, no = 0;
  {  // the queue: any thread asks, the UI thread takes in order; the same id waits once
    ConfirmQueue q;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&, i] { q.ask(ask("t" + std::to_string(i), &ok, &no)); });
    for (auto& t : threads) t.join();
    CHECK(q.size() == 4);
    CHECK(!q.ask(ask("t2", &ok, &no)) && q.size() == 4);
    CHECK(q.ask(ask("", &ok, &no)) && q.ask(ask("", &ok, &no)));  // no id: never de-duplicated
    CHECK(q.waiting("t1") && !q.waiting("zz"));
    ConfirmRequest r;
    int taken = 0;
    while (q.take(r)) ++taken;
    CHECK(taken == 6 && q.size() == 0 && !q.take(r));
  }
  {  // accept runs on_ok only, cancel on_cancel only, each once
    ConfirmState s;
    CHECK(!s.open() && !s.accept() && !s.cancel());
    CHECK(s.show(ask("a", &ok, &no)) && s.open() && s.request().id == "a");
    CHECK(s.accept() && !s.open() && ok == 1 && no == 0);
    CHECK(!s.accept() && ok == 1);
    s.show(ask("b", &ok, &no));
    CHECK(s.cancel() && no == 1 && ok == 1);
  }
  {  // keys: Enter accepts, Escape cancels, nothing open = not used
    ConfirmState s;
    ok = no = 0;
    CHECK(!s.key(ConfirmKey::Enter));
    s.show(ask("k", &ok, &no));
    CHECK(s.key(ConfirmKey::Enter) && ok == 1 && no == 0);
    s.show(ask("k", &ok, &no));
    CHECK(s.key(ConfirmKey::Escape) && ok == 1 && no == 1);
  }
  {  // a new question replaces the shown one, which counts as declined; the same id is dropped
    ConfirmState s;
    ok = no = 0;
    s.show(ask("first", &ok, &no));
    CHECK(!s.show(ask("first", &ok, &no)) && no == 0 && s.open());
    CHECK(s.show(ask("second", &ok, &no)) && no == 1 && s.request().id == "second");
    s.cancel();
    CHECK(no == 2);
  }
  {  // an answer may ask again (the callback runs after the dialog closed)
    ConfirmState s;
    ConfirmQueue q;
    int again = 0;
    ConfirmRequest r;
    r.id = "chain";
    r.on_ok = [&] {
      CHECK(!s.open());
      ++again;
      q.ask(ask("next", &ok, &no));
    };
    s.show(std::move(r));
    s.accept();
    CHECK(again == 1 && q.size() == 1);
    // empty callbacks are fine
    ConfirmRequest bare;
    s.show(std::move(bare));
    CHECK(s.accept() && !s.open());
  }

  {  // every language pack carries the dialogs' strings, with the same placeholders (the packs are validated against English)
    i18n::Catalog cat;
    CHECK(cat.load_builtin("resources/lang/en.dhlang"));
    std::vector<std::string> errs;
    CHECK(cat.load_directory("resources/lang", &errs) >= 5 && errs.empty());
    const char* keys[] = {"confirm.ok", "confirm.cancel", "quit.title", "quit.both", "quit.capture", "quit.mix", "quit.detail", "quit.anyway", "quit.stay",
                          "cues.follow_title", "cues.follow_ask", "cues.follow_links", "cues.follow_off_grid", "cues.follow_undo", "cues.follow_move",
                          "cues.follow_keep", "cues.followed", "cues.follow_failed", "shared.leave_title", "shared.leave_ask", "shared.leave_detail"};
    for (const char* tag : {"en", "de", "fr", "es", "ja"}) {
      CHECK(cat.set_language(tag));
      for (const char* k : keys) {
        CHECK(cat.has(k));
        CHECK(!cat.tr(k).empty());
      }
      CHECK(cat.tr("cues.follow_ask", {{"count", 3}}).find("3") != std::string::npos);
      CHECK(cat.tr("shared.leave_ask", {{"name", "Friday"}}).find("Friday") != std::string::npos);
    }
  }
  std::puts("test_confirm_dialog: ok");
  return 0;
}
