/*
 * Приймач команд від "очей" (ESP32 з CameraEyes + LinkOut) по UART → турель.
 * Типово слухає Serial: RX (D0 на Uno/Nano) — від ESP32 GPIO14, TX лишається логом у USB.
 * На час прошивки Arduino провід на D0 від'єднувати!
 *
 * Немає команд довше LINK_TIMEOUT_MS — дозвіл на постріл скасовується, LED швидко блимає.
 */
#pragma once
#include "AimTarget.h"
#include "Module.h"
#include "Protocol.h"

namespace fly {

class EyesLink : public Module {
 public:
  static const uint16_t LINK_TIMEOUT_MS = 500;

  explicit EyesLink(AimTarget &target, Stream &in = Serial) : target_(target), in_(in) {}

  const __FlashStringHelper *name() const override { return F("EyesLink"); }
  bool begin(Base &base) override;
  void tick(Base &base, uint32_t now) override;
  Health health() const override { return linkOk_ ? Health::Ok : Health::Fault; }

  bool linkOk() const { return linkOk_; }

 private:
  void handle(const Command &c);

  AimTarget &target_;
  Stream &in_;
  LineReader reader_;
  uint32_t lastLinkAt_ = 0;
  bool linkOk_ = false;
};

}  // namespace fly
