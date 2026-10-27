// DS3231 battery-backed RTC (I2C address 0x68). Registers hold BCD digits.
#pragma once
#include <ctime>

#include "hal/i2c_bus.h"

namespace guardeye {

class Rtc {
 public:
  virtual ~Rtc() = default;
  // Reads local calendar time. Returns false if unreadable or the oscillator
  // stopped (battery was removed: time is not trustworthy).
  virtual bool read(std::tm& out) = 0;
  virtual bool set(const std::tm& in) = 0;
  virtual const char* name() const = 0;
};

class Ds3231 : public Rtc {
 public:
  explicit Ds3231(I2CBus& bus, uint8_t addr = 0x68) : bus_(bus), addr_(addr) {}
  bool probe();
  bool read(std::tm& out) override;
  bool set(const std::tm& in) override;
  const char* name() const override { return "DS3231"; }

  static uint8_t bcd2bin(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
  static uint8_t bin2bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
 private:
  I2CBus& bus_;
  uint8_t addr_;
};

// Simulated RTC = system clock (used with --sim).
class SystemRtc : public Rtc {
 public:
  bool read(std::tm& out) override { std::time_t t = std::time(nullptr); localtime_r(&t, &out); return true; }
  bool set(const std::tm&) override { return false; }
  const char* name() const override { return "system-clock"; }
};

}  // namespace guardeye
