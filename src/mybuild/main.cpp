/*
 * Своя збірка: основа + модулі на вибір.
 *
 * 1. Зберіть основу: плата, тумблер ARM (до GND), статус-LED, за бажанням бузер.
 *    Залийте прошивку (pio run -e mybuild -t upload) — LED коротко блимає раз на
 *    2 с (SAFE), у Serial (pio device monitor) видно "ARM on/off" при перемиканні тумблера.
 * 2. Підключіть модуль, розкоментуйте його рядки нижче, вкажіть свої піни.
 *    Якщо два модулі хочуть один пін — у Serial буде "CONFIG ERROR" з поясненням,
 *    а пристрій не озброїться.
 * 3. Свій модуль — клас-нащадок fly::Module (приклад UptimeLog нижче).
 *    Для іншої плати змініть секцію [env:mybuild] у platformio.ini (extends = nano / esp32cam).
 *
 * Каталог модулів, роз'єми JST-XH і сумісність — docs/modules.md.
 */
#include <Arduino.h>
#include <FlyDefense.h>

fly::Base base(A0, 13);  // тумблер ARM, статус-LED

// --- Хлопавка (AVR) ---------------------------------------------------------
// fly::IrCurtain curtain(4, 5, 6, 7);             // ІЧ-завіса з 4 променів
// fly::ClapLatch clap(curtain, 9, 10, 8);         // соленоїд, серво, кінцевик

// --- Турель ------------------------------------------------------------------
// fly::PanTilt head(9, 10);                       // серви PAN, TILT
// fly::Nozzle gun(5, fly::Nozzle::PUMP);          // або fly::Nozzle::VALVE
// fly::Turret turret(head, gun);
//
// ...і одне джерело цілей для неї:
// fly::Sonar sonar(turret, 2, 3, 4);              // HC-SR04: TRIG, ECHO, кнопка RECAL
// fly::EyesLink eyes(turret);                     // кути від ESP32 по Serial (D0)

// --- Свій модуль ---------------------------------------------------------------
// Раз на хвилину друкує час роботи. Шаблон для власних сенсорів і виконавців:
// займіть піни в begin(), робіть швидку роботу в tick(), зупиняйтесь у disarm().
class UptimeLog : public fly::Module {
 public:
  const __FlashStringHelper *name() const override { return F("UptimeLog"); }

  void tick(fly::Base &base, uint32_t now) override {
    if (!fly::reached(now, next_)) return;
    next_ = now + 60000UL;
    base.log().print(F("Uptime, min: "));
    base.log().println(now / 60000UL);
  }

 private:
  uint32_t next_ = 0;
};

UptimeLog uptime;

void setup() {
  base.buzzer(11);  // пасивний бузер (необов'язково)
  base.add(uptime);
  // base.add(curtain); base.add(clap);
  // base.add(turret);  base.add(sonar);
  base.begin();
}

void loop() { base.update(); }
