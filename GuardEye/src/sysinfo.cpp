#include "sysinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace guardeye {

double cpuTempC() {
  std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
  double milli = 0;
  if (f >> milli) return milli / 1000.0;
  return -1;
}

std::string primaryIPv4() {
  struct ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return "no network";
  std::string ip = "no network";
  for (auto* p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
    if (p->ifa_flags & IFF_LOOPBACK) continue;
    if (!(p->ifa_flags & IFF_UP)) continue;
    char buf[INET_ADDRSTRLEN];
    auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) { ip = buf; break; }
  }
  freeifaddrs(ifa);
  return ip;
}

double CpuMeter::sample() {
  std::ifstream f("/proc/stat");
  std::string cpu;
  uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  if (!(f >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal)) return 0;
  uint64_t idleAll = idle + iowait;
  uint64_t busy = user + nice + sys + irq + softirq + steal;
  uint64_t total = busy + idleAll;
  double pct = 0;
  if (lastTotal_ > 0 && total > lastTotal_) pct = 100.0 * (busy - lastBusy_) / static_cast<double>(total - lastTotal_);
  lastBusy_ = busy;
  lastTotal_ = total;
  return pct;
}

void sdNotify(const std::string& msg) {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (!path || !*path) return;
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t len = std::strlen(path);
  if (len >= sizeof addr.sun_path) { close(fd); return; }
  std::memcpy(addr.sun_path, path, len);
  if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract socket
  socklen_t alen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
  sendto(fd, msg.data(), msg.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr), alen);
  close(fd);
}

uint64_t watchdogIntervalUsec() {
  const char* v = std::getenv("WATCHDOG_USEC");
  return v ? std::strtoull(v, nullptr, 10) : 0;
}

}  // namespace guardeye
