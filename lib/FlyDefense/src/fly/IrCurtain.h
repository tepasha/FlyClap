/*
 * ІЧ-завіса з 4 променів (тригер для FlyClap). Промінь цілий = LOW, перекритий = HIGH.
 *
 *   COMPARATOR — фототранзистори + LM339 з гістерезисом (підтяжки на платі
 *                компаратора), або готові break-beam модулі з відкритим колектором.
 *   TSSP       — модульовані приймачі TSSP4038: основа генерує несучу 38 кГц на D3
 *                (Timer2), LM339 не потрібен, завіса не боїться сонця. Timer2 зайнятий,
 *                тому бузер має бути АКТИВНИМ (base.buzzer(pin, true)).
 *
 * Плати: AVR (Uno, Nano). Входи — будь-які піни з pin-change перериванням
 * (на Uno/Nano — усі цифрові й аналогові). Не поєднується з SoftwareSerial
 * (обидва використовують переривання PCINT).
 */
#pragma once
#include "Trigger.h"

namespace fly {

class IrCurtain : public Trigger {
 public:
  enum Receiver : uint8_t { COMPARATOR, TSSP };
  static const uint8_t BEAMS = 4;
  static const uint8_t CONFIRM_US = 30;  // повторне читання в ISR проти імпульсних завад
  static const uint8_t TSSP_CARRIER_PIN = 3;  // OC2B на Uno/Nano

  IrCurtain(uint8_t beam1, uint8_t beam2, uint8_t beam3, uint8_t beam4, Receiver rx = COMPARATOR);

  const __FlashStringHelper *name() const override { return F("IrCurtain"); }
  bool begin(Base &base) override;
  bool blocked() const override;

  static void isr();  // для векторів переривань; не викликати вручну

 private:
  uint8_t pins_[BEAMS];
  Receiver rx_;
#if defined(__AVR__)
  volatile uint8_t *in_[BEAMS] = {};
  uint8_t mask_[BEAMS] = {};
#endif
  static IrCurtain *instance_;
};

}  // namespace fly
