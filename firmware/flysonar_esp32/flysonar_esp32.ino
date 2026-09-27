/*
 * FlySonar — ESP32-версія: турель з водометом, що бачить мошку (ESP32-CAM / ESP32-S3-CAM)
 *
 * Камера дивиться на яскравий підсвічений фон. Комаха на ньому — темна точка,
 * яку видно навіть коли вона займає 2–4 пікселі (мошка 1–3 мм). ESP32 шукає
 * точки, веде ціль, прогнозує її положення на час пострілу, перераховує
 * пікселі в кути серв і стріляє.
 *
 * Дві ролі (ESP32_ROLE_EYES):
 *   0 — САМОСТІЙНА (за замовчуванням): одна плата — зір + серви + клапан + ARM.
 *       Arduino не потрібен. Серви й клапан обслуговує окрема задача FreeRTOS
 *       кожну 1 мс, тож таймінг пострілу не залежить від кадрів камери.
 *   1 — "ОЧІ" для Arduino-версії (firmware/flysonar, TARGET_SOURCE_CAMERA 1):
 *       ESP32 лише бачить і шле команди по UART, стріляє Arduino.
 *       Протокол (ESP32 -> Arduino, 115200 8N1, рядки ASCII):
 *         A <pan10> <tilt10> <fire>\n  — навестись (кути в 0.1°), fire=1 — можна стріляти
 *         M <pan10> <tilt10>\n         — просто навестись (калібрування)
 *         F <ms>\n                     — тестовий постріл (калібрування)
 *         B\n                          — у кадрі великий об'єкт: не стріляти
 *
 * Керування й калібрування — через USB Serial (115200), див. docs/flysonar-esp32.md.
 */

#include "esp_camera.h"
#include <Preferences.h>
#include "vision.h"

#ifndef ESP32_ROLE_EYES
#define ESP32_ROLE_EYES 0
#endif
#ifndef SHOOTER_VALVE
#define SHOOTER_VALVE   1   // 1 — клапан + бак під тиском, 0 — помпа R385
#endif

#if !ESP32_ROLE_EYES
#include "driver/ledc.h"
#include "turret.h"
#endif

// ---------------- Плата ----------------
// Оберіть одну (або прапорцем -D у platformio.ini). Піни камери взяті з
// camera_pins.h прикладу CameraWebServer з arduino-esp32.
#if !defined(CAMERA_MODEL_AI_THINKER) && !defined(CAMERA_MODEL_ESP32S3_EYE)
#define CAMERA_MODEL_AI_THINKER      // ESP32-CAM (AI-Thinker), найдешевша
// #define CAMERA_MODEL_ESP32S3_EYE  // ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM
#endif

#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
// Вільні піни (без SD-картки). GPIO12 — strapping-пін: на старті не має бути
// HIGH, тому тумблер ARM лише на GND і БЕЗ зовнішньої підтяжки.
const int PIN_LINK_TX = 14;          // роль "очі"
const int PIN_PAN     = 14;          // самостійна роль
const int PIN_TILT    = 15;
const int PIN_VALVE   = 13;
const int PIN_ARM     = 12;
const int PIN_LED     = 33;          // червоний LED на платі
const bool LED_ACTIVE_LOW = true;
#elif defined(CAMERA_MODEL_ESP32S3_EYE)
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y2_GPIO_NUM    11
#define Y3_GPIO_NUM    9
#define Y4_GPIO_NUM    8
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    12
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    16
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13
// Розкладка під Freenove ESP32-S3-WROOM CAM; на інших платах перевірте, що піни вільні
const int PIN_LINK_TX = 14;
const int PIN_PAN     = 1;
const int PIN_TILT    = 14;
const int PIN_VALVE   = 21;
const int PIN_ARM     = 47;
const int PIN_LED     = 2;
const bool LED_ACTIVE_LOW = false;
#else
#error "Select a camera model"
#endif

// ---------------- Налаштування зору ----------------
const uint32_t LATENCY_MS        = 70;    // кадр + обробка + доворот серв + політ струменя
const uint16_t LOCK_HITS         = 5;     // стільки кадрів поспіль бачимо ціль, перш ніж дозволити постріл
const float    STILL_PX_S        = 15.0f; // повільніше — ціль "сидить"
const uint32_t BIG_HOLD_MS       = 3000;  // після великого об'єкта мовчимо стільки
const uint32_t EXPOSURE_LOCK_MS  = 2500;  // дати автоекспозиції встановитись, потім заморозити
const int16_t  JOG_FINE10        = 5;     // 0.5°
const int16_t  JOG_COARSE10      = 50;    // 5°
const uint16_t TEST_SHOT_MS      = 40;
const int      MAX_CAL_POINTS    = 9;

bool fireMoving = false;                  // стріляти й по цілях у польоті (з упередженням)

