// Camera ingest thread: RTSP (via GStreamer or FFmpeg), webcam index, or a
// video file for testing. Always keeps only the NEWEST frame so a slow
// consumer never builds up latency, and reconnects with back-off.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <thread>

#include "config.h"

namespace guardeye {

class FrameGrabber {
 public:
  using FrameCallback = std::function<void(const cv::Mat&, double monoTs)>;

  explicit FrameGrabber(const CameraCfg& cfg) : cfg_(cfg) {}
  ~FrameGrabber() { stop(); }

  void setFrameCallback(FrameCallback cb) { cb_ = std::move(cb); }
  void start();
  void stop();

  // Wait up to timeoutMs for a frame newer than `seq`. Copies it to `out`.
  bool waitLatest(cv::Mat& out, uint64_t& seq, int timeoutMs);

  bool online() const;                 // got a frame in the last few seconds
  double offlineSeconds() const;       // 0 when online
  double fps() const { return fps_.load(); }
  uint64_t reconnects() const { return reconnects_.load(); }

  // Exposed for tests / docs.
  static std::string buildPipeline(const CameraCfg& cfg);

 private:
  void run();
  bool openCapture(cv::VideoCapture& cap);

  CameraCfg cfg_;
  FrameCallback cb_;
  std::thread th_;
  std::atomic<bool> running_{false};

  std::mutex m_;
  std::condition_variable cv_;
  cv::Mat frame_;
  uint64_t seq_ = 0;

  std::atomic<double> lastFrameMono_{-1};
  std::atomic<double> startedMono_{0};
  std::atomic<double> fps_{0};
  std::atomic<uint64_t> reconnects_{0};
};

}  // namespace guardeye
