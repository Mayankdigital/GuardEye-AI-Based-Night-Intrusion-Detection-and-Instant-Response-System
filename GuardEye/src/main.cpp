// GuardEye - AI night intrusion detection and instant response.
//   guardeye --config /etc/guardeye/guardeye.json
//   guardeye --config config/guardeye.json --sim       (laptop demo, no hardware)
//   guardeye --config ... --selftest [--test-relays]   (hardware bring-up)
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

#include "alerts.h"
#include "app_state.h"
#include "capture.h"
#include "clip_recorder.h"
#include "config.h"
#include "controller.h"
#include "detector.h"
#include "event_store.h"
#include "hal/hardware.h"
#include "http_server.h"
#include "log.h"
#include "sysinfo.h"
#include "util.h"
#include "vision.h"

using namespace guardeye;
namespace fs = std::filesystem;

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

static void usage() {
  std::cout << "GuardEye " << GUARDEYE_VERSION << "\n"
            << "  --config <file>   config file (default config/guardeye.json)\n"
            << "  --sim             simulate GPIO/I2C/SPI (no hardware needed)\n"
            << "  --selftest        init hardware, print readings, exit\n"
            << "  --test-relays     with --selftest: pulse light then siren for 1 s each\n"
            << "  --version\n";
}

static void cleanupOld(const std::string& dir, int keepDays) {
  if (keepDays <= 0) return;
  std::error_code ec;
  auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * keepDays);
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && e.last_write_time(ec) < limit) {
      fs::remove(e.path(), ec);
      LOG_INFO << "retention: removed " << e.path().filename().string();
    }
  }
}

