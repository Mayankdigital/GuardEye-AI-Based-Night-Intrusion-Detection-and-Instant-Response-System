#include "controller.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spawn.h>
#include <sys/wait.h>

#include "json.hpp"
#include "log.h"
#include "util.h"

extern char** environ;

namespace guardeye {
namespace fs = std::filesystem;
using nlohmann::json;

Controller::Controller(const Config& cfg, Hardware& hw, AlertManager& alerts, EventStore& store,
                       ClipRecorder& clips, FrameGrabber& grabber, AppState& app)
    : cfg_(cfg), hw_(hw), alerts_(alerts), store_(store), clips_(clips), grabber_(grabber), app_(app),
      sm_(cfg.logic) {
  armedStateFile_ = cfg.storage.dataDir + "/armed.state";
  sm_.setListener([this](State f, State t, const std::string& r) { onTransition(f, t, r); });
}

std::string Controller::stamp() const { return formatLocal(hw_.wallClock()); }

// ------------------------------------------------------------------ plumbing
void Controller::start() {
  running_ = true;
  th_ = std::thread(&Controller::loop, this);
  post([this] {
    // Boot behaviour: schedule decides if enabled, otherwise restore the last manual state.
    if (!cfg_.schedule.enabled && restoreArmed()) sm_.arm(monoNow(), "restored after reboot");
  });
}

void Controller::stop() {
  {
    std::lock_guard<std::mutex> lk(qm_);
    if (!running_) return;
    running_ = false;
  }
  qcv_.notify_all();
  if (th_.joinable()) th_.join();
}

void Controller::post(std::function<void()> f) {
  {
    std::lock_guard<std::mutex> lk(qm_);
    if (q_.size() > 1000) q_.pop_front();  // never grow without bound
    q_.push_back(std::move(f));
  }
  qcv_.notify_one();
}

void Controller::loop() {
  while (true) {
    std::deque<std::function<void()>> work;
    {
      std::unique_lock<std::mutex> lk(qm_);
      qcv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return !q_.empty() || !running_; });
      if (!running_) return;
      work.swap(q_);
    }
    for (auto& f : work) f();
    tickLocked(monoNow());
  }
}

void Controller::tickLocked(double now) {
  sm_.tick(now);

  // Daily schedule: only acts when the wanted state CHANGES, so a manual
  // arm/disarm stays in force until the next scheduled change.
  if (cfg_.schedule.enabled) {
    int wanted = cfg_.schedule.wantsArmed(hw_.minuteOfDay()) ? 1 : 0;
    if (wanted != lastScheduleWanted_) {
      lastScheduleWanted_ = wanted;
      if (wanted) { sm_.arm(now, "schedule"); persistArmed(true); }
      else if (sm_.state() != State::Disarmed) { sm_.disarm(now, "schedule"); persistArmed(false); }
    }
  }

  // Camera health: a dead camera is itself a security problem.
  if (sm_.state() != State::Disarmed) {
    double off = grabber_.offlineSeconds();
    if (off > cfg_.camera.offlineAlertSec && !cameraOfflineAlerted_) {
      cameraOfflineAlerted_ = true;
      record("camera_offline", "no frames for " + std::to_string(static_cast<int>(off)) + "s");
      sendInfo("warn", "CAMERA OFFLINE (" + std::to_string(static_cast<int>(off)) + "s) at " + stamp(), "camera");
    } else if (off == 0 && cameraOfflineAlerted_) {
      cameraOfflineAlerted_ = false;
      record("camera_online", "stream restored");
      sendInfo("info", "Camera back online at " + stamp(), "camera");
    }
  }
}

// ------------------------------------------------------------------- inputs
void Controller::postPerson(std::shared_ptr<cv::Mat> frame, float conf) {
  post([this, frame, conf] {
    lastFrame_ = frame;
    lastConf_ = conf;
    sm_.onPersonInZone(monoNow());
  });
}

