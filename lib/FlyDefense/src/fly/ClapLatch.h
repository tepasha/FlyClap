/*
 * Засувка-хлопавка (виконавець FlyClap).
 *
 * Дві пружинні пластини тримає засувка. Тригер (IrCurtain) з переривання подає
 * імпульс на соленоїд → засувка відкривається → пластини схлопуються (~15–25 мс)
 * → серво знову розводить пластини до зачеплення засувки (кінцевик "взведено").
 *
 * Стани: DISARMED → (ARM) → COCKING → SERVO_RETURN → WAIT_CLEAR → ARMED →
 *        (муха) → FIRED → COCKING … ; FAULT — вихід лише вимкненням ARM.
 * Лічильник спрацювань (а не підтверджених мух — пил теж рахується) — в EEPROM.
 */
#pragma once
#include "Module.h"
#include "ServoOut.h"
#include "Trigger.h"

namespace fly {

class ClapLatch : public Module {
 public:
  static const uint16_t SOLENOID_PULSE_MS = 40;  // час утримання засувки відкритою
  static const uint16_t CLAP_SETTLE_MS = 300;    // чекаємо, поки пластини схлопнуться
  static const uint8_t SERVO_REST_DEG = 10;      // важіль відведений, не заважає пластинам
  static const uint8_t SERVO_COCK_DEG = 150;     // важіль розводить пластини до зачеплення
  static const uint16_t COCK_TIMEOUT_MS = 1500;  // не дочекались кінцевика → FAULT
  static const uint16_t SERVO_RETURN_MS = 400;   // час на відведення важеля; потім серво відключаємо
  static const uint16_t BEAMS_CLEAR_MS = 500;    // завіса має бути чистою стільки перед ARMED
  static const uint16_t BEAMS_STUCK_MS = 3000;   // довше перекрито (прилипла муха/сміття) → FAULT

  enum State : uint8_t { DISARMED, WAIT_CLEAR, ARMED, FIRED, COCKING, SERVO_RETURN, FAULT };

  // solenoidPin — затвор MOSFET соленоїда; cockedPin — кінцевик до GND (LOW = взведено).
  ClapLatch(Trigger &trigger, uint8_t solenoidPin, uint8_t servoPin, uint8_t cockedPin, int eepromAddr = 0);

  const __FlashStringHelper *name() const override { return F("ClapLatch"); }
  bool begin(Base &base) override;
  void tick(Base &base, uint32_t now) override;
  void disarm(Base &base) override;
  Health health() const override;

  State state() const { return state_; }
  uint32_t claps() const { return claps_; }

 private:
  static void onCross(void *self);  // з переривання тригера
  void enter(State s, uint32_t now);
  void servoTo(uint8_t deg, uint32_t now);
  void fault(Base &base, const __FlashStringHelper *why, uint32_t now);
  void solenoid(bool on);
  bool cocked() const { return digitalRead(cockedPin_) == LOW; }

  Trigger &trigger_;
  uint8_t solenoidPin_, cockedPin_;
  int eepromAddr_;
  ServoOut servo_;
  State state_ = DISARMED;
  uint32_t stateSince_ = 0, clearSince_ = 0, blockedSince_ = 0, servoMovedAt_ = 0;
  bool blocked_ = false;
  uint8_t servoTarget_ = SERVO_REST_DEG;
  uint32_t claps_ = 0;
  volatile bool fired_ = false;
  volatile uint32_t fireMillis_ = 0;
#if defined(__AVR__)
  volatile uint8_t *solPort_ = nullptr;
  uint8_t solMask_ = 0;
#endif
};

}  // namespace fly
