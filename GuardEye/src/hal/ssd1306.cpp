#include "hal/ssd1306.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "hal/font5x7.h"
#include "log.h"

#if defined(__has_include)
#if __has_include(<linux/spi/spidev.h>)
#include <linux/spi/spidev.h>
#define GUARDEYE_HAVE_SPIDEV 1
#endif
#endif

namespace guardeye {

void ConsoleDisplay::show(const std::vector<std::string>& lines) {
  std::string s;
  for (auto& l : lines) if (!l.empty()) s += "\n    | " + l;
  if (s != last_) { last_ = s; LOG_INFO << "[sim-oled]" << s; }
}

Ssd1306Spi::~Ssd1306Spi() { if (fd_ >= 0) ::close(fd_); }

void Ssd1306Spi::render(const std::vector<std::string>& lines, std::vector<uint8_t>& fb) {
  fb.assign(128 * 8, 0);
  for (size_t row = 0; row < 8 && row < lines.size(); ++row) {
    int x = 0;
    for (char ch : lines[row]) {
      if (x + 6 > 128) break;
      unsigned char c = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)));
      if (c < 0x20 || c > 0x5F) c = '?';
      for (int col = 0; col < 5; ++col) fb[row * 128 + x + col] = kFont5x7[c - 0x20][col];
      x += 6;  // 5 px glyph + 1 px gap
    }
  }
}

bool Ssd1306Spi::cmd(const uint8_t* c, size_t n) {
  gpio_.write("oled_dc", false);  // DC low = command
  return ::write(fd_, c, n) == static_cast<ssize_t>(n);
}
bool Ssd1306Spi::data(const uint8_t* d, size_t n) {
  gpio_.write("oled_dc", true);   // DC high = display data
  return ::write(fd_, d, n) == static_cast<ssize_t>(n);
}

bool Ssd1306Spi::init(const std::string& spiDev, std::string& err) {
#ifndef GUARDEYE_HAVE_SPIDEV
  err = "built without <linux/spi/spidev.h>";
  return false;
#else
  fd_ = ::open(spiDev.c_str(), O_RDWR);
  if (fd_ < 0) { err = "cannot open " + spiDev + ": " + std::strerror(errno) + " (enable SPI in raspi-config)"; return false; }
  uint8_t mode = SPI_MODE_0, bits = 8;
  uint32_t speed = 8000000;
  if (ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
    err = "spidev ioctl failed";
    return false;
  }
  if (gpio_.has("oled_rst")) {  // hardware reset pulse
    gpio_.write("oled_rst", true);  std::this_thread::sleep_for(std::chrono::milliseconds(5));
    gpio_.write("oled_rst", false); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    gpio_.write("oled_rst", true);  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  static const uint8_t init[] = {
      0xAE,        // display off
      0xD5, 0x80,  // clock divide
      0xA8, 0x3F,  // multiplex 64
      0xD3, 0x00,  // display offset
      0x40,        // start line 0
      0x8D, 0x14,  // charge pump on
      0x20, 0x00,  // horizontal addressing mode
      0xA1, 0xC8,  // segment/COM remap (not upside down)
      0xDA, 0x12,  // COM pins
      0x81, 0xCF,  // contrast
      0xD9, 0xF1,  // pre-charge
      0xDB, 0x40,  // VCOM detect
      0xA4, 0xA6,  // resume RAM content, normal (not inverted)
      0xAF};       // display on
  if (!cmd(init, sizeof init)) { err = "SPI write failed (check wiring / DC pin)"; return false; }
  show({});
  return true;
#endif
}

void Ssd1306Spi::show(const std::vector<std::string>& lines) {
  if (fd_ < 0) return;
  std::vector<uint8_t> fb;
  render(lines, fb);
  if (fb == last_) return;  // only touch the bus when something changed
  last_ = fb;
  static const uint8_t window[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};  // columns 0-127, pages 0-7
  cmd(window, sizeof window);
  data(fb.data(), fb.size());
}

}  // namespace guardeye