void Controller::postDoor(bool open) {
  post([this, open] {
    app_.update([&](Status& s) { s.doorOpen = open; });
    State st = sm_.state();
    if (st != State::Disarmed) {
      record(open ? "door_open" : "door_closed", std::string("state=") + stateName(st));
      sendInfo("info", std::string("Door ") + (open ? "OPENED" : "closed") + " while " + stateName(st) + " at " + stamp(),
               "door", kMqtt);
    }
    sm_.onDoor(open, monoNow());
  });
}

void Controller::postButton() {
  post([this] {
    if (sm_.state() == State::Disarmed) { sm_.arm(monoNow(), "button"); persistArmed(true); }
    else { sm_.disarm(monoNow(), "button"); persistArmed(false); }
  });
}

void Controller::requestArm(const std::string& src) {
  post([this, src] { sm_.arm(monoNow(), src); persistArmed(true); });
}
void Controller::requestDisarm(const std::string& src) {
  post([this, src] { sm_.disarm(monoNow(), src); persistArmed(false); });
}
void Controller::requestSilence() { post([this] { sm_.silence(monoNow()); }); }

void Controller::postMains(bool ok) {
  post([this, ok] {
    if (ok == mainsOk_) return;
    mainsOk_ = ok;
    app_.update([&](Status& s) { s.mainsOk = ok; });
    record(ok ? "mains_restored" : "mains_lost", "battery=" + std::to_string(lastBatteryV_) + "V");
    sendInfo(ok ? "info" : "warn",
             ok ? "Mains power restored at " + stamp() : "MAINS POWER LOST, running on battery at " + stamp(), "power");
  });
}

void Controller::postPower(const PowerReading& r) {
  post([this, r] {
    app_.update([&](Status& s) { s.batteryOk = r.ok; s.batteryV = r.busV; s.batteryMa = r.currentMa; });
    if (!r.ok) return;
    lastBatteryV_ = r.busV;
    const auto& h = cfg_.hardware;
    if (r.busV > 1.0 && r.busV < h.batteryLowV && !lowBatteryAlerted_) {
      lowBatteryAlerted_ = true;
      record("battery_low", std::to_string(r.busV) + "V");
      sendInfo("warn", "LOW BATTERY: " + std::to_string(r.busV).substr(0, 5) + " V at " + stamp(), "power");
    } else if (r.busV > h.batteryLowV + 0.3) {
      lowBatteryAlerted_ = false;
      criticalHandled_ = false;
    }
    if (r.busV > 1.0 && r.busV < h.batteryCriticalV && !mainsOk_ && !criticalHandled_) {
      criticalHandled_ = true;
      record("battery_critical", std::to_string(r.busV) + "V");
      sendInfo("warn", "BATTERY CRITICAL: shutting down safely" , "power");
      if (h.shutdownOnCritical) {
        LOG_ERROR << "battery critical -> clean shutdown";
        char* argv[] = {const_cast<char*>("sudo"), const_cast<char*>("-n"), const_cast<char*>("systemctl"),
                        const_cast<char*>("poweroff"), nullptr};
        pid_t pid;
        if (posix_spawnp(&pid, "sudo", nullptr, nullptr, argv, environ) == 0) { int st; waitpid(pid, &st, 0); }
      }
    }
  });
}