int main(int argc, char** argv) {
  std::string cfgPath = "config/guardeye.json";
  bool sim = false, selftest = false, testRelays = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
    else if (!std::strcmp(argv[i], "--sim")) sim = true;
    else if (!std::strcmp(argv[i], "--selftest")) selftest = true;
    else if (!std::strcmp(argv[i], "--test-relays")) testRelays = true;
    else if (!std::strcmp(argv[i], "--version")) { std::cout << GUARDEYE_VERSION << "\n"; return 0; }
    else { usage(); return std::strcmp(argv[i], "--help") ? 2 : 0; }
  }

  Config cfg;
  std::string err;
  if (!Config::load(cfgPath, cfg, err)) { LOG_ERROR << err; return 1; }
  std::error_code ec;
  fs::create_directories(cfg.storage.dataDir + "/snapshots", ec);
  fs::create_directories(cfg.storage.dataDir + "/clips", ec);

  // ---- hardware ------------------------------------------------------------
  Hardware hw(cfg.hardware);
  if (!hw.init(sim, err)) { LOG_ERROR << "hardware init failed: " << err; return 1; }
  LOG_INFO << "GuardEye " << GUARDEYE_VERSION << " | hardware: " << hw.summary() << (hw.simulated() ? " [SIMULATION]" : "");

  if (selftest) {
    std::tm tm{};
    std::time_t t = hw.wallClock();
    localtime_r(&t, &tm);
    std::cout << "RTC present : " << (hw.rtcPresent() ? "yes" : "no (system clock)") << "\n"
              << "wall clock  : " << formatLocal(t) << "\n";
    if (testRelays) {
      std::cout << "light ON 1s...\n";  hw.setLight(true); std::this_thread::sleep_for(std::chrono::seconds(1)); hw.setLight(false);
      std::cout << "siren ON 1s...\n";  hw.setSiren(true); std::this_thread::sleep_for(std::chrono::seconds(1)); hw.setSiren(false);
    }
    std::cout << "selftest done\n";
    return 0;
  }

  // ---- components ------------------------------------------------------------
  EventStore store;
  if (!store.open(cfg.storage.dataDir + "/events.db", err)) { LOG_ERROR << err; return 1; }

  std::unique_ptr<Detector> detector;
  if (cfg.detector.mode == "yolo") {
    detector = std::make_unique<Detector>(cfg.detector);
    if (!detector->load(err)) {
      LOG_ERROR << err << "\n  -> run scripts/export_model.py, or set detector.mode to \"motion_only\" for a model-free demo";
      return 1;
    }
    LOG_INFO << "YOLO model loaded: " << cfg.detector.modelPath << " (input " << cfg.detector.inputSize << ")";
  }

  AppState app;
  ZoneStore zone(cfg.zone, cfgPath);
  AlertManager alerts(cfg.alerts);
  ClipRecorder clips(cfg.storage);
  FrameGrabber grabber(cfg.camera);
  Controller ctl(cfg, hw, alerts, store, clips, grabber, app);
  Vision vision(cfg, grabber, zone, detector.get(), ctl, app);
  HttpServer http(cfg, ctl, store, app, zone, hw);

  app.update([&](Status& s) { s.hardware = hw.summary(); s.ip = primaryIPv4(); });
  grabber.setFrameCallback([&](const cv::Mat& f, double ts) { clips.push(f, ts); });

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  alerts.start();
  ctl.start();
  HardwareCallbacks cb;
  cb.onDoor = [&](bool open) { ctl.postDoor(open); };
  cb.onButton = [&] { ctl.postButton(); };
  cb.onMains = [&](bool ok) { ctl.postMains(ok); };
  cb.onPower = [&](const PowerReading& r) { ctl.postPower(r); };
  cb.displayLines = [&]() {
    Status s = app.get();
    char l3[32], l4[32];
    std::snprintf(l3, sizeof l3, "CAM %s %.0FPS", s.cameraOnline ? "OK" : "LOST", s.captureFps);
    std::snprintf(l4, sizeof l4, "BAT %.1fV %s", s.batteryV, s.mainsOk ? "MAINS" : "BATT!");
    return std::vector<std::string>{"GUARDEYE " + s.state, s.ip + ":" + std::to_string(cfg.http.port),
                                    formatLocal(hw.wallClock(), "%d %b %H:%M"), l3, l4,
                                    std::string("DOOR ") + (s.doorOpen ? "OPEN" : "CLOSED"), s.lastEvent};
  };
  hw.start(cb);
  grabber.start();
  vision.start();
  if (!http.start(err)) { LOG_ERROR << err; g_stop = true; }
  else LOG_INFO << "dashboard: http://" << primaryIPv4() << ":" << cfg.http.port << "/";

  sdNotify("READY=1");
  const double wdInterval = watchdogIntervalUsec() > 0 ? watchdogIntervalUsec() / 2e6 : 0;
  double lastWd = 0, lastClean = 0;
  uint64_t lastHb = vision.heartbeat();
  CpuMeter cpu;
  cpu.sample();

  // ---- main loop: housekeeping + watchdog -----------------------------------------
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    double now = monoNow();

    double c = cpu.sample(), temp = cpuTempC();
    app.update([&](Status& s) {
      s.cpuPercent = c; s.cpuTempC = temp; s.cameraOnline = grabber.online();
      s.captureFps = grabber.fps(); s.ip = primaryIPv4();
    });

    // Only ping the watchdog while the vision loop is genuinely alive; a hang
    // makes systemd restart us.
    if (wdInterval > 0 && now - lastWd >= wdInterval) {
      uint64_t hb = vision.heartbeat();
      if (hb != lastHb) { sdNotify("WATCHDOG=1"); lastHb = hb; }
      else LOG_ERROR << "vision thread heartbeat stalled: withholding watchdog ping";
      lastWd = now;
    }
    if (now - lastClean > 3600) {
      lastClean = now;
      cleanupOld(cfg.storage.dataDir + "/snapshots", cfg.storage.keepDays);
      cleanupOld(cfg.storage.dataDir + "/clips", cfg.storage.keepDays);
    }
  }

  // ---- orderly shutdown: outputs OFF first ------------------------------------------
  LOG_INFO << "shutting down";
  hw.allOff();  // siren/light off immediately, before the slower joins
  sdNotify("STOPPING=1");
  http.stop();
  vision.stop();
  grabber.stop();
  ctl.stop();
  hw.stop();
  clips.stop();
  alerts.stop();
  return 0;
}
