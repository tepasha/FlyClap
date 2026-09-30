/*
 * FlySonar — турель на Arduino, якою керує ESP32-CAM ("очі").
 *
 * ESP32 з прошивкою src/flysonar_eyes бачить мошку й шле кути по UART; Arduino —
 * контролер реального часу: наводить серви, відкриває клапан/помпу, тримає ARM,
 * серії, кулдаун і блокування великих об'єктів. Опис — docs/flysonar-esp32.md.
 *
 * Модулі:  основа + Turret (PanTilt + Nozzle) + EyesLink (прийом по UART)
 * Збірка:  pio run -e flysonar_turret -t upload   (flysonar_turret_nano — Nano)
 *
 * Пінаут:
 *   D0  — RX від ESP32 GPIO14. Від'єднувати на час прошивки Arduino!
 *   D5  — MOSFET помпи/клапана    D9  — серво PAN     D10 — серво TILT
 *   D13 — статус-LED              A0  — тумблер ARM (до GND)
 */
#include <Arduino.h>
#include <FlyDefense.h>

#ifndef SHOOTER_VALVE
#define SHOOTER_VALVE 0  // 0 — помпа R385, 1 — клапан 12 В + бак під тиском
#endif

fly::Base base(A0, 13);
fly::PanTilt head(9, 10);
fly::Nozzle gun(5, SHOOTER_VALVE ? fly::Nozzle::VALVE : fly::Nozzle::PUMP);
fly::Turret turret(head, gun);
fly::EyesLink eyes(turret);  // слухає Serial (D0)

void setup() {
  base.add(turret);
  base.add(eyes);
  base.begin();
}

void loop() { base.update(); }