// ------------------------------------------------------------- state changes
void Controller::onTransition(State from, State to, const std::string& reason) {
  state_ = static_cast<int>(to);
  LOG_INFO << "STATE " << stateName(from) << " -> " << stateName(to) << " (" << reason << ")";
  app_.update([&](Status& s) { s.state = stateName(to); s.lastEvent = std::string(stateName(to)) + " " + stamp().substr(11); });

  switch (to) {
    case State::Disarmed: hw_.setSiren(false); hw_.setLight(false); hw_.setLed(LedMode::Off); break;
    case State::Armed:    hw_.setSiren(false); hw_.setLight(false); hw_.setLed(LedMode::On); break;
    case State::Suspect:  hw_.setSiren(false); hw_.setLight(cfg_.logic.lightOnSuspect); hw_.setLed(LedMode::SlowBlink); break;
    case State::Alarm:    hw_.setSiren(true);  hw_.setLight(true);  hw_.setLed(LedMode::FastBlink); break;
  }

  if (to == State::Alarm) {
    onAlarm(reason);
  } else if (to == State::Suspect) {
    record("suspect", reason, lastConf_);
  } else if (to == State::Disarmed || (to == State::Armed && from == State::Disarmed)) {
    record(to == State::Armed ? "armed" : "disarmed", reason);
    if (cfg_.alerts.notifyArmDisarm)
      sendInfo("info", std::string("System ") + (to == State::Armed ? "ARMED" : "DISARMED") + " (" + reason + ") at " + stamp(),
               to == State::Armed ? "armed" : "disarmed");
  } else if (from == State::Alarm || from == State::Suspect) {
    record("rearmed", reason);
  }
}

void Controller::onAlarm(const std::string& reason) {
  // Siren/light are already on (set by the caller) BEFORE any slow work below.
  std::time_t t = hw_.wallClock();
  std::string base = formatLocal(t, "%Y%m%d_%H%M%S");
  std::string snapPath = cfg_.storage.dataDir + "/snapshots/" + base + "_alarm.jpg";
  std::string clipPath = cfg_.storage.dataDir + "/clips/" + base + "_alarm.mp4";

  bool haveSnap = false;
  try {
    cv::Mat img;
    if (lastFrame_ && !lastFrame_->empty()) img = *lastFrame_;
    else app_.getFrame(img);
    if (!img.empty()) {
      cv::putText(img, "ALARM " + formatLocal(t), {8, img.rows - 10}, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                  {0, 0, 255}, 1, cv::LINE_AA);
      fs::create_directories(fs::path(snapPath).parent_path());
      haveSnap = cv::imwrite(snapPath, img, {cv::IMWRITE_JPEG_QUALITY, 85});
    }
  } catch (const std::exception& e) {
    LOG_ERROR << "snapshot failed: " << e.what();
  }

  int64_t id = store_.add(t, "alarm", reason, lastConf_, haveSnap ? snapPath : "", "");
  clips_.trigger(clipPath, [this, id](const std::string& path, bool ok) {
    if (ok) store_.setClip(id, path);
  });

  Alert a;
  a.kind = "alarm";
  a.text = "INTRUSION ALARM\nTime: " + formatLocal(t) + "\nReason: " + reason + "\nConfidence: " +
           std::to_string(static_cast<int>(lastConf_ * 100)) + "%";
  a.snapshotPath = haveSnap ? snapPath : "";
  a.mqttSubtopic = "alarm";
  a.mqttPayload = json{{"event", "alarm"}, {"time", formatLocal(t)}, {"reason", reason},
                       {"confidence", lastConf_}, {"snapshot", haveSnap ? snapPath : ""}}.dump();
  alerts_.enqueue(std::move(a));
}

void Controller::record(const std::string& type, const std::string& detail, double conf,
                        const std::string& snap, const std::string& clip) {
  store_.add(hw_.wallClock(), type, detail, conf, snap, clip);
}

void Controller::sendInfo(const std::string& kind, const std::string& text, const std::string& sub, int channels) {
  Alert a;
  a.kind = kind;
  a.text = text;
  a.mqttSubtopic = sub;
  a.mqttPayload = json{{"event", sub}, {"kind", kind}, {"message", text}}.dump();
  a.channels = channels;
  alerts_.enqueue(std::move(a));
}

// Remember armed/disarmed across power cuts (a reboot must not silently disarm the shop).
void Controller::persistArmed(bool armed) {
  std::ofstream f(armedStateFile_, std::ios::trunc);
  if (f) f << (armed ? "armed" : "disarmed") << "\n";
}
bool Controller::restoreArmed() {
  std::ifstream f(armedStateFile_);
  std::string s;
  return f && (f >> s) && s == "armed";
}

}  // namespace guardeye
