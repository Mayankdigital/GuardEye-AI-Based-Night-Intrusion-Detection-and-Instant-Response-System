#include "check.h"
#include "schedule.h"
using namespace guardeye;
int main() {
  CHECK(parseHHMM("21:30", 0) == 21 * 60 + 30);
  CHECK(parseHHMM("garbage", 77) == 77);
  CHECK(parseHHMM("25:00", 5) == 5);
  Schedule s;
  s.enabled = true; s.armMin = 21 * 60; s.disarmMin = 7 * 60;   // overnight window
  CHECK(s.wantsArmed(22 * 60));
  CHECK(s.wantsArmed(0));
  CHECK(s.wantsArmed(6 * 60 + 59));
  CHECK(!s.wantsArmed(7 * 60));
  CHECK(!s.wantsArmed(12 * 60));
  CHECK(s.wantsArmed(21 * 60));
  Schedule day; day.armMin = 9 * 60; day.disarmMin = 17 * 60;   // same-day window
  CHECK(day.wantsArmed(10 * 60));
  CHECK(!day.wantsArmed(20 * 60));
  PASS("schedule");
  return 0;
}
