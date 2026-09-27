/*
 * FlySonar — "турель" з водометом для мух (Arduino Uno / Nano)
 *
 * Два джерела цілей (TARGET_SOURCE_CAMERA):
 *
 * 0 — СОНАР HC-SR04 на pan-tilt платформі разом із соплом:
 *   1) CALIBRATE: обхід сітки кутів, запам'ятовуємо "фон" (медіана з 5 вимірів).
 *   2) SCAN: змійкою по сітці; ехо значно ближче за фон — кандидат.
 *   3) CONFIRM: медіана з 3 + перевірка сусідніх кутів (широке ехо — рука/кіт,
 *      НЕ стріляємо).
 *   4) REFINE: дрібний скан ±10° кроком 2° по горизонталі й вертикалі,
 *      ціль = центроїд кутів, де є ехо. Точність ~1–2° замість кроку сітки 5°.
 *   5) STILL: кілька вимірів у центроїді; стріляємо лише по цілі, що сидить
 *      (STATIC_ONLY) — по мусі в польоті сонар все одно не влучить.
 *   6) FIRE: поправка на сопло й балістику, постріл, перевірка, до MAX_SHOTS.
 *
 * 1 — КАМЕРА: ESP32-CAM з прошивкою firmware/flyvision шукає комах на
 *   підсвіченому фоні й надсилає готові кути по UART у D0 (RX). Arduino тут —
 *   контролер реального часу: плавно наводить серви, відкриває клапан/помпу,
 *   тримає ARM, кулдаун і блокування великих об'єктів. Протокол — див. flyvision.ino.
 *
 * Серви керуються через writeMicroseconds з роздільністю 0.1° (кути в коді —
 * у десятих градуса, *10).
 *
 * Пінаут:
 *   D0  — (камера) RX від ESP32 GPIO14. Від'єднувати на час прошивки Arduino!
 *   D2  — HC-SR04 TRIG           (сонар)
 *   D3  — HC-SR04 ECHO           (сонар)
 *   D4  — кнопка перекалібрування фону (сонар; INPUT_PULLUP, до GND)
 *   D5  — затвор MOSFET помпи або клапана (IRLZ44N)
 *   D9  — серво PAN  (горизонталь)
 *   D10 — серво TILT (вертикаль)
 *   D13 — статус-LED
 *   A0  — тумблер ARM (INPUT_PULLUP, LOW = стріляти дозволено)
 */

#include <Servo.h>

// ---------------- Конфігурація збірки ----------------
// Можна перевизначити прапорцями компілятора (див. platformio.ini)
#ifndef TARGET_SOURCE_CAMERA
#define TARGET_SOURCE_CAMERA 0  // 0 — сонар HC-SR04, 1 — ESP32-CAM (firmware/flyvision)
#endif
#ifndef SHOOTER_VALVE
#define SHOOTER_VALVE        0  // 0 — помпа R385, 1 — клапан 12 В + бак під тиском
#endif
#ifndef STATIC_ONLY
#define STATIC_ONLY          1  // сонар: стріляти лише по нерухомій цілі
#endif

// ---------------- Серви ----------------
const uint16_t SERVO_US_MIN        = 544;   // як у Servo::write(): 0°
const uint16_t SERVO_US_MAX        = 2400;  // 180°
const uint8_t  SERVO_SETTLE_BASE_MS = 12;   // мінімальна пауза після команди
const uint8_t  SERVO_MS_PER_DEG     = 2;    // MG90S: ~0.1 с/60° + запас
const uint16_t SERVO_SETTLE_MAX_MS  = 300;

// ---------------- Водомет ----------------
#if SHOOTER_VALVE
const uint16_t SHOT_MS              = 50;    // клапан відкривається за ~5–10 мс, тиск уже є
const float    BALLISTIC_DEG_PER_CM = 0.08f; // струмінь під тиском майже не провисає
#else
const uint16_t SHOT_MS              = 120;   // помпі потрібен час, щоб набрати тиск
const float    BALLISTIC_DEG_PER_CM = 0.25f;
#endif
const int16_t  PAN_NOZZLE_OFFSET10  = 0;     // сопло збоку від сонара — поправка, 0.1°
const int16_t  TILT_NOZZLE_OFFSET10 = 20;    // сопло під сонаром — трохи вгору, 0.1°
const int8_t   TILT_UP_SIGN         = +1;    // +1, якщо більший кут серви = вгору
const uint8_t  MAX_SHOTS            = 3;
const uint16_t COOLDOWN_MS          = 2500;

