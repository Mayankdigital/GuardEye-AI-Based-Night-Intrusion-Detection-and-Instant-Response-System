// GPIO abstraction. Pins have NAMES ("siren", "reed"...) and LOGICAL values:
// write("siren", true) always means "siren ON", even if the relay board is
// active-low. Backends: libgpiod v1 (real) and a console simulator.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "config.h"

#ifdef HAVE_GPIOD
struct gpiod_chip;
struct gpiod_line;
#endif

namespace guardeye {

class Gpio {
 public:
  virtual ~Gpio() = default;
  virtual bool openOutput(const std::string& name, const PinCfg& cfg, bool initialOn) = 0;
  virtual bool openInput(const std::string& name, const PinCfg& cfg) = 0;
  virtual void write(const std::string& name, bool on) = 0;
  virtual bool read(const std::string& name) = 0;
  virtual bool has(const std::string& name) const = 0;
  virtual const char* backend() const = 0;
};

class SimGpio : public Gpio {
 public:
  bool openOutput(const std::string& name, const PinCfg& cfg, bool initialOn) override;
  bool openInput(const std::string& name, const PinCfg& cfg) override;
  void write(const std::string& name, bool on) override;
  bool read(const std::string& name) override;
  bool has(const std::string& name) const override;
  const char* backend() const override { return "simulated"; }
  void setInput(const std::string& name, bool logical);  // test / web-UI hook
 private:
  mutable std::mutex m_;
  std::map<std::string, bool> out_, in_;
};

#ifdef HAVE_GPIOD
class LibgpiodGpio : public Gpio {
 public:
  ~LibgpiodGpio() override;
  bool openChip(const std::string& chipName, std::string& err);
  bool openOutput(const std::string& name, const PinCfg& cfg, bool initialOn) override;
  bool openInput(const std::string& name, const PinCfg& cfg) override;
  void write(const std::string& name, bool on) override;
  bool read(const std::string& name) override;
  bool has(const std::string& name) const override;
  const char* backend() const override { return "libgpiod"; }
 private:
  struct Pin { gpiod_line* line = nullptr; PinCfg cfg; };
  mutable std::mutex m_;
  gpiod_chip* chip_ = nullptr;
  std::map<std::string, Pin> pins_;
};
#endif

}  // namespace guardeye
