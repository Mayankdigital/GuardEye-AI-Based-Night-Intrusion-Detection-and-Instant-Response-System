// Owns every physical device (GPIO, I2C RTC + battery monitor, SPI OLED) and
// the IO thread that polls inputs, blinks the LED and refreshes the display.
#pragma once
#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "hal/ds3231.h"
#include "hal/gpio.h"
#include "hal/i2c_bus.h"
#include "hal/ina219.h"
#include "hal/ssd1306.h"

namespace guardeye {

enum class LedMode { Off = 0, On = 1, SlowBlink = 2, FastBlink = 3 };

struct HardwareCallbacks {
  std::function<void(bool open)> onDoor;                 // reed switch (debounced)
  std::function<void()> onButton;                        // push button pressed
  std::function<void(bool mainsOk)> onMains;             // mains-present input
  std::function<void(const PowerReading&)> onPower;      // every second
  std::function<std::vector<std::string>()> displayLines;  // polled for the OLED
};

class Hardware {
 public:
  explicit Hardware(const HardwareCfg& cfg) : cfg_(cfg) {}
  ~Hardware() { stop(); }

  // simulate=true forces simulators. Otherwise real devices are used, and in
  // mode "auto" any missing device silently falls back to its simulator.
  bool init(bool simulate, std::string& err);
  void start(HardwareCallbacks cb);
  void stop();

  // Outputs (logical values, safe no-ops if the pin is not configured).
  void setSiren(bool on);
  void setLight(bool on);
  void setLed(LedMode m) { led_ = static_cast<int>(m); }
  void allOff();

  // Wall-clock time: system clock corrected by the RTC offset (if present).
  std::time_t wallClock() const;
  int minuteOfDay() const;
  bool rtcPresent() const { return rtcOk_; }

  bool simulated() const { return sim_; }
  std::string summary() const { return summary_; }

  // Simulation hooks (used by the web UI in --sim mode).
  bool simSetDoor(bool open);
  bool simSetMains(bool ok);

 private:
  void loop();
  struct Debounce {
    bool stable = false, last = false;
    double since = 0;
    bool update(bool sample, double now, double holdSec) {  // true when stable value changed
      if (sample != last) { last = sample; since = now; }
      if (last != stable && now - since >= holdSec) { stable = last; return true; }
      return false;
    }
  };

  HardwareCfg cfg_;
  HardwareCallbacks cb_;
  bool sim_ = false;
  std::string summary_;

  std::unique_ptr<Gpio> gpio_;
  std::unique_ptr<I2CBus> bus_;
  std::unique_ptr<Rtc> rtc_;
  std::unique_ptr<PowerMonitor> power_;
  std::unique_ptr<Display> display_;
  SimPower* simPower_ = nullptr;

  std::atomic<bool> rtcOk_{false};
  std::atomic<long> rtcOffsetSec_{0};
  std::atomic<int> led_{0};
  std::atomic<bool> running_{false};
  std::thread th_;
};

}  // namespace guardeye
