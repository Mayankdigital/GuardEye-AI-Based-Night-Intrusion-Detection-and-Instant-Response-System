// GuardEye decision logic. Pure C++ (no hardware, no OpenCV), takes time as a
// parameter so it can be unit-tested with fake detections and a fake clock.
//
//   DISARMED --arm--> ARMED --person in zone--> SUSPECT --confirmed--> ALARM
//   SUSPECT --timeout--> ARMED        ALARM --silence/max time--> ARMED
//   any --disarm--> DISARMED
#pragma once
#include <functional>
#include <string>

#include "config.h"

namespace guardeye {

enum class State { Disarmed, Armed, Suspect, Alarm };

inline const char* stateName(State s) {
  switch (s) {
    case State::Disarmed: return "DISARMED";
    case State::Armed:    return "ARMED";
    case State::Suspect:  return "SUSPECT";
    case State::Alarm:    return "ALARM";
  }
  return "?";
}

class StateMachine {
 public:
  using Listener = std::function<void(State from, State to, const std::string& reason)>;

  explicit StateMachine(const LogicCfg& cfg) : cfg_(cfg) {}
  void setListener(Listener l) { listener_ = std::move(l); }

  State state() const { return state_; }

  void arm(double now, const std::string& reason = "arm");
  void disarm(double now, const std::string& reason = "disarm");
  void silence(double now);              // stop siren, go back to ARMED (with cooldown)
  void onPersonInZone(double now);       // one detection cycle with a person inside the zone
  void onDoor(bool open, double now);    // reed switch edge
  void tick(double now);                 // call ~10 Hz: handles timeouts

  // Diagnostics for the dashboard/tests.
  int hits() const { return hits_; }
  double streakSeconds() const { return streakStart_ < 0 ? 0 : lastSeen_ - streakStart_; }

 private:
  void go(State to, double now, const std::string& reason);
  bool doorRecent(double now) const { return lastDoorOpen_ >= 0 && now - lastDoorOpen_ <= cfg_.doorWindowSec; }

  LogicCfg cfg_;
  Listener listener_;
  State state_ = State::Disarmed;
  double stateSince_ = 0;
  double cooldownUntil_ = -1;
  // person streak tracking
  double streakStart_ = -1;
  double lastSeen_ = -1;
  int hits_ = 0;
  double lastDoorOpen_ = -1;
};

}  // namespace guardeye
