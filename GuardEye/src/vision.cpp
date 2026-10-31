#include "vision.h"

#include <opencv2/imgproc.hpp>

#include "log.h"
#include "state_machine.h"
#include "util.h"

namespace guardeye {

void Vision::start() {
  running_ = true;
  th_ = std::thread(&Vision::run, this);
}
void Vision::stop() {
  if (running_.exchange(false) && th_.joinable()) th_.join();
}

void Vision::run() {
  cv::Mat frame;
  uint64_t seq = 0;
  std::vector<Detection> lastDets;
  double lastDetTime = -1e9, inferMsEma = 0;
  const bool motionOnly = (cfg_.detector.mode == "motion_only");
  if (motionOnly) LOG_WARN << "detector.mode = motion_only: ANY motion in the zone counts as a person (demo/testing only)";

  while (running_) {
    ++heartbeat_;  // proves the loop is alive even when the camera is down
    if (!grabber_.waitLatest(frame, seq, 500)) continue;
    const double now = monoNow();
    const auto zone = zone_.get();

    // 1) motion gate (always while SUSPECT/ALARM, so the decision is never starved)
    MotionResult m = gate_.update(frame, now, ctl_.needContinuousDetection());

    // 2) detector, only when the gate says so
    bool ran = false;
    std::vector<Detection> dets;
    if (m.runDetector) {
      ran = true;
      if (motionOnly) {
        if (m.hasBox) {
          Detection d;
          d.box = cv::Rect2f(m.box);
          d.conf = 0.99f;
          dets.push_back(d);
        }
      } else if (detector_ && detector_->loaded()) {
        double t0 = monoNow();
        auto raw = detector_->detect(frame);
        double ms = (monoNow() - t0) * 1000.0;
        inferMsEma = inferMsEma == 0 ? ms : inferMsEma * 0.9 + ms * 0.1;
        for (auto& d : raw)
          if (d.box.height >= cfg_.detector.minBoxHeightFrac * frame.rows) dets.push_back(d);
      }
      lastDets = dets;
      lastDetTime = now;
    } else if (now - lastDetTime > 1.0) {
      lastDets.clear();
    }

    // 3) zone test: bottom-centre of each box (≈ feet) inside the polygon?
    bool inZone = false;
    float bestConf = 0;
    for (auto& d : (ran ? dets : lastDets)) {
      if (boxInZone(zone, d.box.x, d.box.y, d.box.width, d.box.height, frame.cols, frame.rows)) {
        inZone = true;
        bestConf = std::max(bestConf, d.conf);
      }
    }

    // 4) annotate (zone colour = state, boxes red when inside the zone)
    State st = ctl_.state();
    cv::Scalar zc = st == State::Alarm ? cv::Scalar(0, 0, 255)
                  : st == State::Suspect ? cv::Scalar(0, 165, 255)
                  : st == State::Armed ? cv::Scalar(0, 255, 255) : cv::Scalar(160, 160, 160);
    cv::Mat vis = frame;  // frame is already our private copy
    if (zone.size() >= 3) {
      std::vector<cv::Point> poly;
      for (auto& p : zone) poly.emplace_back(static_cast<int>(p.x * vis.cols), static_cast<int>(p.y * vis.rows));
      cv::polylines(vis, poly, true, zc, 2, cv::LINE_AA);
    }
    for (auto& d : lastDets) {
      bool in = boxInZone(zone, d.box.x, d.box.y, d.box.width, d.box.height, vis.cols, vis.rows);
      cv::rectangle(vis, d.box, in ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 2);
      cv::putText(vis, cv::format("person %.0f%%", d.conf * 100), cv::Point(static_cast<int>(d.box.x), std::max(12, static_cast<int>(d.box.y) - 4)),
                  cv::FONT_HERSHEY_SIMPLEX, 0.45, in ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    }
    cv::putText(vis, std::string(stateName(st)) + "  " + nowString(), {8, 18}, cv::FONT_HERSHEY_SIMPLEX, 0.55,
                {255, 255, 255}, 1, cv::LINE_AA);

    if (ran && inZone) ctl_.postPerson(std::make_shared<cv::Mat>(vis.clone()), bestConf);
    app_.setFrame(vis);

    // 5) stats for the dashboard / benchmarks
    app_.update([&](Status& s) {
      s.captureFps = grabber_.fps();
      s.reconnects = grabber_.reconnects();
      s.inferMs = inferMsEma;
      s.motionPercent = m.changedPercent;
      s.persons = static_cast<int>(lastDets.size());
      s.gatePassRatio = gate_.framesSeen() ? static_cast<double>(gate_.framesPassed()) / gate_.framesSeen() : 1.0;
    });
  }
}

}  // namespace guardeye
