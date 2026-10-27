// Event history in SQLite (one file, no server, survives power cuts: WAL mode).
#pragma once
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>

struct sqlite3;

namespace guardeye {

class EventStore {
 public:
  ~EventStore();
  bool open(const std::string& path, std::string& err);
  int64_t add(std::time_t ts, const std::string& type, const std::string& detail,
              double confidence = 0, const std::string& snapshot = "", const std::string& clip = "");
  void setClip(int64_t id, const std::string& clip);
  std::string recentJson(int limit);   // JSON array, newest first
  int64_t count();
 private:
  sqlite3* db_ = nullptr;
  std::mutex m_;
};

}  // namespace guardeye
