#include "capture.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "log.h"
#include "util.h"

namespace guardeye {

namespace {
bool isRtsp(const std::string& u) { return u.rfind("rtsp://", 0) == 0 || u.rfind("rtsps://", 0) == 0; }
bool isNumber(const std::string& u) {
  return !u.empty() && std::all_of(u.begin(), u.end(), [](unsigned char c) { return std::isdigit(c); });
}
std::string gstQuote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o + "\"";
}
}  // namespace

std::string FrameGrabber::buildPipeline(const CameraCfg& c) {
  if (!c.gstPipeline.empty()) return c.gstPipeline;
  const bool h265 = (c.codec == "h265" || c.codec == "hevc");
  std::string p = "rtspsrc location=" + gstQuote(c.url) +
                  (c.rtspTcp ? " protocols=tcp" : "") +
                  " latency=" + std::to_string(c.latencyMs) + " drop-on-latency=true"
                  " ! " + (h265 ? "rtph265depay ! h265parse ! avdec_h265" : "rtph264depay ! h264parse ! avdec_h264") +
                  " ! videoconvert ! videoscale"
                  " ! video/x-raw,format=BGR,width=" + std::to_string(c.width) +
                  ",height=" + std::to_string(c.height) +
                  " ! appsink drop=true max-buffers=1 sync=false";
  return p;
}

void FrameGrabber::start() {
  if (running_.exchange(true)) return;
  startedMono_ = monoNow();
  th_ = std::thread(&FrameGrabber::run, this);
}

void FrameGrabber::stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

bool FrameGrabber::openCapture(cv::VideoCapture& cap) {
  const std::string& url = cfg_.url;
  try {
    if (isNumber(url)) {
      cap.open(std::stoi(url));
    } else if (isRtsp(url) && cfg_.useGstreamer) {
      cap.open(buildPipeline(cfg_), cv::CAP_GSTREAMER);
    } else if (isRtsp(url)) {
      if (cfg_.rtspTcp) setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp", 1);
      cap.open(url, cv::CAP_FFMPEG);
    } else {
      cap.open(url);  // video file (testing)
    }
  } catch (const cv::Exception& e) {
    LOG_WARN << "camera open exception: " << e.what();
    return false;
  }
  return cap.isOpened();
}

void FrameGrabber::run() {
  cv::VideoCapture cap;
  double backoff = 1.0;
  const bool isFile = !isNumber(cfg_.url) && !isRtsp(cfg_.url);
  double lastTs = 0;
  bool wasOpen = false;

  while (running_) {
    if (!cap.isOpened()) {
      if (!openCapture(cap)) {
        LOG_WARN << "camera: cannot open '" << (isRtsp(cfg_.url) ? "rtsp://***" : cfg_.url)
                 << "', retry in " << backoff << "s";
        // interruptible sleep
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::duration<double>(backoff), [&] { return !running_.load(); });
        backoff = std::min(backoff * 2, 10.0);
        continue;
      }
      if (wasOpen) ++reconnects_;
      wasOpen = true;
      backoff = 1.0;
      LOG_INFO << "camera: stream opened";
    }

    cv::Mat f;
    if (!cap.read(f) || f.empty()) {
      if (isFile && cfg_.loopFile) { cap.set(cv::CAP_PROP_POS_FRAMES, 0); continue; }
      LOG_WARN << "camera: read failed, reconnecting";
      cap.release();
      continue;
    }
    if (f.cols != cfg_.width || f.rows != cfg_.height) cv::resize(f, f, cv::Size(cfg_.width, cfg_.height));

    double now = monoNow();
    if (lastTs > 0) {
      double inst = 1.0 / std::max(1e-3, now - lastTs);
      fps_ = fps_ * 0.9 + inst * 0.1;
    }
    lastTs = now;
    lastFrameMono_ = now;

    if (cb_) cb_(f, now);
    {
      std::lock_guard<std::mutex> lk(m_);
      frame_ = f;
      ++seq_;
    }
    cv_.notify_all();

    if (isFile) {  // pace file playback like a real camera
      double fps = cap.get(cv::CAP_PROP_FPS);
      if (fps < 1 || fps > 120) fps = 25;
      std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / fps));
    }
  }
  cap.release();
}

bool FrameGrabber::waitLatest(cv::Mat& out, uint64_t& seq, int timeoutMs) {
  std::unique_lock<std::mutex> lk(m_);
  cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return seq_ > seq || !running_.load(); });
  if (seq_ <= seq || frame_.empty()) return false;
  out = frame_.clone();
  seq = seq_;
  return true;
}

bool FrameGrabber::online() const {
  double t = lastFrameMono_.load();
  return t > 0 && (monoNow() - t) < 5.0;
}

double FrameGrabber::offlineSeconds() const {
  if (online()) return 0;
  double t = lastFrameMono_.load();
  return monoNow() - (t > 0 ? t : startedMono_.load());
}

}  // namespace guardeye
