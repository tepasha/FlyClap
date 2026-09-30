/*
 * Поворотна голова з двох серв (pan — горизонталь, tilt — вертикаль). Кути в
 * десятих градуса (0..1800); команда повертає, скільки мс серво їхатиме.
 * Використовується Turret; сама по собі не модуль.
 */
#pragma once
#include "ServoOut.h"

namespace fly {

class PanTilt {
 public:
  uint16_t settleBaseMs = 12;   // мінімальна пауза після команди
  uint8_t settleMsPerDeg = 2;   // MG90S: ~0.1 с/60° + запас
  uint16_t settleMaxMs = 300;

  PanTilt(uint8_t panPin, uint8_t tiltPin) : pan_(panPin), tilt_(tiltPin) {}

  bool begin(Base &base, const __FlashStringHelper *owner) {
    return pan_.begin(base, owner) && tilt_.begin(base, owner);
  }

  // Навести без очікування. Повертає паузу до зупинки серв, мс (0 — кут той самий).
  uint16_t aim(int pan10, int tilt10) {
    const int16_t p = clamp10(pan10), t = clamp10(tilt10);
    const bool first = !pan_.attached();
    if (!first && p == pan10_ && t == tilt10_) return 0;
    const int16_t dp = p > pan10_ ? p - pan10_ : pan10_ - p;
    const int16_t dt = t > tilt10_ ? t - tilt10_ : tilt10_ - t;
    pan_.writeUs(ServoOut::deg10ToUs(p));
    tilt_.writeUs(ServoOut::deg10ToUs(t));
    pan10_ = p;
    tilt10_ = t;
    const uint32_t ms = settleBaseMs + (uint32_t)(dp > dt ? dp : dt) * settleMsPerDeg / 10;
    return first ? settleMaxMs : (ms > settleMaxMs ? settleMaxMs : ms);
  }

  int16_t pan10() const { return pan10_; }
  int16_t tilt10() const { return tilt10_; }
  uint16_t panUs() const { return pan_.us(); }
  uint16_t tiltUs() const { return tilt_.us(); }

  static int16_t clamp10(int v) { return (int16_t)(v < 0 ? 0 : v > 1800 ? 1800 : v); }

 private:
  ServoOut pan_, tilt_;
  int16_t pan10_ = 900, tilt10_ = 900;
};

}  // namespace fly
