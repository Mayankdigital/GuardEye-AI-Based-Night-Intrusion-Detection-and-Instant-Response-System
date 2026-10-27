#include "hal/hardware.h"

#include <unistd.h>

#include <chrono>
#include <cstring>

#include "log.h"
#include "util.h"

namespace guardeye {

bool Hardware::init(bool simulate, std::string& err) {
  const bool strict = (cfg_.mode == "real") && !simulate;
  sim_ = simulate || cfg_.mode == "sim";
  std::string notes;

  // ---- GPIO -------------------------------------------------------------
  if (!sim_) {
#ifdef HAVE_GPIOD
    auto g = std::make_unique<LibgpiodGpio>();
    std::string e;
    if (g->openChip(cfg_.gpioChip, e)) { gpio_ = std::move(g); }
    else if (strict) { err = e; return false; }
    else LOG_WARN << "GPIO: " << e << " -> using simulator";
#else
    if (strict) { err = "built without libgpiod"; return false; }
#endif
  }
  if (!gpio_) gpio_ = std::make_unique<SimGpio>();
  notes += std::string("gpio=") + gpio_->backend();

  // Outputs start OFF: the siren must never sound just because the app booted.
  for (const char* n : {"siren", "light", "led_status", "oled_dc", "oled_rst"}) {
    auto it = cfg_.pins.find(n);
    if (it != cfg_.pins.end() && it->second.line >= 0) {
      bool initial = (std::strcmp(n, "oled_rst") == 0);  // reset line idles high
      if (!gpio_->openOutput(n, it->second, initial)) {
        if (strict) { err = std::string("cannot open output ") + n; return false; }
        LOG_WARN << "GPIO: output '" << n << "' unavailable";
      }
    }
  }
  for (const char* n : {"reed", "button", "mains"}) {
    auto it = cfg_.pins.find(n);
    if (it != cfg_.pins.end() && it->second.line >= 0) {
      if (!gpio_->openInput(n, it->second)) {
        if (strict) { err = std::string("cannot open input ") + n; return false; }
        LOG_WARN << "GPIO: input '" << n << "' unavailable";
      }
    }
  }

  // ---- I2C: RTC + battery monitor ----------------------------------------
  bool needI2c = !sim_ && (cfg_.rtcEnabled || cfg_.inaEnabled);
  if (needI2c) {
    bus_ = std::make_unique<I2CBus>();
    std::string e;
    if (!bus_->open(cfg_.i2cDev, e)) {
      if (strict) { err = e; return false; }
      LOG_WARN << "I2C: " << e << " -> RTC/INA219 simulated";
      bus_.reset();
    }
  }
  if (cfg_.rtcEnabled) {
    if (bus_) {
      auto d = std::make_unique<Ds3231>(*bus_);
      if (d->probe()) rtc_ = std::move(d);
      else if (strict) { err = "DS3231 not found at 0x68 (check wiring, run i2cdetect -y 1)"; return false; }
      else LOG_WARN << "RTC: DS3231 not found at 0x68 -> using system clock";
    } else if (sim_) {
      rtc_ = std::make_unique<SystemRtc>();
    }
  }
  if (cfg_.inaEnabled) {
    if (bus_) {
      auto p = std::make_unique<Ina219>(*bus_, static_cast<uint8_t>(cfg_.inaAddr), cfg_.shuntOhm);
      if (p->init()) power_ = std::move(p);
      else if (strict) { err = "INA219 not found (check wiring/address)"; return false; }
      else LOG_WARN << "INA219 not found -> battery monitoring disabled";
    } else if (sim_) {
      auto sp = std::make_unique<SimPower>();
      simPower_ = sp.get();
      power_ = std::move(sp);
    }
  }
  notes += std::string(" rtc=") + (rtc_ ? rtc_->name() : "none");
  notes += std::string(" battery=") + (power_ ? power_->name() : "none");

  // ---- SPI OLED ----------------------------------------------------------
  if (cfg_.oledEnabled) {
    if (sim_) {
      display_ = std::make_unique<ConsoleDisplay>();
    } else if (gpio_->has("oled_dc")) {
      auto d = std::make_unique<Ssd1306Spi>(*gpio_);
      std::string e;
      if (d->init(cfg_.spiDev, e)) display_ = std::move(d);
      else if (strict) { err = "OLED: " + e; return false; }
      else LOG_WARN << "OLED: " << e << " -> display disabled";
    } else {
      LOG_WARN << "OLED enabled but pin 'oled_dc' not configured";
    }
  }
  notes += std::string(" oled=") + (display_ ? "yes" : "none");
  summary_ = notes;
  allOff();
  return true;
}

void Hardware::allOff() { setSiren(false); setLight(false); }
void Hardware::setSiren(bool on) { if (gpio_ && gpio_->has("siren")) gpio_->write("siren", on); }
void Hardware::setLight(bool on) { if (gpio_ && gpio_->has("light")) gpio_->write("light", on); }

std::time_t Hardware::wallClock() const { return std::time(nullptr) + rtcOffsetSec_.load(); }

int Hardware::minuteOfDay() const {
  std::tm tm{};
  std::time_t t = wallClock();
  localtime_r(&t, &tm);
  return tm.tm_hour * 60 + tm.tm_min;
}

bool Hardware::simSetDoor(bool open) {
  auto* s = dynamic_cast<SimGpio*>(gpio_.get());
  if (!s) return false;
  s->setInput("reed", open);
  return true;
}
bool Hardware::simSetMains(bool ok) {
  if (simPower_) simPower_->setMains(ok);
  auto* s = dynamic_cast<SimGpio*>(gpio_.get());
  if (s) s->setInput("mains", ok);
  return simPower_ != nullptr || s != nullptr;
}

void Hardware::start(HardwareCallbacks cb) {
  cb_ = std::move(cb);
  if (gpio_ && sim_ && gpio_->has("mains")) dynamic_cast<SimGpio*>(gpio_.get())->setInput("mains", true);
  running_ = true;
  th_ = std::thread(&Hardware::loop, this);
}

void Hardware::stop() {
  if (running_.exchange(false) && th_.joinable()) th_.join();
  if (gpio_) { allOff(); if (gpio_->has("led_status")) gpio_->write("led_status", false); }
}

void Hardware::loop() {
  Debounce door, button, mains;
  const bool haveReed = gpio_->has("reed"), haveBtn = gpio_->has("button"), haveMains = gpio_->has("mains");
  const double t0 = monoNow();
  // Seed with the real initial levels so boot does not look like an "event".
  if (haveReed)  { bool v = gpio_->read("reed");   door.stable = door.last = v; door.since = t0; }
  if (haveBtn)   { bool v = gpio_->read("button"); button.stable = button.last = v; button.since = t0; }
  if (haveMains) { bool v = gpio_->read("mains");  mains.stable = mains.last = v; mains.since = t0; }

  double lastSlow = 0, lastRtc = -1e9;
  bool ledState = false;
  int tick = 0;

  while (running_) {
    double now = monoNow();

    // Inputs, polled every 10 ms with software debounce. (Upgrade path: use
    // libgpiod edge events so this thread sleeps until something changes.)
    if (haveReed && door.update(gpio_->read("reed"), now, 0.05) && cb_.onDoor) cb_.onDoor(door.stable);
    if (haveBtn && button.update(gpio_->read("button"), now, 0.05) && button.stable && cb_.onButton) cb_.onButton();
    if (haveMains && mains.update(gpio_->read("mains"), now, 0.5) && cb_.onMains) cb_.onMains(mains.stable);

    // Status LED patterns (10 ms ticks).
    int mode = led_.load();
    if (gpio_->has("led_status")) {
      bool want = (mode == 1) ||
                  (mode == 2 && (tick % 100) < 50) ||   // 1 Hz
                  (mode == 3 && (tick % 20) < 10);      // 5 Hz
      if (want != ledState) { ledState = want; gpio_->write("led_status", want); }
    }

    // RTC -> offset against the system clock, every 10 s.
    if (rtc_ && now - lastRtc >= 10.0) {
      lastRtc = now;
      std::tm tm{};
      if (rtc_->read(tm)) {
        std::time_t rtcT = std::mktime(&tm);
        rtcOffsetSec_ = static_cast<long>(rtcT - std::time(nullptr));
        rtcOk_ = true;
      } else {
        rtcOk_ = false;
        rtcOffsetSec_ = 0;
      }
    }

    // 1 Hz: battery + display.
    if (now - lastSlow >= 1.0) {
      lastSlow = now;
      if (power_ && cb_.onPower) cb_.onPower(power_->read());
      if (display_ && cb_.displayLines) display_->show(cb_.displayLines());
    }

    ++tick;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace guardeye