// ---------------- Стан зору ----------------
fv::Vision        vision;
fv::Track         track;
fv::TrackerConfig trackCfg;
fv::Affine        cal;
fv::CalPoint      calPts[MAX_CAL_POINTS];
int               calCount = 0;
bool              calMode = false;
int16_t           jogPan10 = 900, jogTilt10 = 900;
uint32_t          bigUntil = 0;
bool              exposureLocked = false;
uint32_t          bootMs = 0;
uint32_t          frames = 0, fpsMark = 0;
float             fps = 0;
fv::Blob          lastBlobs[8];
int               lastBlobCount = 0;

Preferences prefs;

// =====================================================================
//   Вихід: або власна турель (самостійна роль), або UART на Arduino
// =====================================================================
#if ESP32_ROLE_EYES

void outBegin() { Serial1.begin(115200, SERIAL_8N1, -1, PIN_LINK_TX); }   // лише TX
void outAim(int pan10, int tilt10, bool fire) { Serial1.printf("A %d %d %d\n", pan10, tilt10, fire ? 1 : 0); }
void outMove(int pan10, int tilt10) { Serial1.printf("M %d %d\n", pan10, tilt10); }
void outTestShot(uint16_t ms) { Serial1.printf("F %u\n", (unsigned)ms); }
void outBig() { Serial1.print("B\n"); }
void outPoll() {}

#else

// Серви — через драйвер LEDC ESP-IDF на явно заданому таймері. ledcAttach()
// з ядра arduino-esp32 3.x не бачить таймер XCLK камери (його налаштовує
// драйвер камери напряму) і може забрати таймер 0, зупинивши камеру.
const ledc_mode_t    SERVO_MODE  = LEDC_LOW_SPEED_MODE;
const ledc_timer_t   SERVO_TIMER = LEDC_TIMER_2;          // камера — LEDC_TIMER_0 / канал 0
const ledc_channel_t CH_PAN      = LEDC_CHANNEL_4;
const ledc_channel_t CH_TILT     = LEDC_CHANNEL_5;
const uint32_t       SERVO_HZ    = 50;
const uint32_t       SERVO_BITS  = 14;                    // 20 мс / 16384 ≈ 1.2 мкс ≈ 0.12°
const uint16_t       SERVO_US_MIN = 544, SERVO_US_MAX = 2400;   // як у Arduino Servo: 0..180°

fv::Turret   turret;
portMUX_TYPE turretMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t shotsReported = 0;

void servoWrite(ledc_channel_t ch, int16_t deg10) {
  uint32_t us = SERVO_US_MIN + (int32_t)deg10 * (SERVO_US_MAX - SERVO_US_MIN) / 1800;
  ledc_set_duty(SERVO_MODE, ch, us * (1u << SERVO_BITS) / (1000000u / SERVO_HZ));
  ledc_update_duty(SERVO_MODE, ch);
}

void servoSetup(ledc_channel_t ch, int pin, int16_t deg10) {
  ledc_channel_config_t c = {};
  c.gpio_num   = pin;
  c.speed_mode = SERVO_MODE;
  c.channel    = ch;
  c.timer_sel  = SERVO_TIMER;
  c.duty       = 0;
  c.hpoint     = 0;
  ledc_channel_config(&c);
  servoWrite(ch, deg10);
}

inline bool armed() { return digitalRead(PIN_ARM) == LOW; }
inline void led(bool on) { digitalWrite(PIN_LED, on != LED_ACTIVE_LOW ? HIGH : LOW); }

// Задача реального часу: клапан, серви, ARM, LED — кожну 1 мс
void actuatorTask(void *) {
  int16_t shownPan = -1, shownTilt = -1;
  for (;;) {
    const uint32_t now = millis();
    const bool arm = armed();
    portENTER_CRITICAL(&turretMux);
    turret.update(now, arm);
    const bool    valve = turret.valveOn;
    const int16_t p = turret.pan10, t = turret.tilt10;
    const uint32_t shots = turret.shots;
    portEXIT_CRITICAL(&turretMux);

    digitalWrite(PIN_VALVE, valve ? HIGH : LOW);
    if (p != shownPan)  { servoWrite(CH_PAN, p);  shownPan = p; }
    if (t != shownTilt) { servoWrite(CH_TILT, t); shownTilt = t; }
    shotsReported = shots;

    // LED: ARM — блимає, SAFE — горить
    led(arm ? (now / 250) % 2 : true);
    vTaskDelay(1);
  }
}

