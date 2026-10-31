// YOLOv8 person detector using OpenCV's DNN module (ONNX).
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <string>
#include <vector>

#include "config.h"

namespace guardeye {

struct Detection {
  cv::Rect2f box;  // pixels in the ORIGINAL frame
  float conf = 0;
};

class Detector {
 public:
  explicit Detector(const DetectorCfg& cfg) : cfg_(cfg) {}
  bool load(std::string& err);
  bool loaded() const { return loaded_; }

  // Full pipeline: letterbox -> blob -> forward -> postprocess.
  std::vector<Detection> detect(const cv::Mat& bgr);

  // Turn the raw YOLOv8 tensor [1, 4+nc, N] (or [1, N, 4+nc]) into person boxes.
  // Public + static so it can be unit-tested with synthetic data.
  static std::vector<Detection> postprocess(const cv::Mat& raw, float scale, int padX, int padY,
                                            int origW, int origH, float confThr, float nmsThr,
                                            int personClass);

 private:
  DetectorCfg cfg_;
  cv::dnn::Net net_;
  bool loaded_ = false;
};

}  // namespace guardeye
