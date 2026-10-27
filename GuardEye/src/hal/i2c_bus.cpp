#include "hal/i2c_bus.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace guardeye {

namespace { constexpr unsigned long kI2C_SLAVE = 0x0703; }  // same value as linux/i2c-dev.h

I2CBus::~I2CBus() { if (fd_ >= 0) ::close(fd_); }

bool I2CBus::open(const std::string& dev, std::string& err) {
  fd_ = ::open(dev.c_str(), O_RDWR);
  if (fd_ < 0) { err = "cannot open " + dev + ": " + std::strerror(errno); return false; }
  return true;
}

bool I2CBus::select(uint8_t addr) { return ioctl(fd_, kI2C_SLAVE, addr) >= 0; }

bool I2CBus::readReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  std::lock_guard<std::mutex> lk(m_);
  if (fd_ < 0 || !select(addr)) return false;
  if (::write(fd_, &reg, 1) != 1) return false;           // set register pointer
  return ::read(fd_, buf, n) == static_cast<ssize_t>(n);  // then read n bytes
}

bool I2CBus::writeReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) {
  std::lock_guard<std::mutex> lk(m_);
  if (fd_ < 0 || !select(addr) || n > 32) return false;
  uint8_t tmp[33];
  tmp[0] = reg;
  std::memcpy(tmp + 1, data, n);
  return ::write(fd_, tmp, n + 1) == static_cast<ssize_t>(n + 1);
}

}  // namespace guardeye
