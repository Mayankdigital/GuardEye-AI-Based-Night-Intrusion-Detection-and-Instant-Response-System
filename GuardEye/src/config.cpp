#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "json.hpp"
#include "log.h"

namespace guardeye {
using nlohmann::json;

namespace {
json section(const json& root, const char* key) {
  if (root.contains(key) && root[key].is_object()) return root[key];
  return json::object();
}
void envOverride(const char* name, std::string& dst) {
  if (const char* v = std::getenv(name)) { if (*v) dst = v; }
}
}  // namespace

bool Config::load(const std::string& path, Config& c, std::string& err) {
  std::ifstream f(path);
  if (!f) { err = "cannot open config file: " + path; return false; }
  json root;
  try { root = json::parse(f, nullptr, true, /*ignore_comments=*/true); }
  catch (const std::exception& e) { err = std::string("config parse error: ") + e.what(); return false; }

  try {
    c.path = path;
    { auto j = section(root, "camera");
      c.camera.url = j.value("url", c.camera.url);
      c.camera.useGstreamer = j.value("use_gstreamer", c.camera.useGstreamer);
      c.camera.codec = j.value("codec", c.camera.codec);
      c.camera.gstPipeline = j.value("gst_pipeline", c.camera.gstPipeline);
      c.camera.rtspTcp = j.value("rtsp_tcp", c.camera.rtspTcp);
      c.camera.latencyMs = j.value("latency_ms", c.camera.latencyMs);
      c.camera.width = j.value("width", c.camera.width);
      c.camera.height = j.value("height", c.camera.height);
      c.camera.loopFile = j.value("loop_file", c.camera.loopFile);
      c.camera.offlineAlertSec = j.value("offline_alert_seconds", c.camera.offlineAlertSec); }
    { auto j = section(root, "detector");
      c.detector.mode = j.value("mode", c.detector.mode);
      c.detector.modelPath = j.value("model_path", c.detector.modelPath);
      c.detector.inputSize = j.value("input_size", c.detector.inputSize);
      c.detector.conf = j.value("conf", c.detector.conf);
      c.detector.nms = j.value("nms", c.detector.nms);
      c.detector.personClass = j.value("person_class", c.detector.personClass);
      c.detector.minBoxHeightFrac = j.value("min_box_height_frac", c.detector.minBoxHeightFrac);
      c.detector.numThreads = j.value("num_threads", c.detector.numThreads); }
    { auto j = section(root, "motion");
      c.motion.enabled = j.value("enabled", c.motion.enabled);
      c.motion.analysisWidth = j.value("analysis_width", c.motion.analysisWidth);
      c.motion.blur = j.value("blur", c.motion.blur);
      c.motion.pixelThreshold = j.value("pixel_threshold", c.motion.pixelThreshold);
      c.motion.areaPercent = j.value("area_percent", c.motion.areaPercent);
      c.motion.holdSeconds = j.value("hold_seconds", c.motion.holdSeconds);
      c.motion.heartbeatSeconds = j.value("heartbeat_seconds", c.motion.heartbeatSeconds);
      c.motion.bgAlpha = j.value("bg_alpha", c.motion.bgAlpha); }
    { auto j = section(root, "zone");
      c.zone.clear();
      if (j.contains("polygon") && j["polygon"].is_array())
        for (auto& p : j["polygon"])
          if (p.is_array() && p.size() == 2) c.zone.push_back({p[0].get<float>(), p[1].get<float>()}); }
    { auto j = section(root, "logic");
      c.logic.confirmSeconds = j.value("confirm_seconds", c.logic.confirmSeconds);
      c.logic.minHits = j.value("min_hits", c.logic.minHits);
      c.logic.gapToleranceSec = j.value("gap_tolerance_seconds", c.logic.gapToleranceSec);
      c.logic.suspectTimeoutSec = j.value("suspect_timeout_seconds", c.logic.suspectTimeoutSec);
      c.logic.doorWindowSec = j.value("door_window_seconds", c.logic.doorWindowSec);
      c.logic.doorMinHits = j.value("door_min_hits", c.logic.doorMinHits);
      c.logic.alarmMaxSec = j.value("alarm_max_seconds", c.logic.alarmMaxSec);
      c.logic.rearmCooldownSec = j.value("rearm_cooldown_seconds", c.logic.rearmCooldownSec);
      c.logic.lightOnSuspect = j.value("light_on_suspect", c.logic.lightOnSuspect); }
    { auto j = section(root, "schedule");
      c.schedule.enabled = j.value("enabled", c.schedule.enabled);
      c.schedule.armMin = parseHHMM(j.value("arm", "21:00"), c.schedule.armMin);
      c.schedule.disarmMin = parseHHMM(j.value("disarm", "07:00"), c.schedule.disarmMin); }
    { auto j = section(root, "hardware");
      c.hardware.mode = j.value("mode", c.hardware.mode);
      c.hardware.gpioChip = j.value("gpio_chip", c.hardware.gpioChip);
      c.hardware.i2cDev = j.value("i2c_dev", c.hardware.i2cDev);
      c.hardware.rtcEnabled = j.value("rtc_enabled", c.hardware.rtcEnabled);
      c.hardware.inaEnabled = j.value("ina219_enabled", c.hardware.inaEnabled);
      c.hardware.inaAddr = j.value("ina219_addr", c.hardware.inaAddr);
      c.hardware.shuntOhm = j.value("shunt_ohm", c.hardware.shuntOhm);
      c.hardware.oledEnabled = j.value("oled_enabled", c.hardware.oledEnabled);
      c.hardware.spiDev = j.value("spi_dev", c.hardware.spiDev);
      c.hardware.batteryLowV = j.value("battery_low_v", c.hardware.batteryLowV);
      c.hardware.batteryCriticalV = j.value("battery_critical_v", c.hardware.batteryCriticalV);
      c.hardware.shutdownOnCritical = j.value("shutdown_on_critical", c.hardware.shutdownOnCritical);
      auto pins = section(j, "pins");
      for (auto it = pins.begin(); it != pins.end(); ++it) {
        PinCfg p;
        p.line = it.value().value("line", -1);
        p.activeLow = it.value().value("active_low", false);
        p.pullUp = it.value().value("pull_up", false);
        c.hardware.pins[it.key()] = p;
      } }
    { auto j = section(root, "alerts");
      c.alerts.notifyArmDisarm = j.value("notify_arm_disarm", c.alerts.notifyArmDisarm);
      c.alerts.retryMax = j.value("retry_max", c.alerts.retryMax);
      auto t = section(j, "telegram");
      c.alerts.telegram.enabled = t.value("enabled", false);
      c.alerts.telegram.token = t.value("token", "");
      c.alerts.telegram.chatId = t.value("chat_id", "");
      auto m = section(j, "mqtt");
      c.alerts.mqtt.enabled = m.value("enabled", false);
      c.alerts.mqtt.host = m.value("host", c.alerts.mqtt.host);
      c.alerts.mqtt.port = m.value("port", c.alerts.mqtt.port);
      c.alerts.mqtt.topicPrefix = m.value("topic_prefix", c.alerts.mqtt.topicPrefix);
      c.alerts.mqtt.username = m.value("username", "");
      c.alerts.mqtt.password = m.value("password", ""); }
    { auto j = section(root, "http");
      c.http.host = j.value("host", c.http.host);
      c.http.port = j.value("port", c.http.port);
      c.http.token = j.value("token", c.http.token);
      c.http.webDir = j.value("web_dir", c.http.webDir); }
    { auto j = section(root, "storage");
      c.storage.dataDir = j.value("data_dir", c.storage.dataDir);
      c.storage.preSeconds = j.value("pre_seconds", c.storage.preSeconds);
      c.storage.postSeconds = j.value("post_seconds", c.storage.postSeconds);
      c.storage.clipFps = j.value("clip_fps", c.storage.clipFps);
      c.storage.keepDays = j.value("keep_days", c.storage.keepDays); }
  } catch (const std::exception& e) {
    err = std::string("config value error: ") + e.what();
    return false;
  }

  // Secrets can come from the environment so they stay out of git.
  envOverride("GUARDEYE_TELEGRAM_TOKEN", c.alerts.telegram.token);
  envOverride("GUARDEYE_TELEGRAM_CHAT_ID", c.alerts.telegram.chatId);
  envOverride("GUARDEYE_HTTP_TOKEN", c.http.token);
  envOverride("GUARDEYE_MQTT_PASSWORD", c.alerts.mqtt.password);

  if (c.camera.width < 64 || c.camera.height < 64) { err = "camera width/height too small"; return false; }
  if (c.detector.inputSize < 32 || c.detector.inputSize % 32 != 0) {
    err = "detector.input_size must be a multiple of 32"; return false;
  }
  return true;
}

// ---------------------------------------------------------------- ZoneStore
ZoneStore::ZoneStore(std::vector<Pt> p, std::string configPath)
    : poly_(std::move(p)), configPath_(std::move(configPath)) {}

std::vector<Pt> ZoneStore::get() const {
  std::lock_guard<std::mutex> lk(m_);
  return poly_;
}

bool ZoneStore::set(const std::vector<Pt>& p, std::string& err) {
  if (!p.empty() && p.size() < 3) { err = "polygon needs at least 3 points (or 0 to clear)"; return false; }
  for (auto& q : p)
    if (q.x < 0 || q.x > 1 || q.y < 0 || q.y > 1) { err = "points must be normalised to 0..1"; return false; }

  std::lock_guard<std::mutex> lk(m_);
  poly_ = p;
  if (configPath_.empty()) return true;

  // Persist: re-read the file, replace only zone.polygon, write atomically.
  try {
    std::ifstream in(configPath_);
    json root = in ? json::parse(in, nullptr, true, true) : json::object();
    json arr = json::array();
    for (auto& q : p) arr.push_back({q.x, q.y});
    root["zone"]["polygon"] = arr;
    std::string tmp = configPath_ + ".tmp";
    { std::ofstream out(tmp); out << root.dump(2) << "\n"; if (!out) throw std::runtime_error("write failed"); }
    if (std::rename(tmp.c_str(), configPath_.c_str()) != 0) throw std::runtime_error("rename failed");
  } catch (const std::exception& e) {
    LOG_WARN << "zone applied in memory but not saved to " << configPath_ << ": " << e.what();
  }
  return true;
}

}  // namespace guardeye
