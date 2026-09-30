#include "IrCurtain.h"
#include "Base.h"

namespace fly {

IrCurtain *IrCurtain::instance_ = nullptr;

IrCurtain::IrCurtain(uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, Receiver rx) : pins_{b1, b2, b3, b4}, rx_(rx) {}

#if defined(__AVR__)

bool IrCurtain::begin(Base &base) {
  if (instance_) {
    base.configError(name(), F("only one IrCurtain is supported"));
    return false;
  }
  for (uint8_t i = 0; i < BEAMS; i++) {
    const uint8_t p = pins_[i];
    if (!base.claimPin(p, name())) return false;
    if (!digitalPinToPCICR(p)) {
      base.configError(name(), F("beam pin has no pin-change interrupt"));
      return false;
    }
    // TSSP має слабку власну підтяжку — додаємо внутрішню; компаратор має свої
    pinMode(p, rx_ == TSSP ? INPUT_PULLUP : INPUT);
    in_[i] = portInputRegister(digitalPinToPort(p));
    mask_[i] = digitalPinToBitMask(p);
  }

  if (rx_ == TSSP) {
#if defined(__AVR_ATmega328P__)
    if (!base.claim(Res::Timer2, F("IrCurtain TSSP carrier")) || !base.claimPin(TSSP_CARRIER_PIN, name())) return false;
    // Timer2, Fast PWM з TOP = OCR2A: 16 МГц / 8 / (52 + 1) ≈ 37.7 кГц, ~50 % на OC2B (D3)
    pinMode(TSSP_CARRIER_PIN, OUTPUT);
    TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);
    TCCR2B = _BV(WGM22) | _BV(CS21);
    OCR2A = 52;
    OCR2B = 26;
    delay(50);  // АРУ приймачів встановлюється на несучу
#else
    base.configError(name(), F("TSSP carrier needs ATmega328P (Uno/Nano)"));
    return false;
#endif
  }

  instance_ = this;
  for (uint8_t i = 0; i < BEAMS; i++) {
    const uint8_t p = pins_[i];
    *digitalPinToPCMSK(p) |= _BV(digitalPinToPCMSKbit(p));
    PCICR |= _BV(digitalPinToPCICRbit(p));
  }
  return true;
}

bool IrCurtain::blocked() const {
  for (uint8_t i = 0; i < BEAMS; i++)
    if (*in_[i] & mask_[i]) return true;
  return false;
}

void IrCurtain::isr() {
  IrCurtain *c = instance_;
  if (!c || !c->shotEnabled_) return;
  if (!c->blocked()) return;  // це був фронт відновлення променя
  delayMicroseconds(CONFIRM_US);
  if (!c->blocked()) return;
  c->crossedFromIsr();
}

}  // namespace fly

#if defined(PCINT0_vect)
ISR(PCINT0_vect) { fly::IrCurtain::isr(); }
#endif
#if defined(PCINT1_vect)
ISR(PCINT1_vect) { fly::IrCurtain::isr(); }
#endif
#if defined(PCINT2_vect)
ISR(PCINT2_vect) { fly::IrCurtain::isr(); }
#endif

#else  // не AVR

bool IrCurtain::begin(Base &base) {
  base.configError(name(), F("supported on AVR boards (Uno, Nano) only"));
  return false;
}

bool IrCurtain::blocked() const {
  for (uint8_t i = 0; i < BEAMS; i++)
    if (digitalRead(pins_[i]) == HIGH) return true;
  return false;
}

void IrCurtain::isr() {}

}  // namespace fly

#endif
