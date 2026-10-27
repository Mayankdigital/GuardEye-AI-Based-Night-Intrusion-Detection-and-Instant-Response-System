// Vision thread: frame -> motion gate -> YOLO -> zone test -> controller.
#pragma once
#include <atomic>
#include <thread>

#include "app_state.h"
#include "capture.h"
#include "config.h"
#include "controller.h"
#include "detector.h"
#include "motion_gate.h"

namespace guardeye {

class Vision {
 public:
  Vision(const Config& cfg, FrameGrabber& grabber, ZoneStore& zone, Detector* detector,
         Controller& ctl, AppState& app)
      : cfg_(cfg), grabber_(grabber), zone_(zone), detector_(detector), ctl_(ctl), app_(app), gate_(cfg.motion) {}
  ~Vision() { stop(); }

  void start();
  void stop();
  uint64_t heartbeat() const { return heartbeat_.load(); }  // watchdog: must keep increasing

 private:
  void run();

  Config cfg_;
  FrameGrabber& grabber_;
  ZoneStore& zone_;
  Detector* detector_;
  Controller& ctl_;
  AppState& app_;
  MotionGate gate_;
  std::thread th_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> heartbeat_{0};
};

}  // namespace guardeye
