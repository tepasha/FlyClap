/*
 * Основа пристрою: те, що потрібне будь-якій збірці.
 *
 *   - тумблер ARM (до GND, LOW = стріляти дозволено) — єдиний дозвіл на постріл
 *     для всіх модулів; вимкнення миттєво зупиняє все (Module::disarm);
 *   - статус-LED однаковий для всіх пристроїв:
 *       коротке мигання раз на 2 с — SAFE (ARM вимкнено), пристрій живий
 *       горить                     — ARM, усе готово
 *       повільно блимає            — ARM, модуль зайнятий (взводиться, калібрується…)
 *       швидко блимає              — аварія, помилка конфігурації або немає зв'язку
 *   - бузер (необов'язковий): пасивний (тон) або активний (просто вмикається);
 *   - лог у Serial (115200);
 *   - реєстр пінів і таймерів: два модулі на одному піні — зрозуміла помилка
 *     "CONFIG ERROR" у Serial на старті, а не дивна поведінка.
 *
 * Використання:
 *   fly::Base base(A0, 13);          // ARM на A0, LED на D13
 *   base.add(someModule);            // у setup(), до begin()
 *   base.begin();
 *   ...
 *   void loop() { base.update(); }
 */
#pragma once
#include <Arduino.h>
#include "Module.h"

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#ifndef FLY_MAX_PINS
#ifdef NUM_DIGITAL_PINS
#define FLY_MAX_PINS NUM_DIGITAL_PINS
#else
#define FLY_MAX_PINS 50
#endif
#endif

namespace fly {

// Спільні апаратні ресурси, крім пінів
enum class Res : uint8_t {
  Timer2,       // AVR: tone() пасивного бузера або несуча 38 кГц завіси TSSP
  SerialRx,     // прийом команд по Serial (EyesLink)
  LedcCamera,   // ESP32: LEDC timer 0 — тактування камери
  Count
};

class Base {
 public:
  static const uint8_t NO_PIN = 255;

  Base(uint8_t armPin, uint8_t ledPin, bool ledActiveLow = false);

  // Бузер: active = true — активний (пищить сам від 5 В), інакше пасивний (тон).
  void buzzer(uint8_t pin, bool active = false);
  void add(Module &m);
  // false — помилка конфігурації (деталі в Serial); пристрій лишається в SAFE.
  bool begin(unsigned long baud = 115200);
  // Викликати з loop().
  void update();
  // Блокуюча пауза, під час якої tick() модулів продовжує працювати.
  void wait(uint32_t ms);
  // Один крок tick() усіх модулів — для коротких активних очікувань.
  void idle();

  bool armed() const { return armed_; }
  bool configOk() const { return !configError_; }
  void beep(uint16_t hz, uint16_t ms);
  Print &log() { return Serial; }

  // Зайняти пін / ресурс. false — уже зайнято (помилку вже надруковано).
  bool claimPin(uint8_t pin, const __FlashStringHelper *who);
  bool claim(Res r, const __FlashStringHelper *who);
  // Помилка конфігурації: друкує причину й блокує ARM назавжди (до перезавантаження).
  void configError(const __FlashStringHelper *who, const __FlashStringHelper *what);

  // Захист стану, спільного між loop() і tick(). На ESP32 tick() іде з окремої
  // задачі, тож модулі беруть Lock у методах, які викликаються з loop().
  // На AVR усе в одному потоці — Lock нічого не робить.
  class Lock {
   public:
    explicit Lock(Base &b);
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

   private:
    Base &b_;
  };

  // Крок реального часу для всіх модулів. Зазвичай його викликає сама основа.
  void tick(uint32_t now);

 private:
  void updateLed(uint32_t now, Health worst);

  uint8_t armPin_, ledPin_, buzzerPin_ = NO_PIN;
  bool ledActiveLow_, buzzerActive_ = false, buzzing_ = false;
  bool armed_ = false, configError_ = false, ledOn_ = false, started_ = false;
  uint32_t buzzerOffAt_ = 0;
  Module *first_ = nullptr;
  const __FlashStringHelper *pinOwner_[FLY_MAX_PINS] = {};
  const __FlashStringHelper *resOwner_[(uint8_t)Res::Count] = {};
#if defined(ESP32)
  SemaphoreHandle_t mutex_ = nullptr;
  static void realtimeTask(void *self);
#endif
};

}  // namespace fly
