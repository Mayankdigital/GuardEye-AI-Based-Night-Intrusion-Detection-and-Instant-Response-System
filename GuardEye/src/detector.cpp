#include "detector.h"

#include <algorithm>
#include <opencv2/imgproc.hpp>

#include "log.h"

namespace guardeye {

bool Detector::load(std::string& err) {
  try {
    net_ = cv::dnn::readNetFromONNX(cfg_.modelPath);
    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    if (cfg_.numThreads > 0) cv::setNumThreads(cfg_.numThreads);
    // warm-up so the first real frame is not slow
    cv::Mat dummy(cfg_.inputSize, cfg_.inputSize, CV_8UC3, cv::Scalar(114, 114, 114));
    detect(dummy);
    loaded_ = true;
    return true;
  } catch (const cv::Exception& e) {
    err = "cannot load model '" + cfg_.modelPath + "': " + e.what();
    return false;
  }
}

std::vector<Detection> Detector::detect(const cv::Mat& bgr) {
  const int S = cfg_.inputSize;
  // Letterbox: keep aspect ratio, pad with grey so the person is not stretched.
  float scale = std::min(static_cast<float>(S) / bgr.cols, static_cast<float>(S) / bgr.rows);
  int nw = std::max(1, static_cast<int>(bgr.cols * scale));
  int nh = std::max(1, static_cast<int>(bgr.rows * scale));
  int padX = (S - nw) / 2, padY = (S - nh) / 2;
  cv::Mat resized, canvas(S, S, CV_8UC3, cv::Scalar(114, 114, 114));
  cv::resize(bgr, resized, cv::Size(nw, nh));
  resized.copyTo(canvas(cv::Rect(padX, padY, nw, nh)));

  // BGR->RGB, /255, HWC->NCHW
  cv::Mat blob = cv::dnn::blobFromImage(canvas, 1.0 / 255.0, cv::Size(S, S), cv::Scalar(), true, false);
  net_.setInput(blob);
  cv::Mat out = net_.forward();
  return postprocess(out, scale, padX, padY, bgr.cols, bgr.rows, cfg_.conf, cfg_.nms, cfg_.personClass);
}

std::vector<Detection> Detector::postprocess(const cv::Mat& raw, float scale, int padX, int padY,
                                             int origW, int origH, float confThr, float nmsThr,
                                             int personClass) {
  std::vector<Detection> result;
  if (raw.dims != 3 || raw.type() != CV_32F || !raw.isContinuous()) return result;

  int d1 = raw.size[1], d2 = raw.size[2];
  cv::Mat m(d1, d2, CV_32F, const_cast<float*>(raw.ptr<float>()));
  // YOLOv8 exports [1, 4+nc, N]; transpose so each row is one candidate box.
  cv::Mat rows = (d1 < d2) ? cv::Mat(m.t()) : m;
  int attrs = rows.cols;
  if (personClass < 0 || 4 + personClass >= attrs) return result;

  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  for (int i = 0; i < rows.rows; ++i) {
    const float* r = rows.ptr<float>(i);
    float score = r[4 + personClass];
    if (score < confThr) continue;
    float cx = r[0], cy = r[1], w = r[2], h = r[3];
    float x1 = (cx - w / 2 - padX) / scale;
    float y1 = (cy - h / 2 - padY) / scale;
    float x2 = (cx + w / 2 - padX) / scale;
    float y2 = (cy + h / 2 - padY) / scale;
    x1 = std::clamp(x1, 0.f, static_cast<float>(origW - 1));
    y1 = std::clamp(y1, 0.f, static_cast<float>(origH - 1));
    x2 = std::clamp(x2, 0.f, static_cast<float>(origW - 1));
    y2 = std::clamp(y2, 0.f, static_cast<float>(origH - 1));
    if (x2 - x1 < 2 || y2 - y1 < 2) continue;
    boxes.emplace_back(x1, y1, x2 - x1, y2 - y1);
    scores.push_back(score);
  }

  // NMS drops overlapping duplicates (keeps the highest-confidence box).
  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, confThr, nmsThr, keep);
  for (int idx : keep) {
    Detection d;
    d.box = cv::Rect2f(static_cast<float>(boxes[idx].x), static_cast<float>(boxes[idx].y),
                       static_cast<float>(boxes[idx].width), static_cast<float>(boxes[idx].height));
    d.conf = scores[idx];
    result.push_back(d);
  }
  return result;
}

}  // namespace guardeye
