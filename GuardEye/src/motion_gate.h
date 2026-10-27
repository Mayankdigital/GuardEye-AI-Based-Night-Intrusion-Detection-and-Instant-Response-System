// Cheap "is anything changing?" filter in front of the expensive YOLO model.
#pragma once
#include <atomic>
#include <opencv2/core.hpp>

#include "config.h"

namespace guardeye {

struct MotionResult {
  bool runDetector = true;   // should YOLO run on this frame?
  bool motion = false;       // did the frame differ from the background?
  double changedPercent = 0; // % of (downscaled) pixels that changed
  bool hasBox = false;       // bounding box of the largest moving blob (frame coords)
  cv::Rect box;
};

class MotionGate {
 public:
  explicit MotionGate(const MotionCfg& cfg) : cfg_(cfg) {}

  // force=true -> always run the detector (e.g. while SUSPECT/ALARM).
  MotionResult update(const cv::Mat& bgr, double now, bool force = false);

  uint64_t framesSeen() const { return seen_; }
  uint64_t framesPassed() const { return passed_; }

 private:
  MotionCfg cfg_;
  cv::Mat bg_;  // running-average background, CV_32F
  cv::Size bgSize_;
  double lastMotion_ = -1e9;
  double lastRun_ = -1e9;
  std::atomic<uint64_t> seen_{0}, passed_{0};
};

}  // namespace guardeye
