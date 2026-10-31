// INA219 high-side current/voltage monitor (I2C 0x40) for the 12 V battery.
#pragma once
#include <atomic>

#include "hal/i2c_bus.h"

namespace guardeye {

struct PowerReading {
  bool ok = false;
  double busV = 0;       // battery voltage
  double currentMa = 0;  // signed
};

class PowerMonitor {
 public:
  virtual ~PowerMonitor() = default;
  virtual PowerReading read() = 0;
  virtual const char* name() const = 0;
};

class Ina219 : public PowerMonitor {
 public:
  Ina219(I2CBus& bus, uint8_t addr, double shuntOhm) : bus_(bus), addr_(addr), shunt_(shuntOhm) {}
  bool init();
  PowerReading read() override;
  const char* name() const override { return "INA219"; }
 private:
  bool readU16(uint8_t reg, uint16_t& v);
  I2CBus& bus_;
  uint8_t addr_;
  double shunt_;
};

// Simulated battery: sags slowly when "mains" is off so low-battery logic can be demoed.
class SimPower : public PowerMonitor {
 public:
  PowerReading read() override;
  const char* name() const override { return "sim-battery"; }
  void setMains(bool ok) { mains_ = ok; }
 private:
  std::atomic<bool> mains_{true};
  double v_ = 12.6;
};

}  // namespace guardeye