// ---------------- Піни ----------------
const uint8_t PIN_TRIG = 2, PIN_ECHO = 3, PIN_RECAL = 4, PIN_PUMP = 5;
const uint8_t PIN_PAN = 9, PIN_TILT = 10, PIN_LED = 13, PIN_ARM = A0;

Servo    panServo, tiltServo;
int16_t  curPan10 = 900, curTilt10 = 900;
uint32_t shotsTotal = 0;

// ============ Спільне ============

inline bool armed() { return digitalRead(PIN_ARM) == LOW; }
inline bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }  // стійко до переповнення millis()

inline uint16_t deg10ToUs(int16_t d10) {
  return SERVO_US_MIN + (int32_t)d10 * (SERVO_US_MAX - SERVO_US_MIN) / 1800;
}

// Навести без очікування. Повертає, скільки мс серво їхатиме.
uint16_t aim10(int pan10, int tilt10) {
  int16_t pan  = constrain(pan10, 0, 1800);
  int16_t tilt = constrain(tilt10, 0, 1800);
  int16_t dist = max(abs(pan - curPan10), abs(tilt - curTilt10));
  panServo.writeMicroseconds(deg10ToUs(pan));
  tiltServo.writeMicroseconds(deg10ToUs(tilt));
  curPan10 = pan; curTilt10 = tilt;
  uint32_t ms = SERVO_SETTLE_BASE_MS + (uint32_t)dist * SERVO_MS_PER_DEG / 10;
  return ms > SERVO_SETTLE_MAX_MS ? SERVO_SETTLE_MAX_MS : ms;
}

// Навести й дочекатись
void moveTo10(int pan10, int tilt10) { delay(aim10(pan10, tilt10)); }

void logShot(int pan10, int tilt10) {
  shotsTotal++;
  Serial.print(F("PSSHT! pan=")); Serial.print(pan10 / 10.0f, 1);
  Serial.print(F(" tilt="));      Serial.print(tilt10 / 10.0f, 1);
  Serial.print(F(" total="));     Serial.println(shotsTotal);
}

#if !TARGET_SOURCE_CAMERA
// =====================================================================
//                              СОНАР
// =====================================================================

// ---------------- Геометрія сканування (градуси) ----------------
const uint8_t PAN_MIN   = 30;
const uint8_t PAN_MAX   = 150;
const uint8_t PAN_STEP  = 5;                           // ~ ширина пелюстки HC-SR04 / 3
const uint8_t PAN_CELLS = (PAN_MAX - PAN_MIN) / PAN_STEP + 1;   // 25
const uint8_t TILT_LEVELS[] = { 80, 95, 110 };
const uint8_t TILT_CELLS = sizeof(TILT_LEVELS);

// ---------------- Сонар ----------------
const uint16_t ECHO_TIMEOUT_US = 6000;   // ~1 м — далі нас не цікавить
const uint16_t PING_GAP_MS     = 30;     // пауза між пінгами, щоб не ловити старе ехо
const uint16_t ECHO_IDLE_TIMEOUT_MS = 50; // без ехо HC-SR04 тримає ECHO у HIGH ~38 мс
const uint16_t NO_ECHO         = 999;
const uint8_t  MIN_CM          = 8;      // ближче — мертва зона HC-SR04 / сам корпус
const uint8_t  MAX_CM          = 60;     // далі муху вже не видно в шумі
const uint8_t  DELTA_CM        = 10;     // на скільки ближче за фон має бути ціль
const uint8_t  SIMILAR_CM      = 6;      // "та сама відстань" для перевірки ширини
// Пелюстка ~16° накриває до 4 клітинок сітки 5°, тож навіть точкова муха дає
// ехо в сусідах. Великий об'єкт (рука, кіт) — суцільна смуга з 5+ клітинок.
const int8_t   WIDE_RANGE      = 6;      // далі в кожен бік не дивимось
const uint8_t  WIDE_SPAN       = 5;      // суцільна смуга стільки клітинок = великий об'єкт
const uint16_t FRESH_MS        = 400;    // вимір сусідньої клітинки, свіжіший за це, не повторюємо

