#include "hal/gpio.h"

#include "log.h"

#ifdef HAVE_GPIOD
#include <gpiod.h>
#endif

namespace guardeye {

// ---------------------------------------------------------------- simulator
bool SimGpio::openOutput(const std::string& name, const PinCfg&, bool initialOn) {
  std::lock_guard<std::mutex> lk(m_);
  out_[name] = initialOn;
  return true;
}
bool SimGpio::openInput(const std::string& name, const PinCfg&) {
  std::lock_guard<std::mutex> lk(m_);
  in_[name] = false;
  return true;
}
void SimGpio::write(const std::string& name, bool on) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = out_.find(name);
  if (it == out_.end()) return;
  if (it->second != on) {
    it->second = on;
    if (name != "led_status") LOG_INFO << "[sim-gpio] " << name << " -> " << (on ? "ON" : "OFF");
  }
}
bool SimGpio::read(const std::string& name) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = in_.find(name);
  return it != in_.end() && it->second;
}
bool SimGpio::has(const std::string& name) const {
  std::lock_guard<std::mutex> lk(m_);
  return out_.count(name) || in_.count(name);
}
void SimGpio::setInput(const std::string& name, bool logical) {
  std::lock_guard<std::mutex> lk(m_);
  if (in_.count(name)) in_[name] = logical;
}

// ---------------------------------------------------------------- libgpiod
#ifdef HAVE_GPIOD
LibgpiodGpio::~LibgpiodGpio() {
  for (auto& kv : pins_) if (kv.second.line) gpiod_line_release(kv.second.line);
  if (chip_) gpiod_chip_close(chip_);
}

bool LibgpiodGpio::openChip(const std::string& chipName, std::string& err) {
  chip_ = gpiod_chip_open_by_name(chipName.c_str());
  if (!chip_) { err = "cannot open /dev/" + chipName + " (permissions? add user to 'gpio' group)"; return false; }
  return true;
}

bool LibgpiodGpio::openOutput(const std::string& name, const PinCfg& cfg, bool initialOn) {
  std::lock_guard<std::mutex> lk(m_);
  if (!chip_ || cfg.line < 0) return false;
  gpiod_line* l = gpiod_chip_get_line(chip_, cfg.line);
  if (!l) return false;
  int raw = (initialOn != cfg.activeLow) ? 1 : 0;  // active-low: ON == electrical LOW
  if (gpiod_line_request_output(l, "guardeye", raw) < 0) {
    LOG_ERROR << "gpio: cannot request output '" << name << "' line " << cfg.line;
    return false;
  }
  pins_[name] = {l, cfg};
  return true;
}

bool LibgpiodGpio::openInput(const std::string& name, const PinCfg& cfg) {
  std::lock_guard<std::mutex> lk(m_);
  if (!chip_ || cfg.line < 0) return false;
  gpiod_line* l = gpiod_chip_get_line(chip_, cfg.line);
  if (!l) return false;
  int flags = cfg.pullUp ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP : 0;  // needs libgpiod >= 1.5
  if (gpiod_line_request_input_flags(l, "guardeye", flags) < 0) {
    LOG_ERROR << "gpio: cannot request input '" << name << "' line " << cfg.line;
    return false;
  }
  pins_[name] = {l, cfg};
  return true;
}

void LibgpiodGpio::write(const std::string& name, bool on) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = pins_.find(name);
  if (it == pins_.end()) return;
  gpiod_line_set_value(it->second.line, (on != it->second.cfg.activeLow) ? 1 : 0);
}

bool LibgpiodGpio::read(const std::string& name) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = pins_.find(name);
  if (it == pins_.end()) return false;
  int raw = gpiod_line_get_value(it->second.line);
  if (raw < 0) return false;
  return (raw == 1) != it->second.cfg.activeLow;
}

bool LibgpiodGpio::has(const std::string& name) const {
  std::lock_guard<std::mutex> lk(m_);
  return pins_.count(name) > 0;
}
#endif

}  // namespace guardeye
