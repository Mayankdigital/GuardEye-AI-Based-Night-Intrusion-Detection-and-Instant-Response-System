#include <vector>
#include <string>
#include "check.h"
#include "state_machine.h"
using namespace guardeye;

static LogicCfg cfg() {
  LogicCfg c;
  c.confirmSeconds = 2.0; c.minHits = 5; c.gapToleranceSec = 1.0; c.suspectTimeoutSec = 6;
  c.doorWindowSec = 10; c.doorMinHits = 1; c.alarmMaxSec = 120; c.rearmCooldownSec = 10;
  return c;
}
// Feed a person at 5 detections/second for `secs` seconds starting at t0.
static double feed(StateMachine& sm, double t0, double secs) {
  double t = t0;
  for (; t < t0 + secs; t += 0.2) { sm.onPersonInZone(t); sm.tick(t); }
  return t;
}

int main() {
  // 1) Disarmed ignores everything
  { StateMachine sm(cfg()); feed(sm, 0, 5); CHECK(sm.state() == State::Disarmed); }

  // 2) Armed -> Suspect on first detection, -> Alarm only after sustained presence
  { StateMachine sm(cfg()); sm.arm(0);
    CHECK(sm.state() == State::Armed);
    sm.onPersonInZone(1.0); CHECK(sm.state() == State::Suspect);
    feed(sm, 1.2, 0.6);     CHECK(sm.state() == State::Suspect);   // not yet 2 s
    feed(sm, 1.8, 1.5);     CHECK(sm.state() == State::Alarm);     // >= 2 s and >= 5 hits
  }

  // 3) A one-frame glitch (cat/shadow) never alarms and falls back to Armed
  { StateMachine sm(cfg()); sm.arm(0);
    sm.onPersonInZone(1.0); CHECK(sm.state() == State::Suspect);
    for (double t = 1.1; t < 9; t += 0.1) sm.tick(t);
    CHECK(sm.state() == State::Armed);
  }

  // 4) Door opened + person => fast alarm (2nd confirmation), no 2 s wait
  { StateMachine sm(cfg()); sm.arm(0);
    sm.onDoor(true, 1.0);
    sm.onPersonInZone(1.5);
    CHECK(sm.state() == State::Alarm);
  }

  // 5) Door alone (no person) does nothing; stale door event expires
  { StateMachine sm(cfg()); sm.arm(0);
    sm.onDoor(true, 1.0); CHECK(sm.state() == State::Armed);
    sm.onPersonInZone(30.0);            // 29 s later: outside the 10 s door window
    CHECK(sm.state() == State::Suspect);
  }

  // 6) Detection gap longer than tolerance restarts the streak
  { StateMachine sm(cfg()); sm.arm(0);
    sm.onPersonInZone(1.0); sm.onPersonInZone(1.2);
    sm.onPersonInZone(3.0);             // 1.8 s gap > 1.0 s tolerance
    CHECK(sm.hits() == 1);
  }

  // 7) Silence -> Armed with cooldown; alarm can retrigger afterwards
  { StateMachine sm(cfg()); sm.arm(0); sm.onDoor(true, 1); sm.onPersonInZone(1.1);
    CHECK(sm.state() == State::Alarm);
    sm.silence(5.0); CHECK(sm.state() == State::Armed);
    sm.onPersonInZone(6.0); CHECK(sm.state() == State::Armed);     // inside cooldown
    sm.onPersonInZone(16.0); CHECK(sm.state() == State::Suspect);  // cooldown over
  }

  // 8) Siren safety cut-off
  { StateMachine sm(cfg()); sm.arm(0); sm.onDoor(true, 1); sm.onPersonInZone(1.1);
    CHECK(sm.state() == State::Alarm);
    sm.tick(50);  CHECK(sm.state() == State::Alarm);
    sm.tick(130); CHECK(sm.state() == State::Armed);
  }

  // 9) Disarm from any state; transitions are reported with reasons
  { StateMachine sm(cfg());
    std::vector<std::string> log;
    sm.setListener([&](State f, State t, const std::string& r) { log.push_back(std::string(stateName(f)) + ">" + stateName(t) + ":" + r); });
    sm.arm(0, "button"); sm.onDoor(true, 1); sm.onPersonInZone(1.1); sm.disarm(2, "web");
    CHECK(sm.state() == State::Disarmed);
    CHECK(log.size() == 4);
    CHECK(log[0] == "DISARMED>ARMED:button");
    CHECK(log[3] == "ALARM>DISARMED:web");
  }
  PASS("state_machine");
  return 0;
}
