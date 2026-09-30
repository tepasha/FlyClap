/*
 * FlySonar ESP32 — турель, що бачить мошку, на одній платі (ESP32-CAM / ESP32-S3 CAM).
 *
 * Камера дивиться на підсвічений фон, модуль CameraEyes веде ціль і наводить
 * турель на цій же платі. Керування й калібрування — з USB-консолі (115200,
 * клавіша h). Опис, фон і калібрування — docs/flysonar-esp32.md.
 *
 * Модулі:  основа + Turret (PanTilt + Nozzle) + CameraEyes (сенсор)
 * Збірка:  pio run -e flysonar_esp32 -t upload      (ESP32-CAM AI-Thinker)
 *          pio run -e flysonar_esp32_s3 -t upload   (ESP32-S3-EYE / Freenove S3 CAM)
 */
#include <Arduino.h>
#include <FlyDefense.h>

#ifndef SHOOTER_VALVE
#define SHOOTER_VALVE 1  // 1 — клапан + бак під тиском, 0 — помпа R385
#endif

#if CONFIG_IDF_TARGET_ESP32S3
// Freenove ESP32-S3-WROOM CAM / ESP32-S3-EYE; на інших платах перевірте, що піни вільні
const fly::CameraPins &CAMERA = fly::CAM_ESP32S3_EYE;
const uint8_t PIN_PAN = 1, PIN_TILT = 14, PIN_VALVE = 21, PIN_ARM = 47, PIN_LED = 2;
const bool LED_ACTIVE_LOW = false;
#else
// ESP32-CAM AI-Thinker. Без SD-картки. GPIO12 — strapping-пін: тумблер ARM лише на GND
// і БЕЗ зовнішньої підтяжки, інакше плата не завантажиться.
const fly::CameraPins &CAMERA = fly::CAM_AI_THINKER;
const uint8_t PIN_PAN = 14, PIN_TILT = 15, PIN_VALVE = 13, PIN_ARM = 12, PIN_LED = 33;
const bool LED_ACTIVE_LOW = true;  // червоний LED на платі
#endif

fly::Base base(PIN_ARM, PIN_LED, LED_ACTIVE_LOW);
fly::PanTilt head(PIN_PAN, PIN_TILT);
fly::Nozzle gun(PIN_VALVE, SHOOTER_VALVE ? fly::Nozzle::VALVE : fly::Nozzle::PUMP);
fly::Turret turret(head, gun);
fly::CameraEyes eyes(turret, CAMERA);

void setup() {
  base.add(turret);
  base.add(eyes);
  base.begin();
}

void loop() { base.update(); }
