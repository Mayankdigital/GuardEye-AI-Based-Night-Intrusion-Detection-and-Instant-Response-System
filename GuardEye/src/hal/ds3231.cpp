#include "hal/ds3231.h"

namespace guardeye {

bool Ds3231::probe() {
  uint8_t b;
  return bus_.readReg(addr_, 0x0F, &b, 1);
}

bool Ds3231::read(std::tm& out) {
  uint8_t r[7], status;
  if (!bus_.readReg(addr_, 0x00, r, 7)) return false;
  if (!bus_.readReg(addr_, 0x0F, &status, 1)) return false;
  if (status & 0x80) return false;  // OSF: oscillator stopped, time invalid

  std::tm t{};
  t.tm_sec = bcd2bin(r[0] & 0x7F);
  t.tm_min = bcd2bin(r[1] & 0x7F);
  if (r[2] & 0x40) {  // 12-hour mode
    int h = bcd2bin(r[2] & 0x1F);
    bool pm = r[2] & 0x20;
    t.tm_hour = (h % 12) + (pm ? 12 : 0);
  } else {
    t.tm_hour = bcd2bin(r[2] & 0x3F);
  }
  t.tm_mday = bcd2bin(r[4] & 0x3F);
  t.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
  t.tm_year = bcd2bin(r[6]) + 100;  // chip stores 00-99 => 2000-2099
  t.tm_isdst = -1;
  if (t.tm_year < 124) return false;  // before 2024: clearly never set
  out = t;
  return true;
}

bool Ds3231::set(const std::tm& in) {
  uint8_t r[7] = {bin2bcd(in.tm_sec), bin2bcd(in.tm_min), bin2bcd(in.tm_hour),
                  bin2bcd(in.tm_wday + 1), bin2bcd(in.tm_mday), bin2bcd(in.tm_mon + 1),
                  bin2bcd((in.tm_year + 1900) % 100)};
  if (!bus_.writeReg(addr_, 0x00, r, 7)) return false;
  uint8_t status;
  if (!bus_.readReg(addr_, 0x0F, &status, 1)) return false;
  status &= static_cast<uint8_t>(~0x80);  // clear OSF
  return bus_.writeReg(addr_, 0x0F, &status, 1);
}

}  // namespace guardeye
