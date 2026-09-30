/*
 * FlySonar — турель-водомет із сонаром (Arduino Uno / Nano).
 *
 * Сонар HC-SR04 на pan-tilt голові разом із соплом: калібрує фон, сканує зону
 * змійкою, уточнює напрям на ціль і стріляє лише по нерухомій дрібній цілі.
 * Руки й тварин (широке ехо) не чіпає. Опис — docs/flysonar.md.
 *
 * Модулі:  основа + Turret (PanTilt + Nozzle) + Sonar (сенсор)
 * Збірка:  pio run -e flysonar -t upload   (flysonar_nano — Nano)
 *
 * Пінаут:
 *   D2  — HC-SR04 TRIG        D9  — серво PAN (горизонталь)
 *   D3  — HC-SR04 ECHO        D10 — серво TILT (вертикаль)
 *   D4  — кнопка RECAL        D13 — статус-LED
 *   D5  — MOSFET помпи/клапана A0 — тумблер ARM (до GND)
 */
#include <Arduino.h>
#include <FlyDefense.h>

#ifndef SHOOTER_VALVE
#define SHOOTER_VALVE 0  // 0 — помпа R385, 1 — клапан 12 В + бак під тиском
#endif
#ifndef STATIC_ONLY
#define STATIC_ONLY 1    // стріляти лише по нерухомій цілі
#endif

fly::Base base(A0, 13);
fly::PanTilt head(9, 10);
fly::Nozzle gun(5, SHOOTER_VALVE ? fly::Nozzle::VALVE : fly::Nozzle::PUMP);
fly::Turret turret(head, gun, 900, 800);
fly::Sonar sonar(turret, 2, 3, 4);  // TRIG, ECHO, кнопка перекалібрування

void setup() {
  sonar.staticOnly = STATIC_ONLY;
  base.add(turret);
  base.add(sonar);
  base.begin();
}

void loop() { base.update(); }
