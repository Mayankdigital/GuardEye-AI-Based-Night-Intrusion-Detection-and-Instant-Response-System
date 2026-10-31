#include "clip_recorder.h"

#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "log.h"

namespace guardeye {

void ClipRecorder::push(const cv::Mat& frame, double ts) {
  const double minGap = 1.0 / std::max(1, cfg_.clipFps);
  {
    std::lock_guard<std::mutex> lk(m_);
    if (lastPush_ >= 0 && ts - lastPush_ < minGap) return;
    lastPush_ = ts;
    frameSize_ = frame.size();

    Item it;
    it.ts = ts;
    cv::imencode(".jpg", frame, it.jpg, {cv::IMWRITE_JPEG_QUALITY, 80});

    if (active_) {
      clip_.push_back(it);
      if (ts >= endTs_) {
        auto items = std::move(clip_);
        clip_.clear();
        active_ = false;
        spawn(std::move(items), path_, done_, frameSize_);
      }
    }
    ring_.push_back(std::move(it));
    while (!ring_.empty() && ts - ring_.front().ts > cfg_.preSeconds) ring_.pop_front();
  }
}

void ClipRecorder::trigger(const std::string& path, DoneCallback done) {
  std::lock_guard<std::mutex> lk(m_);
  double now = ring_.empty() ? 0 : ring_.back().ts;
  endTs_ = now + cfg_.postSeconds;
  if (active_) return;  // already recording: just extended the end time
  active_ = true;
  path_ = path;
  done_ = std::move(done);
  clip_.assign(ring_.begin(), ring_.end());  // pre-event footage
}

void ClipRecorder::reap() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (it->finished->load()) { if (it->t.joinable()) it->t.join(); it = workers_.erase(it); }
    else ++it;
  }
}

void ClipRecorder::spawn(std::vector<Item> items, std::string path, DoneCallback done, cv::Size size) {
  reap();
  auto flag = std::make_shared<std::atomic<bool>>(false);
  std::thread t([this, flag, items = std::move(items), path = std::move(path), done = std::move(done), size]() mutable {
    finalize(std::move(items), std::move(path), std::move(done), size);
    flag->store(true);
  });
  workers_.push_back({std::move(t), flag});
}

void ClipRecorder::finalize(std::vector<Item> items, std::string path, DoneCallback done, cv::Size size) {
  bool ok = false;
  try {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    cv::VideoWriter w(path, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), cfg_.clipFps, size);
    if (w.isOpened()) {
      for (auto& it : items) {
        cv::Mat f = cv::imdecode(it.jpg, cv::IMREAD_COLOR);
        if (!f.empty()) w.write(f);
      }
      ok = true;
    }
  } catch (const std::exception& e) {
    LOG_ERROR << "clip write failed: " << e.what();
  }
  LOG_INFO << "clip " << (ok ? "saved: " : "FAILED: ") << path << " (" << items.size() << " frames)";
  if (done) done(path, ok);
}

void ClipRecorder::stop() {
  std::vector<Worker> w;
  {
    std::lock_guard<std::mutex> lk(m_);
    // flush a clip that is still being recorded
    if (active_ && !clip_.empty()) {
      auto items = std::move(clip_);
      clip_.clear();
      active_ = false;
      spawn(std::move(items), path_, done_, frameSize_);
    }
    w.swap(workers_);
  }
  for (auto& x : w) if (x.t.joinable()) x.t.join();
}

}  // namespace guardeye
