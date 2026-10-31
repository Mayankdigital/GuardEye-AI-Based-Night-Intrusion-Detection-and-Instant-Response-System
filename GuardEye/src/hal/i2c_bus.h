// Minimal Linux i2c-dev wrapper. One bus, many devices, each with its own
// 7-bit address: select the device with ioctl(I2C_SLAVE), then read/write.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace guardeye {

class I2CBus {
 public:
  ~I2CBus();
  bool open(const std::string& dev, std::string& err);
  bool readReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n);
  bool writeReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n);
  bool isOpen() const { return fd_ >= 0; }
 private:
  bool select(uint8_t addr);
  int fd_ = -1;
  std::mutex m_;
};

}  // namespace guardeye
