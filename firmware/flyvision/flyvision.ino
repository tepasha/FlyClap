/*
 * FlyVision — оптичні "очі" для FlySonar на ESP32-CAM / ESP32-S3-CAM
 *
 * Камера дивиться на яскравий підсвічений фон. Комаха на ньому — темна точка,
 * яку видно навіть коли вона займає 2–4 пікселі (мошка 1–3 мм). ESP32 шукає
 * точки, веде ціль, прогнозує її положення на час пострілу, перераховує
 * пікселі в кути серв і надсилає команди на Arduino (FlySonar у режимі
 * TARGET_SOURCE_CAMERA). Рішення "стріляти" остаточно приймає Arduino:
 * тумблер ARM, кулдаун і фільтр великих об'єктів дублюються там.
 *
 * Зв'язок (лише ESP32 -> Arduino, 115200 8N1, рядки ASCII):
 *   A <pan10> <tilt10> <fire>\n  — навестись (кути в 0.1°), fire=1 — можна стріляти
 *   M <pan10> <tilt10>\n         — просто навестись (калібрування)
 *   F <ms>\n                     — тестовий постріл (калібрування)
 *   B\n                          — у кадрі великий об'єкт: не стріляти
 *
 * Підключення: GPIO14 (TX) -> D0 (RX) Arduino, GND -> GND.
 * 3.3 В логіки ESP32 Arduino читає як HIGH, дільник не потрібен.
 *
 * Керування через USB Serial (115200), див. docs/flysonar.md → "Калібрування камери".
 */

#include "esp_camera.h"
#include <Preferences.h>
#include "vision.h"

// ---------------- Плата ----------------
// Оберіть одну (або прапорцем -D у platformio.ini). Піни взяті з camera_pins.h
// прикладу CameraWebServer з arduino-esp32.
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
#else
#error "Select a camera model"
#endif

const int PIN_LINK_TX = 14;          // вільний на обох платах (на AI-Thinker — лінія SD, картку не ставимо)

// ---------------- Налаштування ----------------
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

// ---------------- Стан ----------------
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

void sendMove()                { Serial1.printf("M %d %d\n", jogPan10, jogTilt10); }
void jog(int16_t dp, int16_t dt) {
  jogPan10  = constrain(jogPan10 + dp, 0, 1800);
  jogTilt10 = constrain(jogTilt10 + dt, 0, 1800);
  sendMove();
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
      if (calMode) sendMove();
      break;
    case 'a': if (calMode) jog(-JOG_FINE10, 0); break;
    case 'd': if (calMode) jog(+JOG_FINE10, 0); break;
    case 'w': if (calMode) jog(0, +JOG_FINE10); break;
    case 's': if (calMode) jog(0, -JOG_FINE10); break;
    case 'A': if (calMode) jog(-JOG_COARSE10, 0); break;
    case 'D': if (calMode) jog(+JOG_COARSE10, 0); break;
    case 'W': if (calMode) jog(0, +JOG_COARSE10); break;
    case 'S': if (calMode) jog(0, -JOG_COARSE10); break;
    case 'f': if (calMode) Serial1.printf("F %u\n", (unsigned)TEST_SHOT_MS); break;
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
  Serial1.begin(115200, SERIAL_8N1, -1, PIN_LINK_TX);   // лише TX
  delay(300);
  Serial.println(F("\nFlyVision"));
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
    Serial1.print("B\n");
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
  Serial1.printf("A %d %d %d\n", (int)lroundf(pan10), (int)lroundf(tilt10), fire ? 1 : 0);
}