void outBegin() {
  pinMode(PIN_VALVE, OUTPUT);
  digitalWrite(PIN_VALVE, LOW);
  pinMode(PIN_ARM, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);

  turret.cfg.shotMs = SHOOTER_VALVE ? 50 : 120;
  turret.cfg.bigHoldMs = BIG_HOLD_MS;

  ledc_timer_config_t tc = {};
  tc.speed_mode      = SERVO_MODE;
  tc.duty_resolution = (ledc_timer_bit_t)SERVO_BITS;
  tc.timer_num       = SERVO_TIMER;
  tc.freq_hz         = SERVO_HZ;
  tc.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&tc);
  servoSetup(CH_PAN, PIN_PAN, 900);
  servoSetup(CH_TILT, PIN_TILT, 900);

  // Ядро 0: зір і loop() живуть на ядрі 1 (на одноядерних чипах — просто окрема задача)
  xTaskCreatePinnedToCore(actuatorTask, "turret", 3072, nullptr, 3, nullptr, 0);
}

void outAim(int pan10, int tilt10, bool fire) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&turretMux);
  turret.aim(pan10, tilt10, now);
  turret.request(fire, now);
  portEXIT_CRITICAL(&turretMux);
}

void outMove(int pan10, int tilt10) { outAim(pan10, tilt10, false); }

void outTestShot(uint16_t ms) {
  const uint32_t now = millis();
  const bool arm = armed();
  portENTER_CRITICAL(&turretMux);
  bool ok = turret.testShot(ms, now, arm);
  portEXIT_CRITICAL(&turretMux);
  if (!ok) Serial.println(F("Test shot refused: ARM off, servo moving or big object hold"));
}

void outBig() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&turretMux);
  turret.bigObject(now);
  portEXIT_CRITICAL(&turretMux);
}

// Лог пострілів — з loop(), щоб не друкувати з задачі реального часу
void outPoll() {
  static uint32_t logged = 0;
  if (shotsReported != logged) {
    logged = shotsReported;
    Serial.printf("PSSHT! total=%u\n", (unsigned)logged);
  }
}
#endif

// ---------------- Камера ----------------
bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_GRAYSCALE;  // 1 байт/піксель, без JPEG — одразу в алгоритм
  c.frame_size   = FRAMESIZE_QVGA;       // 320×240: ~1 мм/піксель на полі 30 см
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;   // завжди найсвіжіший кадр — менша затримка
  if (esp_camera_init(&c) != ESP_OK) return false;
  return vision.begin(320, 240);
}

// Після встановлення експозиції фіксуємо її: автоекспозиція, що "дихає",
// ламає модель фону й дає хибні цілі.
void lockExposure() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_exposure_ctrl(s, 0);
  s->set_gain_ctrl(s, 0);
  s->set_whitebal(s, 0);
  exposureLocked = true;
  vision.resetBackground();
  Serial.println(F("Exposure locked, background re-learned"));
}

// ---------------- Калібрування ----------------
void loadCal() {
  prefs.begin("flyvision", true);
  if (prefs.getBytesLength("cal") == sizeof(cal)) prefs.getBytes("cal", &cal, sizeof(cal));
  prefs.end();
  Serial.println(cal.valid ? F("Calibration loaded") : F("NOT CALIBRATED - press 'm' to calibrate"));
}

void saveCal() {
  prefs.begin("flyvision", false);
  prefs.putBytes("cal", &cal, sizeof(cal));
  prefs.end();
}

void jog(int16_t dp, int16_t dt) {
  jogPan10  = constrain(jogPan10 + dp, 0, 1800);
  jogTilt10 = constrain(jogTilt10 + dt, 0, 1800);
  outMove(jogPan10, jogTilt10);
  Serial.printf("aim pan=%.1f tilt=%.1f\n", jogPan10 / 10.0f, jogTilt10 / 10.0f);
}

void printHelp() {
  Serial.println(F(
    "Keys:\n"
    "  m        calibration mode on/off\n"
    "  a d w s  jog 0.5 deg (A D W S = 5 deg)   [cal mode]\n"
    "  f        test shot (ARM must be on)      [cal mode]\n"
    "  c        capture point: marker on the wet spot, exactly one dark dot in view\n"
    "  u        undo last point,  v  solve & save (>=3 points)\n"
    "  b        re-learn background (clear backdrop first)\n"
    "  t        toggle firing at moving targets (lead aim)\n"
    "  p        status"));
}

void printStatus() {
  Serial.printf("fps=%.1f cal=%s points=%d mode=%s fireMoving=%d blobs=%d track=%d hits=%u v=%.0fpx/s\n",
                fps, cal.valid ? "ok" : "NONE", calCount, calMode ? "CAL" : "RUN", fireMoving,
                lastBlobCount, track.active, track.hits, track.speed());
  if (cal.valid)
    Serial.printf("pan10 = %.3f*x + %.3f*y + %.1f ; tilt10 = %.3f*x + %.3f*y + %.1f\n",
                  cal.a[0], cal.a[1], cal.a[2], cal.b[0], cal.b[1], cal.b[2]);
}

