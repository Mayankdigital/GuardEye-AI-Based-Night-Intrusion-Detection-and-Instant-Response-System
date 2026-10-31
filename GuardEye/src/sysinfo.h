#pragma once
#include <cstdint>
#include <string>

namespace guardeye {

double cpuTempC();                 // -1 if unavailable (/sys/class/thermal)
std::string primaryIPv4();         // first non-loopback IPv4, or "no network"

// CPU usage in % between two calls (from /proc/stat).
class CpuMeter {
 public:
  double sample();
 private:
  uint64_t lastBusy_ = 0, lastTotal_ = 0;
};

// systemd integration without libsystemd: write to $NOTIFY_SOCKET.
void sdNotify(const std::string& msg);          // "READY=1", "WATCHDOG=1", "STATUS=..."
uint64_t watchdogIntervalUsec();                // WATCHDOG_USEC from env, 0 = not under watchdog
}  // namespace guardeye
