// Locked beat grids in library sync: the writer's lock state travels with the grid, so a re-analysis from a device that had
// not seen the lock never replaces a locked grid, an explicit edit on a device that holds the lock still wins, and
// unlocking syncs. Two and three simulated devices, concurrent edits, plain and encrypted folders.
#include "sync_dev.hpp"

using namespace syncdev;

namespace {

double bpm_of(Dev& d, const std::string& fp) {
  const auto g = d.lib->beatgrid(row_of(d, fp));
  return g.empty() ? 0 : g[0].bpm;
}
void set_grid(Dev& d, const std::string& fp, double bpm) { d.lib->replace_beatgrid(row_of(d, fp), {{100.0, bpm, 1}}); }
bool locked(Dev& d, const std::string& fp) { return d.lib->grid_locked(row_of(d, fp)); }

void run(const std::string& pass) {
  const std::string fp = fp_of(1);
  Scenario s(pass.empty() ? "lock" : "lockenc");
  Dev& a = s.add("A", false);
  Dev& b = s.add("B", false);
  Dev& c = s.add("C", false);
  for (Dev* d : {&a, &b, &c}) {
    d->cfg.passphrase = pass;
    d->start();
    add_track(*d, 1, "One");
  }
  s.sync_all();
  auto all = [&](double bpm) { return bpm_of(a, fp) == bpm && bpm_of(b, fp) == bpm && bpm_of(c, fp) == bpm; };

  // A analyses and locks; B and C had not seen the lock and re-analyse afterwards (B last).
  adv(10000);
  set_grid(a, fp, 120);
  a.lib->set_grid_locked(row_of(a, fp), true);
  a.sync();
  adv(10000);
  set_grid(c, fp, 125);
  c.sync();  // reads A's lock together with its own grid, and the lock wins
  adv(10000);
  set_grid(b, fp, 130);  // B has read nothing yet and writes the newest grid
  s.sync_all(4);
  if (!all(120)) std::fprintf(stderr, "bpm A %g B %g C %g\n", bpm_of(a, fp), bpm_of(b, fp), bpm_of(c, fp));
  CHECK(all(120));
  CHECK(locked(a, fp) && locked(b, fp) && locked(c, fp));

  // Nothing new: nothing is written.
  CHECK(a.sync().records_written == 0 && b.sync().records_written == 0);

  // An explicit edit on a device that holds the lock wins, and everybody keeps the lock.
  adv(10000);
  set_grid(b, fp, 126);
  s.sync_all(3);
  CHECK(all(126) && locked(a, fp) && locked(c, fp));

  adv(10000);
  // C unlocks without having synced and re-analyses: an unlock syncs, and the grid it wrote after it goes with it.
  c.lib->set_grid_locked(row_of(c, fp), false);
  adv(10000);
  set_grid(c, fp, 140);
  s.sync_all(4);
  CHECK(!locked(a, fp) && !locked(b, fp) && !locked(c, fp));
  CHECK(all(140));

  // Lock and unlock racing with a re-analysis: A locks the new grid, B (unlocked, unaware) re-analyses later, C too.
  adv(10000);
  a.lib->set_grid_locked(row_of(a, fp), true);
  a.sync();
  adv(10000);
  set_grid(b, fp, 150);
  adv(10000);
  set_grid(c, fp, 160);
  s.sync_all(4);
  CHECK(all(140) && locked(a, fp) && locked(b, fp) && locked(c, fp));

  // A device that joins later gets the locked grid, not a stale re-analysis.
  Dev& d = s.add("D", false);
  d.cfg.passphrase = pass;
  d.start();
  d.lib->replace_beatgrid(add_track(d, 1, "One"), {{100.0, 170, 1}});  // analysed before it ever synced
  s.sync_all(4);
  CHECK(all(140) && bpm_of(d, fp) == 140 && locked(d, fp));

  // Unlocking on the locking device and re-analysing elsewhere afterwards: the new grid arrives everywhere.
  adv(10000);
  a.lib->set_grid_locked(row_of(a, fp), false);
  s.sync_all(3);
  CHECK(!locked(b, fp) && !locked(d, fp));
  adv(10000);
  set_grid(d, fp, 175);
  s.sync_all(3);
  CHECK(all(175) && bpm_of(d, fp) == 175);
}

}  // namespace

int main() {
  run("");
  if (deckhand::sync::SyncEngine::encryption_available()) run("grid lock passphrase");
  else std::puts("encryption unavailable in this build: the encrypted run was skipped");
  std::puts("test_sync_locked_grid: all checks passed");
  return 0;
}
