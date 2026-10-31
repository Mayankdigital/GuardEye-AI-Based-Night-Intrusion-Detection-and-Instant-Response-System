#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "schedule.h"
#include "zone.h"

namespace guardeye {

struct PinCfg {
  int line = -1;           // BCM / gpiochip line offset, -1 = not used
  bool activeLow = false;  // logical ON == electrical LOW
  bool pullUp = false;     // request internal pull-up (inputs)
};

struct CameraCfg {
  std::string url = "rtsp://192.168.1.10:554/stream2";  // or file path / "0" for webcam
  bool useGstreamer = true;
  std::string codec = "h264";         // "h264" | "h265" (used to build the GStreamer pipeline)
  std::string gstPipeline;            // optional full custom pipeline (must end in appsink)
  bool rtspTcp = true;        // RTP interleaved over TCP: more reliable than UDP
  int latencyMs = 100;        // rtspsrc jitter buffer
  int width = 640;            // analysis resolution (use the camera sub-stream)
  int height = 360;
  bool loopFile = true;       // when url is a video file (testing)
  double offlineAlertSec = 30;
};

struct DetectorCfg {
  std::string mode = "yolo";  // "yolo" | "motion_only" (demo/testing only)
  std::string modelPath = "models/yolov8n.onnx";
  int inputSize = 320;
  float conf = 0.45f;
  float nms = 0.45f;
  int personClass = 0;
  float minBoxHeightFrac = 0.08f;  // ignore tiny far-away boxes
  int numThreads = 0;              // 0 = OpenCV default
};

struct MotionCfg {
  bool enabled = true;
  int analysisWidth = 160;
  int blur = 5;
  int pixelThreshold = 25;
  double areaPercent = 0.5;
  double holdSeconds = 8;       // keep YOLO running after last motion
  double heartbeatSeconds = 5;  // run YOLO at least this often (still intruder)
  double bgAlpha = 0.02;
};

struct LogicCfg {
  double confirmSeconds = 2.0;   // person must stay in zone this long...
  int minHits = 5;               // ...with at least this many detections
  double gapToleranceSec = 1.0;  // gaps shorter than this keep the streak alive
  double suspectTimeoutSec = 6;  // no person for this long -> back to ARMED
  double doorWindowSec = 10;     // door-open counts as 2nd confirmation for this long
  int doorMinHits = 1;
  double alarmMaxSec = 120;      // siren safety cut-off
  double rearmCooldownSec = 10;
  bool lightOnSuspect = true;
};

struct HardwareCfg {
  std::string mode = "auto";  // "auto" (real if devices exist) | "sim" | "real"
  std::string gpioChip = "gpiochip0";  // Pi 5 usually "gpiochip4"
  std::map<std::string, PinCfg> pins;
  std::string i2cDev = "/dev/i2c-1";
  bool rtcEnabled = true;
  bool inaEnabled = true;
  int inaAddr = 0x40;
  double shuntOhm = 0.1;
  bool oledEnabled = true;
  std::string spiDev = "/dev/spidev0.0";
  double batteryLowV = 11.2;
  double batteryCriticalV = 10.8;
  bool shutdownOnCritical = false;
};

struct TelegramCfg { bool enabled = false; std::string token; std::string chatId; };
struct MqttCfg {
  bool enabled = false; std::string host = "127.0.0.1"; int port = 1883;
  std::string topicPrefix = "guardeye/shop1"; std::string username; std::string password;
};
struct AlertsCfg {
  TelegramCfg telegram; MqttCfg mqtt;
  bool notifyArmDisarm = true;
  int retryMax = 5;
};

struct HttpCfg {
  std::string host = "0.0.0.0";
  int port = 8080;
  std::string token;                 // empty = no auth (LAN testing only!)
  std::string webDir = "web";
};

struct StorageCfg {
  std::string dataDir = "./data";
  double preSeconds = 10;
  double postSeconds = 30;
  int clipFps = 10;
  int keepDays = 14;
};

struct Config {
  CameraCfg camera;
  DetectorCfg detector;
  MotionCfg motion;
  std::vector<Pt> zone;
  LogicCfg logic;
  Schedule schedule;
  HardwareCfg hardware;
  AlertsCfg alerts;
  HttpCfg http;
  StorageCfg storage;
  std::string path;  // file it was loaded from (zone edits are saved back)

  // Returns false and fills err on failure.
  static bool load(const std::string& path, Config& out, std::string& err);
};

// Thread-safe holder so the web UI can change the zone at runtime.
class ZoneStore {
 public:
  ZoneStore(std::vector<Pt> p, std::string configPath);
  std::vector<Pt> get() const;
  bool set(const std::vector<Pt>& p, std::string& err);  // validates + persists
 private:
  mutable std::mutex m_;
  std::vector<Pt> poly_;
  std::string configPath_;
};

}  // namespace guardeye
