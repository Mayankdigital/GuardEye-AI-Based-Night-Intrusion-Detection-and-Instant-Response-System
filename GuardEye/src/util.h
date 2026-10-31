#pragma once
#include <chrono>
#include <string>
#include <ctime>
#include <cstdio>

namespace guardeye {

// Monotonic seconds (for timeouts, never goes backwards).
inline double monoNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline std::string formatLocal(std::time_t t, const char* fmt = "%Y-%m-%d %H:%M:%S") {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

inline std::string nowString(const char* fmt = "%Y-%m-%d %H:%M:%S") {
  return formatLocal(std::time(nullptr), fmt);
}

}  // namespace guardeye
