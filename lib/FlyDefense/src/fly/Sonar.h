/*
 * Сонар HC-SR04 на голові турелі (сенсор FlySonar). Сам водить голову й шукає ціль;
 * стріляє через Turret, тож діють ті самі правила безпеки, що й для камери.
 *
 *   1) CALIBRATE: обхід сітки кутів, запам'ятовуємо "фон" (медіана з 5 вимірів).
 *   2) SCAN: змійкою по сітці; ехо значно ближче за фон — кандидат.
 *   3) CONFIRM: медіана з 3 + ширина об'єкта (широке ехо — рука/кіт, НЕ стріляємо).
 *   4) REFINE: дрібний скан ±10° кроком 2°, ціль = центроїд кутів, де є ехо.
 *      Точність ~1–2° замість кроку сітки 5°.
 *   5) STILL: кілька вимірів у центроїді; за замовчуванням стріляємо лише по
 *      цілі, що сидить (staticOnly) — по мусі в польоті сонар не влучить.
 *   6) FIRE: поправка на сопло й балістику, постріл, перевірка, до 3 пострілів.
 *
 * Кнопка перекалібрування фону (до GND) — необов'язкова.
 */
#pragma once
#include "Module.h"
#include "Turret.h"

namespace fly {

class Sonar : public Module {
 public:
  // Геометрія сканування (градуси)
  static const uint8_t PAN_MIN = 30, PAN_MAX = 150;
  static const uint8_t PAN_STEP = 5;  // ~ ширина пелюстки HC-SR04 / 3
  static const uint8_t PAN_CELLS = (PAN_MAX - PAN_MIN) / PAN_STEP + 1;  // 25
  static const uint8_t TILT_CELLS = 3;
  static const uint8_t TILT_LEVELS[TILT_CELLS];  // 80, 95, 110

  static const uint16_t ECHO_TIMEOUT_US = 6000;    // ~1 м — далі нас не цікавить
  static const uint16_t PING_GAP_MS = 30;          // пауза між пінгами, щоб не ловити старе ехо
  static const uint16_t ECHO_IDLE_TIMEOUT_MS = 50; // без ехо HC-SR04 тримає ECHO у HIGH ~38 мс
  static const uint16_t NO_ECHO = 999;
  static const uint8_t MIN_CM = 8;       // ближче — мертва зона HC-SR04 / сам корпус
  static const uint8_t MAX_CM = 60;      // далі муху вже не видно в шумі
  static const uint8_t DELTA_CM = 10;    // на скільки ближче за фон має бути ціль
  static const uint8_t SIMILAR_CM = 6;   // "та сама відстань" для перевірки ширини
  // Пелюстка ~16° накриває до 4 клітинок сітки 5°, тож навіть точкова муха дає
  // ехо в сусідах. Великий об'єкт (рука, кіт) — суцільна смуга з 5+ клітинок.
  static const int8_t WIDE_RANGE = 6;
  static const uint8_t WIDE_SPAN = 5;
  static const uint16_t FRESH_MS = 400;  // вимір сусідньої клітинки, свіжіший за це, не повторюємо

  static const int16_t FINE_SPAN10 = 100;  // дрібний скан ±10°
  static const int16_t FINE_STEP10 = 20;   // кроком 2°
  static const uint8_t FINE_MIN_HITS = 2;
  static const uint8_t FINE_PASSES = 3;
  static const uint8_t STILL_SAMPLES = 4;
  static const uint8_t STILL_CM = 2;

  static const int16_t PAN_NOZZLE_OFFSET10 = 0;    // сопло збоку від сонара — поправка, 0.1°
  static const int16_t TILT_NOZZLE_OFFSET10 = 20;  // сопло під сонаром — трохи вгору, 0.1°
  static const int8_t TILT_UP_SIGN = +1;           // +1, якщо більший кут серви = вгору
  static const uint8_t MAX_SHOTS = 3;
  static const uint16_t COOLDOWN_MS = 2500;

  bool staticOnly = true;  // стріляти лише по нерухомій цілі

  Sonar(Turret &turret, uint8_t trigPin, uint8_t echoPin, uint8_t recalPin = 255);

  const __FlashStringHelper *name() const override { return F("Sonar"); }
  bool begin(Base &base) override;
  void loop(Base &base) override;
  Health health() const override { return calibrating_ ? Health::Busy : Health::Ok; }

  // Для тестів і діагностики
  void calibrate(Base &base);
  void holdAllRows(uint32_t until);

 private:
  uint16_t pingCm(Base &b);
  uint16_t pingMedian3(Base &b);
  void moveTo(Base &b, int pan10, int tilt10);
  uint16_t measureCell(Base &b, uint8_t t, uint8_t p);
  uint8_t objectSpan(Base &b, uint8_t t, uint8_t p, uint16_t d);
  bool fineAxis(Base &b, bool panAxis, int16_t center10, int16_t other10, uint16_t d, int16_t &shift);
  bool refineCentroid(Base &b, uint8_t t, uint8_t p, uint16_t d, int16_t &pan10, int16_t &tilt10);
  bool isStill(Base &b, int16_t pan10, int16_t tilt10, uint16_t &d);
  bool fireAt(Base &b, int16_t pan10, int16_t tilt10);
  bool engage(Base &b, uint8_t t, uint8_t p);

  Turret &turret_;
  uint8_t trigPin_, echoPin_, recalPin_;
  bool calibrated_ = false, calibrating_ = false;
  uint32_t lastPing_ = 0;
  uint8_t t_ = 0, p_ = 0;
  int8_t dir_ = +1;  // "змійка": без великих перекидів серви

  uint16_t background_[TILT_CELLS][PAN_CELLS];
  uint16_t lastScan_[TILT_CELLS][PAN_CELLS];    // останній вимір у кожній клітинці...
  uint32_t lastScanAt_[TILT_CELLS][PAN_CELLS];  // ...і коли його зроблено
  uint32_t cooldownUntil_[TILT_CELLS];          // кулдаун по рядах (дешево по RAM)
};

}  // namespace fly
