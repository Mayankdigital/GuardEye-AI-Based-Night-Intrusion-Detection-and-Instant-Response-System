#include "hal/ina219.h"

#include <algorithm>

namespace guardeye {

bool Ina219::readU16(uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!bus_.readReg(addr_, reg, b, 2)) return false;
  v = static_cast<uint16_t>((b[0] << 8) | b[1]);  // INA219 is big-endian
  return true;
}

bool Ina219::init() {
  // Config: 32 V bus range, +/-320 mV shunt range, 12-bit, continuous (0x399F)
  const uint8_t cfg[2] = {0x39, 0x9F};
  if (!bus_.writeReg(addr_, 0x00, cfg, 2)) return false;
  uint16_t back = 0;
  return readU16(0x00, back) && back == 0x399F;
}

PowerReading Ina219::read() {
  PowerReading r;
  uint16_t bus = 0, shunt = 0;
  if (!readU16(0x02, bus) || !readU16(0x01, shunt)) return r;
  r.busV = ((bus >> 3) * 4) / 1000.0;                      // 4 mV per LSB, bits 15..3
  double shuntMv = static_cast<int16_t>(shunt) * 0.01;     // 10 uV per LSB, signed
  r.currentMa = shuntMv / shunt_;                          // I = V / R  (mV / ohm = mA)
  r.ok = true;
  return r;
}

PowerReading SimPower::read() {
  if (mains_) v_ = std::min(12.6, v_ + 0.02);
  else v_ = std::max(9.0, v_ - 0.01);  // sags ~0.6 V/min on battery
  PowerReading r;
  r.ok = true;
  r.busV = v_;
  r.currentMa = mains_ ? 120.0 : -450.0;
  return r;
}

}  // namespace guardeye
