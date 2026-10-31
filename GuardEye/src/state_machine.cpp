#include "state_machine.h"

namespace guardeye {

void StateMachine::go(State to, double now, const std::string& reason) {
  if (to == state_) return;
  State from = state_;
  state_ = to;
  stateSince_ = now;
  if (to != State::Suspect && to != State::Alarm) { streakStart_ = -1; lastSeen_ = -1; hits_ = 0; }
  if (listener_) listener_(from, to, reason);
}

void StateMachine::arm(double now, const std::string& reason) {
  if (state_ == State::Disarmed) { lastDoorOpen_ = -1; go(State::Armed, now, reason); }
}

void StateMachine::disarm(double now, const std::string& reason) {
  go(State::Disarmed, now, reason);
}

void StateMachine::silence(double now) {
  if (state_ == State::Alarm || state_ == State::Suspect) {
    cooldownUntil_ = now + cfg_.rearmCooldownSec;
    go(State::Armed, now, "silenced");
  }
}

void StateMachine::onDoor(bool open, double now) {
  if (!open) return;
  if (state_ == State::Armed || state_ == State::Suspect) lastDoorOpen_ = now;
}

void StateMachine::onPersonInZone(double now) {
  if (state_ == State::Disarmed || state_ == State::Alarm) return;
  if (now < cooldownUntil_) return;

  // Streak = continuous presence, tolerating short detection gaps.
  if (streakStart_ < 0 || now - lastSeen_ > cfg_.gapToleranceSec) { streakStart_ = now; hits_ = 0; }
  lastSeen_ = now;
  ++hits_;

  if (state_ == State::Armed) go(State::Suspect, now, "person in zone");

  if (state_ == State::Suspect) {
    bool byTime = hits_ >= cfg_.minHits && (lastSeen_ - streakStart_) >= cfg_.confirmSeconds;
    bool byDoor = doorRecent(now) && hits_ >= cfg_.doorMinHits;
    if (byDoor) go(State::Alarm, now, "person in zone + door opened");
    else if (byTime) go(State::Alarm, now, "person present in zone");
  }
}

void StateMachine::tick(double now) {
  if (state_ == State::Suspect) {
    if (lastSeen_ >= 0 && now - lastSeen_ > cfg_.suspectTimeoutSec) go(State::Armed, now, "suspect timeout");
  } else if (state_ == State::Alarm) {
    if (now - stateSince_ > cfg_.alarmMaxSec) {
      cooldownUntil_ = now + cfg_.rearmCooldownSec;
      go(State::Armed, now, "alarm max duration reached");
    }
  }
}

}  // namespace guardeye
