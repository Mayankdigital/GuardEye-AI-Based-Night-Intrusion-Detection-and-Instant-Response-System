// Feeds a synthetic YOLOv8 output tensor [1, 84, N] through postprocess().
#include <vector>
#include "check.h"
#include "detector.h"
using namespace guardeye;

static void setBox(cv::Mat& t, int i, float cx, float cy, float w, float h, int cls, float score) {
  int N = t.size[2];
  float* p = t.ptr<float>();
  p[0 * N + i] = cx; p[1 * N + i] = cy; p[2 * N + i] = w; p[3 * N + i] = h;
  p[(4 + cls) * N + i] = score;
}

int main() {
  const int N = 200;   // real YOLOv8n has 8400 (320 input: 2100) candidates, always > 84 attributes
  int sz[3] = {1, 84, N};
  cv::Mat out(3, sz, CV_32F, cv::Scalar(0));

  // Original frame 640x360 letterboxed into 320x320: scale=0.5, padY=(320-180)/2=70, padX=0
  float scale = 0.5f; int padX = 0, padY = 70;
  // a person at model coords centre (160,160) size 40x80  -> original centre (320,180), 80x160
  setBox(out, 0, 160, 160, 40, 80, 0, 0.90f);
  // duplicate overlapping box for the same person (NMS must drop it)
  setBox(out, 1, 162, 161, 40, 80, 0, 0.70f);
  // a car (class 2): must be ignored
  setBox(out, 2, 60, 150, 50, 30, 2, 0.95f);
  // low-confidence person: ignored
  setBox(out, 3, 250, 160, 30, 60, 0, 0.20f);

  auto dets = Detector::postprocess(out, scale, padX, padY, 640, 360, 0.45f, 0.45f, 0);
  CHECK(dets.size() == 1);
  CHECK(dets[0].conf > 0.89f);
  // letterbox undone: x=(160-20-0)/0.5=280, y=(160-40-70)/0.5=100, w=80, h=160
  CHECK(std::abs(dets[0].box.x - 280) < 1.0f);
  CHECK(std::abs(dets[0].box.y - 100) < 1.0f);
  CHECK(std::abs(dets[0].box.width - 80) < 1.0f);
  CHECK(std::abs(dets[0].box.height - 160) < 1.0f);

  // transposed layout [1, N, 84] must work too
  int sz2[3] = {1, N, 84};
  cv::Mat out2(3, sz2, CV_32F, cv::Scalar(0));
  float* p = out2.ptr<float>();
  p[0 * 84 + 0] = 160; p[0 * 84 + 1] = 160; p[0 * 84 + 2] = 40; p[0 * 84 + 3] = 80; p[0 * 84 + 4] = 0.8f;
  CHECK(Detector::postprocess(out2, scale, padX, padY, 640, 360, 0.45f, 0.45f, 0).size() == 1);

  // garbage input must not crash
  CHECK(Detector::postprocess(cv::Mat(), 1, 0, 0, 640, 360, 0.4f, 0.4f, 0).empty());
  PASS("postprocess");
  return 0;
}