// ---------------- Уточнення й "сидить" ----------------
const int16_t  FINE_SPAN10     = 100;    // дрібний скан ±10°
const int16_t  FINE_STEP10     = 20;     // кроком 2°
const uint8_t  FINE_MIN_HITS   = 2;      // менше влучань — ехо випадкове
const uint8_t  FINE_PASSES     = 3;      // макс. пересувань вікна
const uint8_t  STILL_SAMPLES   = 4;      // вимірів у центроїді
const uint8_t  STILL_CM        = 2;      // розкид відстані, за якого ціль "сидить"

uint16_t background[TILT_CELLS][PAN_CELLS];
uint16_t lastScan[TILT_CELLS][PAN_CELLS];    // останній вимір у кожній клітинці...
uint32_t lastScanAt[TILT_CELLS][PAN_CELLS];  // ...і коли його зроблено
uint32_t cooldownUntil[TILT_CELLS];          // кулдаун по рядах (дешево по RAM)

uint16_t pingCm() {
  static uint32_t lastPing = 0;
  while (millis() - lastPing < PING_GAP_MS) { /* чекаємо затухання відлуння */ }

  // Якщо попередній пінг не отримав ехо, сенсор ще тримає ECHO у HIGH і проігнорує
  // новий тригер. Дочекаємось LOW, інакше кожен другий вимір губиться.
  uint32_t waitStart = millis();
  while (digitalRead(PIN_ECHO) == HIGH) {
    if (millis() - waitStart > ECHO_IDLE_TIMEOUT_MS) {   // сенсор завис/відключений
      lastPing = millis();
      return NO_ECHO;
    }
  }
  lastPing = millis();

  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  uint32_t us = pulseIn(PIN_ECHO, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return NO_ECHO;
  return us / 58;
}

uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { uint16_t t = a; a = b; b = t; }
  if (b > c) { uint16_t t = b; b = c; c = t; }
  if (a > b) { uint16_t t = a; a = b; b = t; }
  return b;
}

uint16_t pingMedian3() { return median3(pingCm(), pingCm(), pingCm()); }

inline int16_t panOf10(uint8_t i) { return (PAN_MIN + i * PAN_STEP) * 10; }
inline int16_t tiltOf10(uint8_t t) { return TILT_LEVELS[t] * 10; }

bool isForeground(uint16_t d, uint16_t bg) {
  if (d == NO_ECHO || d < MIN_CM || d > MAX_CM) return false;
  if (bg == NO_ECHO) return true;               // раніше там була порожнеча
  return d + DELTA_CM < bg;
}

// Ехо від тієї самої цілі (для дрібного скану, де фону по клітинці немає)
inline bool sameTarget(uint16_t dn, uint16_t d) {
  return dn != NO_ECHO && dn >= MIN_CM && abs((int)dn - (int)d) <= SIMILAR_CM;
}

// Навести сонар на клітинку, виміряти й запам'ятати результат
uint16_t measureCell(uint8_t t, uint8_t p) {
  moveTo10(panOf10(p), tiltOf10(t));
  uint16_t d = pingCm();
  lastScan[t][p]   = d;
  lastScanAt[t][p] = millis();
  return d;
}

void calibrate() {
  Serial.println(F("Calibrating background... keep the area clear"));
  digitalWrite(PIN_LED, HIGH);
  for (uint8_t t = 0; t < TILT_CELLS; t++) {
    for (uint8_t p = 0; p < PAN_CELLS; p++) {
      moveTo10(panOf10(p), tiltOf10(t));
      uint16_t s[5];
      for (uint8_t k = 0; k < 5; k++) s[k] = pingCm();
      // медіана з 5 (вставками)
      for (uint8_t i = 1; i < 5; i++) {
        uint16_t v = s[i]; int8_t j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
      }
      background[t][p] = s[2];
      lastScan[t][p]   = s[2];
      lastScanAt[t][p] = millis() - FRESH_MS;   // одразу "несвіжий"
    }
    cooldownUntil[t] = millis();
  }
  digitalWrite(PIN_LED, LOW);
  Serial.println(F("Background ready"));
}

