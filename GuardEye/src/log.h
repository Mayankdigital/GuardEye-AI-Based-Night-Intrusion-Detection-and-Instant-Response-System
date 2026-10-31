// Tiny thread-safe logger: LOG_INFO << "text" << value;
#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>

namespace guardeye {

inline std::mutex& logMutex() { static std::mutex m; return m; }

class LogLine {
 public:
  explicit LogLine(const char* level) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    ss_ << buf << " [" << level << "] ";
  }
  ~LogLine() {
    std::lock_guard<std::mutex> lk(logMutex());
    std::cerr << ss_.str() << std::endl;  // stderr -> journald under systemd
  }
  template <class T> LogLine& operator<<(const T& v) { ss_ << v; return *this; }
 private:
  std::ostringstream ss_;
};

}  // namespace guardeye

#define LOG_INFO  ::guardeye::LogLine("INFO ")
#define LOG_WARN  ::guardeye::LogLine("WARN ")
#define LOG_ERROR ::guardeye::LogLine("ERROR")
