// 128x64 SSD1306 OLED over SPI (4-wire: SCK, MOSI, CS via spidev, plus DC and
// RST as plain GPIO). Text only: 8 lines x 21 characters.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "hal/gpio.h"

namespace guardeye {

class Display {
 public:
  virtual ~Display() = default;
  virtual void show(const std::vector<std::string>& lines) = 0;
};

// Simulation: print the "screen" to the log whenever its content changes.
class ConsoleDisplay : public Display {
 public:
  void show(const std::vector<std::string>& lines) override;
 private:
  std::string last_;
};

class Ssd1306Spi : public Display {
 public:
  Ssd1306Spi(Gpio& gpio) : gpio_(gpio) {}
  ~Ssd1306Spi() override;
  bool init(const std::string& spiDev, std::string& err);
  void show(const std::vector<std::string>& lines) override;

  // Exposed for unit tests: render text to the 1 KB page-format framebuffer.
  static void render(const std::vector<std::string>& lines, std::vector<uint8_t>& fb);

 private:
  bool cmd(const uint8_t* c, size_t n);
  bool data(const uint8_t* d, size_t n);
  Gpio& gpio_;
  int fd_ = -1;
  std::vector<uint8_t> last_;
};

}  // namespace guardeye
