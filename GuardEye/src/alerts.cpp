#include "alerts.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "log.h"
#include "util.h"

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

extern char** environ;

namespace guardeye {

void AlertManager::start() {
#ifdef HAVE_CURL
  curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
  running_ = true;
  th_ = std::thread(&AlertManager::run, this);
}

void AlertManager::stop() {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!running_) return;
    running_ = false;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

size_t AlertManager::pending() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }

void AlertManager::enqueue(Alert a) {
  LOG_INFO << "ALERT [" << a.kind << "] " << a.text;
  if (!cfg_.telegram.enabled) a.telegramDone = true;
  if (!cfg_.mqtt.enabled) a.mqttDone = true;
  if (!(a.channels & kTelegram)) a.telegramDone = true;
  if (!(a.channels & kMqtt)) a.mqttDone = true;
  if (a.telegramDone && a.mqttDone) return;  // nothing to deliver
  {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.size() >= 100) {  // bounded: drop the oldest non-alarm item first
      auto it = std::find_if(q_.begin(), q_.end(), [](const Alert& x) { return x.kind != "alarm"; });
      q_.erase(it == q_.end() ? q_.begin() : it);
    }
    q_.push_back(std::move(a));
  }
  cv_.notify_all();
}

void AlertManager::run() {
  while (true) {
    Alert a;
    {
      std::unique_lock<std::mutex> lk(m_);
      while (running_) {
        double now = monoNow();
        auto it = std::find_if(q_.begin(), q_.end(), [&](const Alert& x) { return x.notBefore <= now; });
        if (it != q_.end()) { a = std::move(*it); q_.erase(it); break; }
        cv_.wait_for(lk, std::chrono::seconds(1));
      }
      if (!running_) return;
    }

    if (!a.telegramDone) a.telegramDone = sendTelegram(a);
    if (!a.mqttDone) a.mqttDone = sendMqtt(a);

    if (!(a.telegramDone && a.mqttDone)) {
      if (++a.attempts > cfg_.retryMax) {
        LOG_ERROR << "alert dropped after " << a.attempts << " attempts: " << a.text;
      } else {
        a.notBefore = monoNow() + std::min(60.0, 5.0 * std::pow(2.0, a.attempts - 1));  // 5s,10s,20s,40s,60s
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(std::move(a));
      }
    }
  }
}

// ------------------------------------------------------------------ Telegram
#ifdef HAVE_CURL
bool AlertManager::sendTelegram(const Alert& a) {
  if (cfg_.telegram.token.empty() || cfg_.telegram.chatId.empty()) {
    LOG_WARN << "telegram enabled but token/chat_id missing";
    return true;  // do not retry a config error forever
  }
  CURL* c = curl_easy_init();
  if (!c) return false;
  const bool photo = !a.snapshotPath.empty() && access(a.snapshotPath.c_str(), R_OK) == 0;
  std::string url = "https://api.telegram.org/bot" + cfg_.telegram.token + (photo ? "/sendPhoto" : "/sendMessage");

  curl_mime* mime = curl_mime_init(c);
  auto field = [&](const char* name, const std::string& v) {
    curl_mimepart* p = curl_mime_addpart(mime);
    curl_mime_name(p, name);
    curl_mime_data(p, v.c_str(), CURL_ZERO_TERMINATED);
  };
  field("chat_id", cfg_.telegram.chatId);
  field(photo ? "caption" : "text", a.text);
  if (photo) {
    curl_mimepart* p = curl_mime_addpart(mime);
    curl_mime_name(p, "photo");
    curl_mime_filedata(p, a.snapshotPath.c_str());
  }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) { return s * n; });  // discard body

  CURLcode rc = curl_easy_perform(c);
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_mime_free(mime);
  curl_easy_cleanup(c);
  bool ok = (rc == CURLE_OK && http == 200);
  // The URL contains the bot token, so it is never logged.
  if (!ok) LOG_WARN << "telegram send failed (" << (rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(http)) << ")";
  return ok;
}
#else
bool AlertManager::sendTelegram(const Alert&) {
  LOG_WARN << "built without libcurl: telegram disabled";
  return true;
}
#endif

// ---------------------------------------------------------------------- MQTT
bool AlertManager::sendMqtt(const Alert& a) {
  const auto& m = cfg_.mqtt;
  std::string topic = m.topicPrefix + "/" + (a.mqttSubtopic.empty() ? a.kind : a.mqttSubtopic);
  std::string port = std::to_string(m.port);
  std::vector<std::string> args = {"timeout", "10", "mosquitto_pub", "-h", m.host, "-p", port,
                                   "-t", topic, "-m", a.mqttPayload.empty() ? a.text : a.mqttPayload};
  if (!m.username.empty()) { args.push_back("-u"); args.push_back(m.username); }
  if (!m.password.empty()) { args.push_back("-P"); args.push_back(m.password); }
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

  // posix_spawnp with an argv ARRAY: no shell involved, so no injection risk.
  pid_t pid;
  if (posix_spawnp(&pid, "timeout", nullptr, nullptr, argv.data(), environ) != 0) {
    LOG_WARN << "mqtt: cannot run mosquitto_pub (sudo apt install mosquitto-clients)";
    return false;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!ok) LOG_WARN << "mqtt publish failed (broker " << m.host << ":" << m.port << ")";
  return ok;
}

}  // namespace guardeye
