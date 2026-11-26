// Moving track table columns by their headers.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/app/column_order.hpp"
#include "check.hpp"

using namespace deckhand;
using Cols = std::vector<std::string>;

int main() {
  const std::vector<float> x0{0, 100, 200}, x1{100, 200, 260};
  CHECK(column_drop_slot(x0, x1, -5) == 0);
  CHECK(column_drop_slot(x0, x1, 49) == 0);
  CHECK(column_drop_slot(x0, x1, 51) == 1);
  CHECK(column_drop_slot(x0, x1, 151) == 2);
  CHECK(column_drop_slot(x0, x1, 900) == 3);

  Cols c{"title", "artist", "bpm"};
  CHECK(!move_column(c, "title", "title"));
  CHECK(!move_column(c, "title", "artist"));  // already in front of it
  CHECK(!move_column(c, "bpm", ""));          // already last
  CHECK(!move_column(c, "nope", ""));
  CHECK((c == Cols{"title", "artist", "bpm"}));
  CHECK(move_column(c, "bpm", "title"));
  CHECK((c == Cols{"bpm", "title", "artist"}));
  CHECK(move_column(c, "bpm", ""));
  CHECK((c == Cols{"title", "artist", "bpm"}));
  CHECK(move_column(c, "title", "bpm"));
  CHECK((c == Cols{"artist", "title", "bpm"}));
  std::puts("column order: all checks passed");
  return 0;
}
