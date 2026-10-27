#include "http_server.h"

#include <filesystem>
#include <opencv2/imgcodecs.hpp>

#include "httplib.h"
#include "json.hpp"
#include "log.h"
#include "sysinfo.h"
#include "util.h"

namespace guardeye {
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
// Constant-time compare so the token cannot be guessed from response timing.
bool safeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}
void jsonReply(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}
}  // namespace

HttpServer::HttpServer(const Config& cfg, Controller& ctl, EventStore& store, AppState& app, ZoneStore& zone, Hardware& hw)
    : cfg_(cfg), ctl_(ctl), store_(store), app_(app), zone_(zone), hw_(hw), svr_(new httplib::Server()) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::authorized(const std::string& authHeader, const std::string& queryToken) const {
  if (cfg_.http.token.empty()) return true;
  const std::string bearer = "Bearer ";
  if (authHeader.rfind(bearer, 0) == 0 && safeEquals(authHeader.substr(bearer.size()), cfg_.http.token)) return true;
  return !queryToken.empty() && safeEquals(queryToken, cfg_.http.token);
}

void HttpServer::routes() {
  auto& s = *svr_;
  s.set_read_timeout(10, 0);
  s.set_write_timeout(10, 0);
  s.set_payload_max_length(64 * 1024);

  // Everything except the static page itself needs the token (when one is set).
  s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    const std::string& p = req.path;
    bool protectedPath = p.rfind("/api/", 0) == 0 || p.rfind("/snapshots/", 0) == 0 || p.rfind("/clips/", 0) == 0;
    if (protectedPath && !authorized(req.get_header_value("Authorization"), req.get_param_value("token"))) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
    Status st = app_.get();
    jsonReply(res, {{"version", GUARDEYE_VERSION}, {"state", st.state}, {"time", formatLocal(hw_.wallClock())},
                    {"camera_online", st.cameraOnline}, {"capture_fps", st.captureFps}, {"infer_ms", st.inferMs},
                    {"gate_pass_ratio", st.gatePassRatio}, {"motion_percent", st.motionPercent},
                    {"persons", st.persons}, {"cpu_percent", st.cpuPercent}, {"cpu_temp_c", st.cpuTempC},
                    {"door_open", st.doorOpen}, {"mains_ok", st.mainsOk},
                    {"battery_ok", st.batteryOk}, {"battery_v", st.batteryV}, {"battery_ma", st.batteryMa},
                    {"last_event", st.lastEvent}, {"ip", st.ip}, {"hardware", st.hardware},
                    {"reconnects", st.reconnects}, {"rtc", hw_.rtcPresent()}, {"simulated", hw_.simulated()},
                    {"events_total", store_.count()}});
  });

  s.Post("/api/arm", [this](const httplib::Request&, httplib::Response& res) { ctl_.requestArm("web"); jsonReply(res, {{"ok", true}}); });
  s.Post("/api/disarm", [this](const httplib::Request&, httplib::Response& res) { ctl_.requestDisarm("web"); jsonReply(res, {{"ok", true}}); });
  s.Post("/api/silence", [this](const httplib::Request&, httplib::Response& res) { ctl_.requestSilence(); jsonReply(res, {{"ok", true}}); });

  s.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
    int limit = 50;
    if (req.has_param("limit")) { try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {} }
    res.set_content(store_.recentJson(limit), "application/json");
  });

  s.Get("/api/snapshot.jpg", [this](const httplib::Request&, httplib::Response& res) {
    cv::Mat f;
    if (!app_.getFrame(f)) { jsonReply(res, {{"error", "no frame yet"}}, 404); return; }
    std::vector<uchar> buf;
    cv::imencode(".jpg", f, buf, {cv::IMWRITE_JPEG_QUALITY, 85});
    res.set_content(std::string(buf.begin(), buf.end()), "image/jpeg");
  });

  // Live view: multipart/x-mixed-replace, works in a plain <img> tag.
  s.Get("/api/stream.mjpg", [this](const httplib::Request&, httplib::Response& res) {
    if (streams_.load() >= 3) { jsonReply(res, {{"error", "too many viewers"}}, 503); return; }
    ++streams_;
    auto seq = std::make_shared<uint64_t>(0);
    res.set_chunked_content_provider(
        "multipart/x-mixed-replace; boundary=frame",
        [this, seq](size_t, httplib::DataSink& sink) {
          if (!running_) return false;
          cv::Mat f;
          if (!app_.waitFrame(f, *seq, 1000)) return true;  // no new frame yet, keep connection
          std::vector<uchar> buf;
          cv::imencode(".jpg", f, buf, {cv::IMWRITE_JPEG_QUALITY, 70});
          std::string head = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(buf.size()) + "\r\n\r\n";
          if (!sink.write(head.data(), head.size())) return false;
          if (!sink.write(reinterpret_cast<const char*>(buf.data()), buf.size())) return false;
          if (!sink.write("\r\n", 2)) return false;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));  // cap at ~10 fps per viewer
          return true;
        },
        [this](bool) { --streams_; });
  });

  s.Get("/api/zone", [this](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& p : zone_.get()) arr.push_back({p.x, p.y});
    jsonReply(res, {{"polygon", arr}});
  });
  auto setZone = [this](const httplib::Request& req, httplib::Response& res) {
    try {
      json j = json::parse(req.body);
      std::vector<Pt> poly;
      for (auto& p : j.at("polygon")) poly.push_back({p.at(0).get<float>(), p.at(1).get<float>()});
      std::string err;
      if (!zone_.set(poly, err)) { jsonReply(res, {{"error", err}}, 400); return; }
      jsonReply(res, {{"ok", true}, {"points", poly.size()}});
    } catch (const std::exception& e) {
      jsonReply(res, {{"error", std::string("bad request: ") + e.what()}}, 400);
    }
  };
  s.Put("/api/zone", setZone);
  s.Post("/api/zone", setZone);

  // Simulation helpers (only exist in --sim mode).
  if (hw_.simulated()) {
    s.Post("/api/sim/door", [this](const httplib::Request& req, httplib::Response& res) {
      bool open = req.get_param_value("open") == "1";
      hw_.simSetDoor(open);
      jsonReply(res, {{"ok", true}, {"door_open", open}});
    });
    s.Post("/api/sim/mains", [this](const httplib::Request& req, httplib::Response& res) {
      bool ok = req.get_param_value("ok") != "0";
      hw_.simSetMains(ok);
      jsonReply(res, {{"ok", true}, {"mains_ok", ok}});
    });
  }

  // Static files: dashboard, plus evidence (token-protected above).
  std::error_code ec;
  fs::create_directories(cfg_.storage.dataDir + "/snapshots", ec);
  fs::create_directories(cfg_.storage.dataDir + "/clips", ec);
  if (!s.set_mount_point("/snapshots", cfg_.storage.dataDir + "/snapshots")) LOG_WARN << "cannot mount snapshots dir";
  if (!s.set_mount_point("/clips", cfg_.storage.dataDir + "/clips")) LOG_WARN << "cannot mount clips dir";
  if (!s.set_mount_point("/", cfg_.http.webDir)) LOG_WARN << "web dir not found: " << cfg_.http.webDir << " (dashboard disabled, API still works)";
}

bool HttpServer::start(std::string& err) {
  routes();
  if (!svr_->bind_to_port(cfg_.http.host, cfg_.http.port)) {
    err = "cannot bind " + cfg_.http.host + ":" + std::to_string(cfg_.http.port) + " (port in use?)";
    return false;
  }
  running_ = true;
  th_ = std::thread([this] { svr_->listen_after_bind(); });
  if (cfg_.http.token.empty())
    LOG_WARN << "http.token is empty: API is OPEN to anyone on the network. Set one before deploying.";
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) return;
  svr_->stop();
  if (th_.joinable()) th_.join();
}

}  // namespace guardeye
