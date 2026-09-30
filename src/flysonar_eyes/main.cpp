/*
 * FlySonar Eyes — ESP32-CAM як "очі" для турелі на Arduino (прошивка src/flysonar_turret).
 *
 * ESP32 лише бачить і шле команди по UART (TX GPIO14 → D0 Arduino, спільна земля);
 * стріляє Arduino, тумблер ARM — теж на ньому. Калібрування — з USB-консолі ESP32
 * (115200, клавіша h). Опис — docs/flysonar-esp32.md.
 *
 * Модулі:  основа + CameraEyes (сенсор) + LinkOut (передавач)
 * Збірка:  pio run -e flysonar_esp32_eyes -t upload   (flysonar_esp32_s3_eyes — ESP32-S3)
 */
#include <Arduino.h>
#include <FlyDefense.h>

#if CONFIG_IDF_TARGET_ESP32S3
const fly::CameraPins &CAMERA = fly::CAM_ESP32S3_EYE;
const uint8_t PIN_LED = 2;
const bool LED_ACTIVE_LOW = false;
#else
const fly::CameraPins &CAMERA = fly::CAM_AI_THINKER;
const uint8_t PIN_LED = 33;
const bool LED_ACTIVE_LOW = true;
#endif
const uint8_t PIN_LINK_TX = 14;

fly::Base base(fly::Base::NO_PIN, PIN_LED, LED_ACTIVE_LOW);  // ARM — на Arduino
fly::LinkOut toArduino(Serial1, PIN_LINK_TX);  // TX → D0 Arduino
fly::CameraEyes eyes(toArduino, CAMERA);

void setup() {
  base.add(toArduino);
  base.add(eyes);
  base.begin();
}

void loop() { base.update(); }
