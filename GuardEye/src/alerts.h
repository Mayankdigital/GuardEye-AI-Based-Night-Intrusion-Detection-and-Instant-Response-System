// Alert delivery on its OWN thread so slow/failed network calls can never
// delay the siren. Telegram (HTTPS, libcurl) + MQTT (mosquitto_pub), with
// per-channel retry/back-off when the internet is down.
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"

namespace guardeye {

enum AlertChannel { kTelegram = 1, kMqtt = 2, kAllChannels = 3 };

struct Alert {
  std::string kind;          // "alarm" | "warn" | "info"
  std::string text;          // human message (Telegram caption / log)
  std::string snapshotPath;  // optional JPEG sent with Telegram
  std::string mqttSubtopic;  // e.g. "alarm"
  std::string mqttPayload;   // JSON string
  int channels = kAllChannels;
  // internal retry state
  int attempts = 0;
  double notBefore = 0;
  bool telegramDone = false, mqttDone = false;
};

class AlertManager {
 public:
  explicit AlertManager(const AlertsCfg& cfg) : cfg_(cfg) {}
  ~AlertManager() { stop(); }
  void start();
  void stop();
  void enqueue(Alert a);
  size_t pending();

 private:
  void run();
  bool sendTelegram(const Alert& a);
  bool sendMqtt(const Alert& a);

  AlertsCfg cfg_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Alert> q_;
  std::thread th_;
  bool running_ = false;
};

}  // namespace guardeye
