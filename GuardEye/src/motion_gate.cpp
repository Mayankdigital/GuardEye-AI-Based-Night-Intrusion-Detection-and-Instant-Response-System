#include "motion_gate.h"

#include <opencv2/imgproc.hpp>

namespace guardeye {

MotionResult MotionGate::update(const cv::Mat& frame, double now, bool force) {
  MotionResult r;
  ++seen_;
  if (frame.empty()) return r;

  // 1) downscale + grayscale + blur: less data, less sensor/IR noise
  double scale = 1.0;
  cv::Mat small;
  if (cfg_.analysisWidth > 0 && frame.cols > cfg_.analysisWidth) {
    scale = static_cast<double>(cfg_.analysisWidth) / frame.cols;
    cv::resize(frame, small, cv::Size(), scale, scale, cv::INTER_AREA);
  } else {
    small = frame;
  }
  cv::Mat gray;
  cv::cvtColor(small, gray, small.channels() == 1 ? cv::COLOR_GRAY2BGR : cv::COLOR_BGR2GRAY);
  int k = cfg_.blur | 1;  // must be odd
  if (k >= 3) cv::GaussianBlur(gray, gray, cv::Size(k, k), 0);

  // 2) compare with the running-average background
  if (bg_.empty() || bgSize_ != gray.size()) {
    gray.convertTo(bg_, CV_32F);
    bgSize_ = gray.size();
    r.runDetector = true;  // first frame: nothing to compare yet
    lastRun_ = now;
    ++passed_;
    return r;
  }
  cv::Mat bg8, diff, mask;
  bg_.convertTo(bg8, CV_8U);
  cv::absdiff(gray, bg8, diff);
  cv::threshold(diff, mask, cfg_.pixelThreshold, 255, cv::THRESH_BINARY);
  r.changedPercent = 100.0 * cv::countNonZero(mask) / static_cast<double>(mask.total());
  cv::accumulateWeighted(gray, bg_, cfg_.bgAlpha);

  r.motion = r.changedPercent > cfg_.areaPercent;
  if (r.motion) {
    lastMotion_ = now;
    // bounding box of the largest moving blob (used by motion_only mode)
    cv::dilate(mask, mask, cv::Mat(), cv::Point(-1, -1), 2);
    std::vector<std::vector<cv::Point>> cs;
    cv::findContours(mask, cs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    double best = 0;
    for (auto& c : cs) {
      double a = cv::contourArea(c);
      if (a > best) {
        best = a;
        cv::Rect b = cv::boundingRect(c);
        r.box = cv::Rect(static_cast<int>(b.x / scale), static_cast<int>(b.y / scale),
                         static_cast<int>(b.width / scale), static_cast<int>(b.height / scale));
        r.hasBox = true;
      }
    }
  }

  // 3) decision: motion now, recent motion (person standing still), or heartbeat
  if (!cfg_.enabled || force) r.runDetector = true;
  else r.runDetector = r.motion || (now - lastMotion_ < cfg_.holdSeconds) ||
                       (now - lastRun_ >= cfg_.heartbeatSeconds);
  if (r.runDetector) { lastRun_ = now; ++passed_; }
  return r;
}

}  // namespace guardeye
