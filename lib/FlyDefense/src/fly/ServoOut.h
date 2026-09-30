/*
 * Серво для будь-якої плати: на AVR — стандартна бібліотека Servo, на ESP32 —
 * драйвер LEDC на окремому таймері 2 (таймер 0 / канал 0 тактують камеру; ledcAttach()
 * з arduino-esp32 про це не знає й може зупинити камеру).
 *
 * Серво "підключається" першим записом кута (спершу кут, потім імпульси — не смикне)
 * і може бути відключене: у спокої не тремтить, не гуде й не наводить завади.
 */
#pragma once
#include <Arduino.h>
#if !defined(ESP32)
#include <Servo.h>
#endif

namespace fly {

class Base;

class ServoOut {
 public:
  static const uint16_t US_MIN = 544;   // 0°, як у Servo::write()
  static const uint16_t US_MAX = 2400;  // 180°

  explicit ServoOut(uint8_t pin) : pin_(pin) {}

  bool begin(Base &base, const __FlashStringHelper *owner);
  void writeUs(uint16_t us);
  void writeDeg(uint8_t deg) { writeUs(deg10ToUs(deg * 10)); }
  void detach();

  bool attached() const { return attached_; }
  uint16_t us() const { return us_; }
  uint8_t pin() const { return pin_; }

  // Кут у десятих градуса (0..1800) → ширина імпульсу, мкс
  static uint16_t deg10ToUs(int16_t d10) {
    return US_MIN + (int32_t)d10 * (US_MAX - US_MIN) / 1800;
  }

 private:
  uint8_t pin_;
  uint16_t us_ = 1472;
  bool attached_ = false;
#if defined(ESP32)
  int8_t channel_ = -1;
#else
  Servo servo_;
#endif
};

}  // namespace fly
