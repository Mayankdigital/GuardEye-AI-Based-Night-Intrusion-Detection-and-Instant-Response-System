// The controller thread owns the state machine and every decision. Other
// threads never touch it directly: they post small tasks onto a queue, so no
// locking is needed around the logic and a slow producer cannot block it.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>

#include "alerts.h"
#include "app_state.h"
#include "capture.h"
#include "clip_recorder.h"
#include "config.h"
#include "event_store.h"
#include "hal/hardware.h"
#include "state_machine.h"

namespace guardeye {

class Controller {
 public:
  Controller(const Config& cfg, Hardware& hw, AlertManager& alerts, EventStore& store,
             ClipRecorder& clips, FrameGrabber& grabber, AppState& app);
  ~Controller() { stop(); }

  void start();
  void stop();

  // ---- thread-safe inputs ------------------------------------------------
  void postPerson(std::shared_ptr<cv::Mat> annotated, float conf);
  void postDoor(bool open);
  void postButton();
  void postMains(bool ok);
  void postPower(const PowerReading& r);
  void requestArm(const std::string& src);
  void requestDisarm(const std::string& src);
  void requestSilence();

  State state() const { return static_cast<State>(state_.load()); }
  bool needContinuousDetection() const { State s = state(); return s == State::Suspect || s == State::Alarm; }

 private:
  void post(std::function<void()> f);
  void loop();
  void tickLocked(double now);
  void onTransition(State from, State to, const std::string& reason);
  void onAlarm(const std::string& reason);
  void record(const std::string& type, const std::string& detail, double conf = 0,
              const std::string& snap = "", const std::string& clip = "");
  void sendInfo(const std::string& kind, const std::string& text, const std::string& sub, int channels = kAllChannels);
  void persistArmed(bool armed);
  bool restoreArmed();
  std::string stamp() const;

  Config cfg_;
  Hardware& hw_;
  AlertManager& alerts_;
  EventStore& store_;
  ClipRecorder& clips_;
  FrameGrabber& grabber_;
  AppState& app_;

  StateMachine sm_;
  std::atomic<int> state_{static_cast<int>(State::Disarmed)};

  std::thread th_;
  std::mutex qm_;
  std::condition_variable qcv_;
  std::deque<std::function<void()>> q_;
  bool running_ = false;

  // owned by the controller thread only:
  std::shared_ptr<cv::Mat> lastFrame_;
  float lastConf_ = 0;
  int lastScheduleWanted_ = -1;
  bool cameraOfflineAlerted_ = false;
  bool lowBatteryAlerted_ = false;
  bool criticalHandled_ = false;
  bool mainsOk_ = true;
  double lastBatteryV_ = 0;
  std::string armedStateFile_;
};

}  // namespace guardeye
