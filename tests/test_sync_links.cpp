// Linked mix points in library sync: identity by content fingerprint, last-writer-wins edits, tombstones, a link whose
// track is not here yet stays pending, the same link made on two devices becomes one, and the switch turns it off.
#include "../src/sync/format.hpp"
#include "sync_dev.hpp"

using namespace syncdev;

namespace {

MixLink make(std::int64_t from, std::int64_t to, double from_ms, double to_ms) {
  MixLink l;
  l.from_track = from;
  l.to_track = to;
  l.from_ms = from_ms;
  l.to_ms = to_ms;
  return l;
}

std::vector<MixLink> links(Dev& d, const std::string& fp) { return d.lib->mix_links_from(row_of(d, fp)); }

Dev& device(Scenario& s, const char* name) {
  Dev& d = s.add(name, false);
  d.cfg.trust_path_hints = false;
  d.start();
  return d;
}

}  // namespace

int main() {
  const std::string fp1 = fp_of(1), fp2 = fp_of(2), fp3 = fp_of(3);
  {
    Scenario s("links");
    Dev& a = device(s, "A");
    Dev& b = device(s, "B");
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two"), t3 = add_track(a, 3, "Three");
    MixLink l = make(t1, t2, 30000, 1000);
    l.transition_beats = 16;
    l.transition_type = "cut";
    l.label = "drop";
    const auto id12 = a.lib->add_mix_link(l);
    a.lib->add_mix_link(make(t2, t3, 45000, 500));
    s.sync_all();

    // 1. Both ends travel by fingerprint: B has its own row ids.
    const auto on_b = links(b, fp1);
    CHECK(on_b.size() == 1);
    CHECK(on_b[0].from_track == row_of(b, fp1) && on_b[0].to_track == row_of(b, fp2));
    CHECK(on_b[0].from_ms == 30000 && on_b[0].to_ms == 1000 && on_b[0].label == "drop");
    CHECK(on_b[0].transition_beats && *on_b[0].transition_beats == 16);
    CHECK(on_b[0].transition_type && *on_b[0].transition_type == "cut");
    CHECK(links(b, fp2).size() == 1 && b.count("SELECT count(*) FROM mix_links") == 2);
    CHECK(a.count("SELECT count(*) FROM mix_links") == 2);

    // 2. Edits on different fields from two devices without seeing each other: both kept, the later label wins.
    auto eb = on_b[0];
    eb.from_ms = 31000;
    eb.label = "B label";
    CHECK(b.lib->update_mix_link(eb));
    adv(5000);
    auto ea = *a.lib->mix_link(id12);
    ea.transition_beats = 8;
    ea.label = "A label";
    CHECK(a.lib->update_mix_link(ea));
    b.sync();  // edits are stamped when they are captured: B captures first, A (without having seen B's) after
    a.sync();
    s.sync_all();
    for (Dev* d : {&a, &b}) {
      const auto v = links(*d, fp1);
      CHECK(v.size() == 1);
      CHECK(v[0].from_ms == 31000);                       // B's edit survives
      CHECK(v[0].transition_beats && *v[0].transition_beats == 8);  // A's edit survives
      CHECK(v[0].label == "A label");                      // later edit wins the shared field
    }

    // 3. Deletion wins over a concurrent edit and does not come back.
    auto eb2 = links(b, fp1)[0];
    eb2.label = "edited after delete?";
    adv(1000);
    CHECK(a.lib->remove_mix_link(id12));
    adv(1000);
    CHECK(b.lib->update_mix_link(eb2));
    a.sync();
    b.sync();  // B's later edit does not bring the deleted link back
    s.sync_all();
    CHECK(links(a, fp1).empty() && links(b, fp1).empty());
    s.sync_all(2);
    CHECK(links(a, fp1).empty() && links(b, fp1).empty());
    CHECK(a.count("SELECT count(*) FROM mix_links") == 1 && b.count("SELECT count(*) FROM mix_links") == 1);

    // 4. A device that joins after the deletion and a compaction never sees the link.
    CHECK(a.eng->compact().ok);
    Dev& c = device(s, "C");
    s.sync_all();
    CHECK(c.count("SELECT count(*) FROM mix_links") == 1 && links(c, fp1).empty() && links(c, fp2).size() == 1);

    // 5. The same link created on two devices becomes one, and later edits still travel.
    a.lib->add_mix_link(make(row_of(a, fp1), row_of(a, fp3), 10000, 2000));
    b.lib->add_mix_link(make(row_of(b, fp1), row_of(b, fp3), 10000.5, 2000));
    s.sync_all(4);
    for (Dev* d : {&a, &b, &c}) CHECK(links(*d, fp1).size() == 1);
    auto e5 = links(c, fp1)[0];
    e5.label = "merged";
    adv(1000);
    CHECK(c.lib->update_mix_link(e5));
    s.sync_all(3);
    for (Dev* d : {&a, &b, &c}) {
      CHECK(links(*d, fp1).size() == 1);
      CHECK(links(*d, fp1)[0].label == "merged");
    }
  }
  {  // 6. A link naming a track this library does not have yet stays pending, and is made when the track appears.
    Scenario s("pending");
    Dev& b = device(s, "B");
    const std::string fp9 = fp_of(9), fp10 = fp_of(10);
    detail::FileHeader h;
    h.device_id = "0123456789abcdef";
    h.device_name = "Forged";
    h.seq = 1;
    h.created_ms = g_now;
    detail::FileWriter w(h);
    const auto reg = [&](const char* field, const detail::Val& v) {
      detail::RegRec r;
      r.kind = 'L';
      r.eid = "0123456789abcdef:1";
      r.field = field;
      r.st = {g_now, 1, h.device_id};
      r.v = v;
      w.add(r);
    };
    reg("from", detail::Val::text(fp9));
    reg("to", detail::Val::text(fp10));
    reg("from_ms", detail::Val::real(12345));
    reg("to_ms", detail::Val::real(250));
    reg("label", detail::Val::text("late"));
    const fs::path dir = fs::path(s.folder) / "DeckHandSync" / "v1" / h.device_id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    write_bytes((dir / "00000001.dhlog").string(), w.finish());
    adv(1000);
    const auto r1 = b.sync();
    CHECK(r1.links_pending == 1 && b.count("SELECT count(*) FROM mix_links") == 0);
    const auto t9 = add_track(b, 9, "Nine");
    CHECK(b.sync().links_pending == 1);  // one end is still missing
    const auto t10 = add_track(b, 10, "Ten");
    const auto r3 = b.sync();
    CHECK(r3.links_pending == 0);
    const auto got = b.lib->mix_links_from(t9);
    CHECK(got.size() == 1 && got[0].to_track == t10 && got[0].from_ms == 12345 && got[0].to_ms == 250 && got[0].label == "late");
    b.sync();
    CHECK(b.count("SELECT count(*) FROM mix_links") == 1);  // repeating changes nothing
  }
  {  // 7. Switched off: nothing is sent or applied.
    Scenario s("off");
    Dev& a = device(s, "A");
    Dev& b = s.add("B", false);
    b.cfg.trust_path_hints = false;
    b.cfg.sync_mix_links = false;
    b.start();
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two");
    a.lib->add_mix_link(make(t1, t2, 1000, 2000));
    s.sync_all();
    CHECK(b.count("SELECT count(*) FROM tracks") == 2 && b.count("SELECT count(*) FROM mix_links") == 0);
  }
  std::puts("test_sync_links: all checks passed");
  return 0;
}
