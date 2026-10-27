// REST API + static dashboard + MJPEG live view (cpp-httplib).
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app_state.h"
#include "config.h"
#include "controller.h"
#include "event_store.h"
#include "hal/hardware.h"

namespace httplib { class Server; }

namespace guardeye {

class HttpServer {
 public:
  HttpServer(const Config& cfg, Controller& ctl, EventStore& store, AppState& app, ZoneStore& zone, Hardware& hw);
  ~HttpServer();
  bool start(std::string& err);
  void stop();

 private:
  void routes();
  bool authorized(const std::string& authHeader, const std::string& queryToken) const;

  Config cfg_;
  Controller& ctl_;
  EventStore& store_;
  AppState& app_;
  ZoneStore& zone_;
  Hardware& hw_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread th_;
  std::atomic<int> streams_{0};
  std::atomic<bool> running_{false};
};

}  // namespace guardeye
