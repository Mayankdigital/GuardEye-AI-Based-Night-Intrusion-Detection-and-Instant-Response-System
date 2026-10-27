// Daily arm window, e.g. arm 21:00 -> disarm 07:00 (may wrap past midnight).
#pragma once
#include <cstdio>
#include <string>

namespace guardeye {

inline int parseHHMM(const std::string& s, int fallback) {
  int h = 0, m = 0;
  if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2) return fallback;
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return h * 60 + m;
}

struct Schedule {
  bool enabled = false;
  int armMin = 21 * 60;     // minutes since midnight
  int disarmMin = 7 * 60;

  bool wantsArmed(int minuteOfDay) const {
    if (armMin == disarmMin) return false;
    if (armMin < disarmMin) return minuteOfDay >= armMin && minuteOfDay < disarmMin;
    return minuteOfDay >= armMin || minuteOfDay < disarmMin;  // wraps midnight
  }
};

}  // namespace guardeye