// Ширина об'єкта: скільки клітинок поспіль (у обидва боки від p) бачать його на
// тій самій відстані. Рахуємо суцільну смугу, бо скан часто натрапляє на великий
// об'єкт з краю — симетрична перевірка сусідів тоді бачить лише половину.
// Клітинки, які змійка щойно пройшла, беремо з lastScan — серво не мотаються
// туди-сюди. Фізично перевимірюємо лише застарілі (зазвичай ті, що попереду).
uint8_t objectSpan(uint8_t t, uint8_t p, uint16_t d) {
  uint8_t span = 1;
  for (int8_t dir = -1; dir <= 1; dir += 2) {
    for (int8_t k = 1; k <= WIDE_RANGE; k++) {
      int8_t q = p + dir * k;
      if (q < 0 || q >= PAN_CELLS) break;
      uint16_t dn = (millis() - lastScanAt[t][q] < FRESH_MS) ? lastScan[t][q] : measureCell(t, q);
      if (!isForeground(dn, background[t][q]) || abs((int)dn - (int)d) > SIMILAR_CM) break;
      span++;
    }
  }
  return span;
}

// Дрібний скан однієї осі навколо center10. Повертає зсув центроїду (0.1°)
// або INT16_MIN, якщо влучань замало. Пелюстка симетрична, тому середнє
// кутів з ехо — це напрям на ціль. Якщо ехо є на краю вікна, вікно обрізало
// пелюстку з одного боку і середнє зміщене — пересуваємо вікно й повторюємо.
int16_t fineAxis(bool panAxis, int16_t center10, int16_t other10, uint16_t d) {
  int16_t shift = 0;
  for (uint8_t pass = 0; pass < FINE_PASSES; pass++) {
    int32_t sum = 0;
    uint8_t hits = 0;
    bool edge = false;
    for (int16_t off = -FINE_SPAN10; off <= FINE_SPAN10; off += FINE_STEP10) {
      if (panAxis) moveTo10(center10 + shift + off, other10);
      else         moveTo10(other10, center10 + shift + off);
      if (sameTarget(pingCm(), d)) {
        sum += off; hits++;
        if (off == -FINE_SPAN10 || off + FINE_STEP10 > FINE_SPAN10) edge = true;
      }
    }
    if (hits < FINE_MIN_HITS) return pass ? shift : INT16_MIN;
    int16_t delta = (int16_t)(sum / hits);
    shift += delta;
    if (!edge || abs(delta) < FINE_STEP10 / 2) break;   // вікно вже накриває пелюстку
  }
  return shift;
}

// Уточнити напрям на ціль. false — ціль зникла під час скану.
bool refineCentroid(uint8_t t, uint8_t p, uint16_t d, int16_t &pan10, int16_t &tilt10) {
  pan10 = panOf10(p); tilt10 = tiltOf10(t);
  int16_t dp = fineAxis(true, pan10, tilt10, d);
  if (dp == INT16_MIN) return false;
  pan10 += dp;
  int16_t dt = fineAxis(false, tilt10, pan10, d);
  if (dt != INT16_MIN) tilt10 += dt;             // по вертикалі може не вистачити — лишаємо рядок
  return true;
}

// Ціль "сидить": усі виміри є і розкид відстані малий. d — оновлюється медіаною.
bool isStill(int16_t pan10, int16_t tilt10, uint16_t &d) {
  moveTo10(pan10, tilt10);
  uint16_t lo = 0xFFFF, hi = 0, s[STILL_SAMPLES];
  for (uint8_t i = 0; i < STILL_SAMPLES; i++) {
    s[i] = pingCm();
    if (!sameTarget(s[i], d)) return false;
    if (s[i] < lo) lo = s[i];
    if (s[i] > hi) hi = s[i];
  }
  d = median3(s[0], s[1], s[2]);
  return hi - lo <= STILL_CM;
}

void aimAndFire(int16_t pan10, int16_t tilt10, uint16_t d) {
  int tiltComp10 = (int)(BALLISTIC_DEG_PER_CM * d * 10 + 0.5f) + TILT_NOZZLE_OFFSET10;
  int aimPan10  = constrain(pan10 + PAN_NOZZLE_OFFSET10, 0, 1800);
  int aimTilt10 = constrain(tilt10 + TILT_UP_SIGN * tiltComp10, 0, 1800);

  moveTo10(aimPan10, aimTilt10);
  digitalWrite(PIN_PUMP, HIGH);
  delay(SHOT_MS);
  digitalWrite(PIN_PUMP, LOW);
  logShot(aimPan10, aimTilt10);
  Serial.print(F("  d=")); Serial.print(d); Serial.println(F("cm"));
}

