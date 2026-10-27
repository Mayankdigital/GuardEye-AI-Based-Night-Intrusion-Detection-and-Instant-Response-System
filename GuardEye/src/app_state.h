// Shared, thread-safe snapshot of system status + the latest annotated frame
// (read by the web dashboard and the OLED).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>

namespace guardeye {

struct Status {
  std::string state = "DISARMED";
  bool cameraOnline = false;
  double captureFps = 0;
  double inferMs = 0;
  double cpuPercent = 0;
  double cpuTempC = -1;
  bool doorOpen = false;
  bool mainsOk = true;
  bool batteryOk = false;
  double batteryV = 0;
  double batteryMa = 0;
  double motionPercent = 0;
  double gatePassRatio = 1;   // fraction of frames that reached YOLO
  int persons = 0;
  std::string lastEvent = "-";
  std::string ip;
  std::string hardware;
  uint64_t reconnects = 0;
};

class AppState {
 public:
  void update(const std::function<void(Status&)>& fn) { std::lock_guard<std::mutex> lk(m_); fn(st_); }
  Status get() const { std::lock_guard<std::mutex> lk(m_); return st_; }

  void setFrame(const cv::Mat& f) {
    { std::lock_guard<std::mutex> lk(fm_); frame_ = f; ++fseq_; }
    fcv_.notify_all();
  }
  bool getFrame(cv::Mat& out) const {
    std::lock_guard<std::mutex> lk(fm_);
    if (frame_.empty()) return false;
    out = frame_.clone();
    return true;
  }
  // Wait for a frame newer than `seq` (used by the MJPEG stream).
  bool waitFrame(cv::Mat& out, uint64_t& seq, int timeoutMs) {
    std::unique_lock<std::mutex> lk(fm_);
    fcv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return fseq_ > seq; });
    if (fseq_ <= seq || frame_.empty()) return false;
    out = frame_.clone();
    seq = fseq_;
    return true;
  }

 private:
  mutable std::mutex m_;
  Status st_;
  mutable std::mutex fm_;
  std::condition_variable fcv_;
  cv::Mat frame_;
  uint64_t fseq_ = 0;
};

}  // namespace guardeye