void handleKey(char k) {
  switch (k) {
    case 'm':
      calMode = !calMode;
      Serial.println(calMode ? F("CAL mode: shoot (f), put a dark marker on the wet spot, capture (c)")
                             : F("RUN mode"));
      if (calMode) outMove(jogPan10, jogTilt10);
      break;
    case 'a': if (calMode) jog(-JOG_FINE10, 0); break;
    case 'd': if (calMode) jog(+JOG_FINE10, 0); break;
    case 'w': if (calMode) jog(0, +JOG_FINE10); break;
    case 's': if (calMode) jog(0, -JOG_FINE10); break;
    case 'A': if (calMode) jog(-JOG_COARSE10, 0); break;
    case 'D': if (calMode) jog(+JOG_COARSE10, 0); break;
    case 'W': if (calMode) jog(0, +JOG_COARSE10); break;
    case 'S': if (calMode) jog(0, -JOG_COARSE10); break;
    case 'f': if (calMode) outTestShot(TEST_SHOT_MS); break;
    case 'c':
      if (!calMode) break;
      if (lastBlobCount != 1) { Serial.printf("Need exactly one dark dot in view, see %d\n", lastBlobCount); break; }
      if (calCount >= MAX_CAL_POINTS) { Serial.println(F("Too many points, solve with 'v'")); break; }
      calPts[calCount++] = { lastBlobs[0].x, lastBlobs[0].y, (float)jogPan10, (float)jogTilt10 };
      Serial.printf("Point %d: px=(%.1f, %.1f) -> pan=%.1f tilt=%.1f\n", calCount,
                    lastBlobs[0].x, lastBlobs[0].y, jogPan10 / 10.0f, jogTilt10 / 10.0f);
      break;
    case 'u': if (calCount) { calCount--; Serial.printf("Points: %d\n", calCount); } break;
    case 'v': {
      fv::Affine a;
      if (!fv::solveAffine(calPts, calCount, a)) {
        Serial.println(F("Need >=3 points not on one line"));
        break;
      }
      cal = a;
      saveCal();
      float worst = 0;                            // залишкова похибка по точках
      for (int i = 0; i < calCount; i++) {
        float p, t; cal.apply(calPts[i].x, calPts[i].y, p, t);
        worst = fmaxf(worst, fmaxf(fabsf(p - calPts[i].pan10), fabsf(t - calPts[i].tilt10)));
      }
      Serial.printf("Saved. Worst residual %.2f deg. Remove markers and press 'b'.\n", worst / 10.0f);
      calCount = 0;
      break;
    }
    case 'b': vision.resetBackground(); Serial.println(F("Background reset")); break;
    case 't': fireMoving = !fireMoving; Serial.printf("fireMoving=%d\n", fireMoving); break;
    case 'p': printStatus(); break;
    case 'h': case '?': printHelp(); break;
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  outBegin();
  delay(300);
  Serial.println(ESP32_ROLE_EYES ? F("\nFlySonar ESP32 - eyes for Arduino") : F("\nFlySonar ESP32 - standalone"));
  if (!cameraInit()) {
    Serial.println(F("Camera init FAILED - check board define and PSRAM"));
    while (true) delay(1000);
  }
  loadCal();
  printHelp();
  bootMs = fpsMark = millis();
}

void loop() {
  while (Serial.available()) handleKey((char)Serial.read());
  outPoll();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  const uint32_t now = millis();

  bool big = false;
  lastBlobCount = vision.process(fb->buf, lastBlobs, 8, big);
  esp_camera_fb_return(fb);

  if (!exposureLocked && now - bootMs > EXPOSURE_LOCK_MS) lockExposure();

  frames++;
  if (now - fpsMark >= 1000) { fps = frames * 1000.0f / (now - fpsMark); frames = 0; fpsMark = now; }

  if (big) {
    if ((int32_t)(now - bigUntil) >= 0) Serial.println(F("Big object - holding fire"));
    bigUntil = now + BIG_HOLD_MS;
    outBig();
  }

  if (calMode || !exposureLocked) return;        // у калібруванні приціл веде людина

  fv::trackUpdate(track, lastBlobs, lastBlobCount, now, trackCfg);
  if (!track.active || !cal.valid) return;

  const bool still = track.speed() < STILL_PX_S;
  // По нерухомій цілі цілимось у поточну точку, по рухомій — з упередженням
  float x = still ? track.x : track.predictX(LATENCY_MS);
  float y = still ? track.y : track.predictY(LATENCY_MS);
  float pan10, tilt10;
  cal.apply(x, y, pan10, tilt10);

  const bool fire = track.hits >= LOCK_HITS && track.misses == 0 &&
                    (int32_t)(now - bigUntil) >= 0 && (still || fireMoving);
  outAim((int)lroundf(pan10), (int)lroundf(tilt10), fire);
}
