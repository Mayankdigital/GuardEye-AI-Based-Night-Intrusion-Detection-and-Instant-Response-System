// Pre/post-event video clips. Frames are kept in a RAM ring buffer as JPEG
// (about 20-40 KB each) so 10 s of history costs only a few MB.
#pragma once
#include <atomic>
#include <deque>
#include <memory>
#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

namespace guardeye {

class ClipRecorder {
 public:
  using DoneCallback = std::function<void(const std::string& path, bool ok)>;

  explicit ClipRecorder(const StorageCfg& cfg) : cfg_(cfg) {}
  ~ClipRecorder() { stop(); }

  // Called by the capture thread for every frame (cheap when throttled).
  void push(const cv::Mat& frame, double monoTs);

  // Start (or extend) a clip: ring buffer content + `postSeconds` more.
  void trigger(const std::string& path, DoneCallback done);

  void stop();  // joins writer threads

 private:
  struct Item { double ts; std::vector<uchar> jpg; };
  struct Worker { std::thread t; std::shared_ptr<std::atomic<bool>> finished; };
  void finalize(std::vector<Item> items, std::string path, DoneCallback done, cv::Size size);
  void spawn(std::vector<Item> items, std::string path, DoneCallback done, cv::Size size);  // m_ held
  void reap();  // join finished writer threads (m_ held)

  StorageCfg cfg_;
  std::mutex m_;
  std::deque<Item> ring_;
  double lastPush_ = -1;
  cv::Size frameSize_;

  bool active_ = false;
  double endTs_ = 0;
  std::string path_;
  DoneCallback done_;
  std::vector<Item> clip_;

  std::vector<Worker> workers_;
};

}  // namespace guardeye
