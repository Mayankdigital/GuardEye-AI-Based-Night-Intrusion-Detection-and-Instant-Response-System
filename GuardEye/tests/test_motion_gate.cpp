// Static scene -> gate keeps YOLO asleep; a moving object -> gate wakes it.
#include <opencv2/imgproc.hpp>
#include "check.h"
#include "motion_gate.h"
using namespace guardeye;

static cv::Mat scene(int personX = -1) {
  cv::Mat f(360, 640, CV_8UC3, cv::Scalar(40, 40, 40));
  cv::randu(f(cv::Rect(0, 0, 10, 10)), 35, 45);                 // tiny noise patch
  if (personX >= 0) cv::rectangle(f, cv::Rect(personX, 120, 70, 200), cv::Scalar(200, 200, 200), cv::FILLED);
  return f;
}

int main() {
  MotionCfg c;               // defaults: hold 8 s, heartbeat 5 s, area 0.5 %
  MotionGate g(c);
  double t = 0;
  g.update(scene(), t);                                   // first frame primes the background
  int woke = 0;
  for (int i = 0; i < 40; ++i) { t += 0.1; if (g.update(scene(), t).runDetector) ++woke; }
  CHECK(woke <= 1);                                       // quiet scene: YOLO (almost) never runs

  t += 0.1;
  MotionResult r = g.update(scene(300), t);               // a "person" appears
  CHECK(r.motion);
  CHECK(r.runDetector);
  CHECK(r.hasBox);
  CHECK(r.box.width > 40 && r.box.height > 100);

  // person freezes: no frame difference any more, but the hold time keeps YOLO running
  for (int i = 0; i < 20; ++i) { t += 0.1; (void)g.update(scene(300), t); }
  CHECK(g.update(scene(300), t + 0.1).runDetector);

  // heartbeat: even a totally static scene is checked once every heartbeat_seconds
  MotionGate g2(c);
  g2.update(scene(), 0);
  bool beat = false;
  for (double tt = 0.1; tt < 6.0; tt += 0.1) beat |= g2.update(scene(), tt).runDetector;
  CHECK(beat);

  // gating disabled -> always run
  MotionCfg off = c; off.enabled = false;
  MotionGate g3(off);
  g3.update(scene(), 0);
  CHECK(g3.update(scene(), 0.1).runDetector);

  // force flag (used while SUSPECT/ALARM)
  MotionGate g4(c);
  g4.update(scene(), 0);
  CHECK(g4.update(scene(), 0.1, true).runDetector);
  PASS("motion_gate");
  return 0;
}
