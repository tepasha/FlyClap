/*
 * Модуль — будь-яка частина пристрою, яку підключають до основи (fly::Base):
 * сенсор, виконавець, зв'язок. Основа викликає методи модуля в такому порядку:
 *
 *   begin()   один раз на старті: зайняти піни (base.claimPin), налаштувати залізо.
 *             false — конфігурація неможлива, пристрій не озброїться.
 *   tick()    часто (~1 мс) і швидко, без блокувань: таймінги пострілів, датчики.
 *             На ESP32 — з окремої задачі реального часу.
 *   loop()    основна робота з головного циклу. Може блокувати, але лише через
 *             base.wait(), щоб tick() інших модулів не зупинявся.
 *   disarm()  тумблер ARM вимкнули: негайно зупинити все, що стріляє чи рухається.
 *   health()  стан для статус-LED: Ok, Busy (зайнятий) або Fault (аварія).
 *
 * Свій модуль — це клас-нащадок fly::Module; див. src/mybuild і docs/modules.md.
 */
#pragma once
#include <Arduino.h>

namespace fly {

class Base;

enum class Health : uint8_t { Ok, Busy, Fault };

class Module {
 public:
  virtual const __FlashStringHelper *name() const = 0;
  virtual bool begin(Base &) { return true; }
  virtual void tick(Base &, uint32_t /*now*/) {}
  virtual void loop(Base &) {}
  virtual void disarm(Base &) {}
  virtual Health health() const { return Health::Ok; }

 private:
  friend class Base;
  Module *next_ = nullptr;  // список модулів без динамічної пам'яті
};

// Момент t настав; стійко до переповнення millis() через ~49.7 доби.
inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

}  // namespace fly