// Повертає true, якщо відпрацювали ціль (щоб продовжити скан з нової позиції)
bool engage(uint8_t t, uint8_t p) {
  // 1. Підтвердження: медіана з 3 має теж бути "переднім планом"
  uint16_t d = pingMedian3();
  if (!isForeground(d, background[t][p])) return false;

  // 2. Фільтр великих об'єктів — рука/кіт/людина
  if (objectSpan(t, p, d) >= WIDE_SPAN) {
    Serial.print(F("Big object at ")); Serial.print(d);
    Serial.println(F("cm - holding fire"));
    cooldownUntil[t] = millis() + COOLDOWN_MS;
    return true;
  }

  // 3. Точний напрям
  int16_t pan10, tilt10;
  if (!refineCentroid(t, p, d, pan10, tilt10)) {
    Serial.println(F("Lost during refine"));
    return true;
  }

  // 4. Сидить чи летить
  bool still = isStill(pan10, tilt10, d);
  Serial.print(still ? F("Target (still) at ") : F("Target (moving) at "));
  Serial.print(pan10 / 10.0f, 1); Serial.print('/'); Serial.print(tilt10 / 10.0f, 1);
  Serial.print(F(" deg, ")); Serial.print(d); Serial.println(F("cm"));
#if STATIC_ONLY
  if (!still) return true;
#endif

  if (!armed()) { Serial.println(F("[SAFE] not firing")); return true; }

  // 5. Постріли з повторною перевіркою в точці центроїду
  for (uint8_t shot = 0; shot < MAX_SHOTS; shot++) {
    aimAndFire(pan10, tilt10, d);
    moveTo10(pan10, tilt10);
    delay(150);                                  // бризки осіли, серво заспокоїлось
    uint16_t dn = pingMedian3();
    if (!sameTarget(dn, d)) { Serial.println(F("Target gone")); break; }
    d = dn;
  }
  cooldownUntil[t] = millis() + COOLDOWN_MS;
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_RECAL, INPUT_PULLUP);
  pinMode(PIN_ARM, INPUT_PULLUP);
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  pinMode(PIN_LED, OUTPUT);

  panServo.writeMicroseconds(deg10ToUs(900));    // до attach, щоб не смикнуло
  tiltServo.writeMicroseconds(deg10ToUs(tiltOf10(0)));
  panServo.attach(PIN_PAN);
  tiltServo.attach(PIN_TILT);
  curPan10 = 900; curTilt10 = tiltOf10(0);
  delay(1000);

  calibrate();
}

void loop() {
  static uint8_t t = 0, p = 0;
  static int8_t dir = +1;                        // "змійка": без великих перекидів серви

  if (digitalRead(PIN_RECAL) == LOW) { calibrate(); p = 0; t = 0; dir = +1; }

  digitalWrite(PIN_LED, armed() ? ((millis() / 250) % 2) : LOW);

  if (timeReached(cooldownUntil[t])) {
    uint16_t d = measureCell(t, p);
    if (isForeground(d, background[t][p])) engage(t, p);
  }

  // наступна клітинка змійкою
  int8_t np = p + dir;
  if (np < 0 || np >= PAN_CELLS) {
    dir = -dir;
    t = (t + 1) % TILT_CELLS;
  } else {
    p = np;
  }
}

#else
// =====================================================================
//                              КАМЕРА
// =====================================================================

const uint16_t LINK_TIMEOUT_MS = 500;   // немає команд довше — зв'язок втрачено
const uint16_t FIRE_FRESH_MS   = 150;   // дозвіл стріляти старший за це — ціль уже деінде
const uint16_t BIG_HOLD_MS     = 3000;  // після "B" мовчимо стільки
const uint16_t SHOT_GAP_MS     = 350;   // між пострілами однієї серії
const uint16_t TEST_SHOT_MAX_MS = 200;

char     lineBuf[32];
uint8_t  lineLen = 0;
bool     lineOverflow = false;

bool     wantFire = false;
uint32_t lastLinkAt = 0, lastFireReqAt = 0;
uint32_t settledAt = 0, nextShotAt = 0, bigHoldUntil = 0;
bool     pumpOn = false;
uint32_t pumpOffAt = 0;
uint8_t  burst = 0;

void startShot(uint16_t ms) {
  digitalWrite(PIN_PUMP, HIGH);
  pumpOn = true;
  pumpOffAt = millis() + ms;
}

void stopShot() {
  digitalWrite(PIN_PUMP, LOW);
  pumpOn = false;
}

// Розібрати до n цілих після літери команди. Повертає, скільки прочитано.
uint8_t parseInts(const char *s, int16_t *v, uint8_t n) {
  uint8_t k = 0;
  while (k < n) {
    while (*s == ' ') s++;
    if (!*s) break;
    char *end;
    long x = strtol(s, &end, 10);
    if (end == s) break;
    v[k++] = (int16_t)x;
    s = end;
  }
  return k;
}

void handleLine(const char *s) {
  int16_t v[3];
  const uint32_t now = millis();
  switch (s[0]) {
    case 'A':                                    // A pan10 tilt10 fire
      if (parseInts(s + 1, v, 3) != 3) return;
      if (v[0] != curPan10 || v[1] != curTilt10) settledAt = now + aim10(v[0], v[1]);
      wantFire = v[2] != 0;
      if (wantFire) lastFireReqAt = now;
      break;
    case 'M':                                    // M pan10 tilt10
      if (parseInts(s + 1, v, 2) != 2) return;
      settledAt = now + aim10(v[0], v[1]);
      wantFire = false;
      break;
    case 'F':                                    // F ms — тестовий постріл
      if (parseInts(s + 1, v, 1) != 1) return;
      if (armed() && !pumpOn && timeReached(settledAt) && timeReached(bigHoldUntil) && v[0] > 0) {
        startShot(min((uint16_t)v[0], TEST_SHOT_MAX_MS));
        logShot(curPan10, curTilt10);
      }
      break;
    case 'B':                                    // великий об'єкт у кадрі
      bigHoldUntil = now + BIG_HOLD_MS;
      wantFire = false;
      if (pumpOn) stopShot();
      break;
    default:
      return;                                    // сміття — ігноруємо, зв'язок не оновлюємо
  }
  lastLinkAt = now;
}

void readLink() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      if (!lineOverflow && lineLen) handleLine(lineBuf);
      lineLen = 0; lineOverflow = false;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineOverflow = true;                       // задовгий рядок — відкидаємо цілком
    }
  }
}

void setup() {
  Serial.begin(115200);                          // RX — від ESP32, TX — лог у USB
  pinMode(PIN_ARM, INPUT_PULLUP);
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  pinMode(PIN_LED, OUTPUT);

  panServo.writeMicroseconds(deg10ToUs(900));
  tiltServo.writeMicroseconds(deg10ToUs(900));
  panServo.attach(PIN_PAN);
  tiltServo.attach(PIN_TILT);
  Serial.println(F("FlySonar camera mode, waiting for FlyVision..."));
}

void loop() {
  readLink();
  const uint32_t now = millis();

  if (pumpOn && (timeReached(pumpOffAt) || !armed())) stopShot();

  const bool linkOk = now - lastLinkAt < LINK_TIMEOUT_MS;
  if (!linkOk) wantFire = false;
  if (!wantFire && now - lastFireReqAt > 1000) burst = 0;   // ціль пропала — нова серія

  const bool fireFresh = wantFire && now - lastFireReqAt < FIRE_FRESH_MS;
  if (fireFresh && armed() && !pumpOn && timeReached(settledAt) &&
      timeReached(nextShotAt) && timeReached(bigHoldUntil)) {
    startShot(SHOT_MS);
    logShot(curPan10, curTilt10);
    if (++burst >= MAX_SHOTS) { burst = 0; nextShotAt = now + COOLDOWN_MS; }
    else                      { nextShotAt = now + SHOT_GAP_MS; }
  }

  // LED: немає зв'язку — швидко; ARM — повільно; SAFE — горить
  if (!linkOk)      digitalWrite(PIN_LED, (now / 100) % 2);
  else if (armed()) digitalWrite(PIN_LED, (now / 250) % 2);
  else              digitalWrite(PIN_LED, HIGH);
}
#endif
