/*
 * FlySonar — ультразвукова "турель" з водометом для мух (Arduino Uno)
 *
 * Сонар HC-SR04 і сопло водомету стоять на одній pan-tilt платформі (2 серви).
 * 1) CALIBRATE: платформа проходить сітку кутів і запам'ятовує "фон" — відстань
 *    до стін/меблів у кожній клітинці (медіана з 5 вимірів).
 * 2) SCAN: платформа обходить сітку; якщо в клітинці з'явилося ехо значно ближче
 *    за фон і в робочому діапазоні — це кандидат.
 * 3) CONFIRM: повторний вимір + перевірка сусідніх кутів. Муха маленька і дає
 *    ехо в 1–2 клітинках. Якщо ехо широке (рука, кіт, людина) — НЕ стріляємо.
 * 4) FIRE: корекція прицілу (зміщення сопла + балістика струменя), імпульс помпи,
 *    повторна перевірка, до MAX_SHOTS пострілів, потім кулдаун.
 *
 * Пінаут:
 *   D2  — HC-SR04 TRIG
 *   D3  — HC-SR04 ECHO
 *   D4  — кнопка перекалібрування фону (INPUT_PULLUP, до GND)
 *   D5  — затвор MOSFET помпи (IRLZ44N)
 *   D9  — серво PAN  (горизонталь)
 *   D10 — серво TILT (вертикаль)
 *   D13 — статус-LED
 *   A0  — тумблер ARM (INPUT_PULLUP, LOW = стріляти дозволено)
 */

#include <Servo.h>

// ---------------- Геометрія сканування ----------------
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
const uint8_t  WIDE_LIMIT      = 2;      // стільки сусідів з тим самим ехо = великий об'єкт
const uint16_t FRESH_MS        = 400;    // вимір сусідньої клітинки, свіжіший за це, не повторюємо

// ---------------- Водомет ----------------
const int8_t   PAN_NOZZLE_OFFSET  = 0;     // якщо сопло збоку від сонара — поправка, град
const int8_t   TILT_NOZZLE_OFFSET = 2;     // сопло під сонаром — трохи вгору
const float    BALLISTIC_DEG_PER_CM = 0.25f; // підйом прицілу на падіння струменя
const int8_t   TILT_UP_SIGN       = +1;    // +1, якщо більший кут серви = вгору
const uint16_t SHOT_MS            = 120;
const uint8_t  MAX_SHOTS          = 3;
const uint16_t COOLDOWN_MS        = 2500;
const uint16_t SERVO_SETTLE_MS    = 25;    // на крок 5° для MG90S
const uint16_t SERVO_BIG_MOVE_MS  = 250;

// ---------------- Піни ----------------
const uint8_t PIN_TRIG = 2, PIN_ECHO = 3, PIN_RECAL = 4, PIN_PUMP = 5;
const uint8_t PIN_PAN = 9, PIN_TILT = 10, PIN_LED = 13, PIN_ARM = A0;

Servo panServo, tiltServo;
uint16_t background[TILT_CELLS][PAN_CELLS];
uint16_t lastScan[TILT_CELLS][PAN_CELLS];    // останній вимір у кожній клітинці...
uint32_t lastScanAt[TILT_CELLS][PAN_CELLS];  // ...і коли його зроблено
uint32_t cooldownUntil[TILT_CELLS];          // кулдаун по рядах (дешево по RAM)
uint32_t shotsTotal = 0;
uint8_t  curPan = 90, curTilt = 95;

// ============ Низькорівневе ============

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

// Кути в int: поправки прицілу можуть дати <0 або >180, обмежуємо до приведення в uint8_t
void moveTo(int panDeg, int tiltDeg) {
  uint8_t pan  = constrain(panDeg, 0, 180);
  uint8_t tilt = constrain(tiltDeg, 0, 180);
  uint8_t dist = max(abs((int)pan - curPan), abs((int)tilt - curTilt));
  panServo.write(pan);
  tiltServo.write(tilt);
  curPan = pan; curTilt = tilt;
  delay(dist > PAN_STEP * 2 ? SERVO_BIG_MOVE_MS : SERVO_SETTLE_MS);
}

inline uint8_t panOf(uint8_t i) { return PAN_MIN + i * PAN_STEP; }
inline bool armed() { return digitalRead(PIN_ARM) == LOW; }
inline bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }  // стійко до переповнення millis()

bool isForeground(uint16_t d, uint16_t bg) {
  if (d == NO_ECHO || d < MIN_CM || d > MAX_CM) return false;
  if (bg == NO_ECHO) return true;               // раніше там була порожнеча
  return d + DELTA_CM < bg;
}

// Навести сонар на клітинку, виміряти й запам'ятати результат
uint16_t measureCell(uint8_t t, uint8_t p) {
  moveTo(panOf(p), TILT_LEVELS[t]);
  uint16_t d = pingCm();
  lastScan[t][p]   = d;
  lastScanAt[t][p] = millis();
  return d;
}

// ============ Калібрування фону ============

void calibrate() {
  Serial.println(F("Calibrating background... keep the area clear"));
  digitalWrite(PIN_LED, HIGH);
  for (uint8_t t = 0; t < TILT_CELLS; t++) {
    for (uint8_t p = 0; p < PAN_CELLS; p++) {
      moveTo(panOf(p), TILT_LEVELS[t]);
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

// ============ Логіка цілі ============

// Скільки сусідніх кутів бачать об'єкт на тій самій відстані.
// Клітинки, які змійка щойно пройшла, беремо з lastScan — серво не мотаються
// туди-сюди. Фізично перевимірюємо лише застарілі (зазвичай ті, що попереду).
uint8_t countWideNeighbors(uint8_t t, uint8_t p, uint16_t d) {
  uint8_t n = 0;
  for (int8_t off = -2; off <= 2; off++) {
    if (off == 0) continue;
    int8_t q = p + off;
    if (q < 0 || q >= PAN_CELLS) continue;
    uint16_t dn = (millis() - lastScanAt[t][q] < FRESH_MS) ? lastScan[t][q] : measureCell(t, q);
    if (isForeground(dn, background[t][q]) && abs((int)dn - (int)d) <= SIMILAR_CM) n++;
  }
  return n;
}

void aimAndFire(uint8_t t, uint8_t p, uint16_t d) {
  int tiltComp = (int)(BALLISTIC_DEG_PER_CM * d + 0.5f) + TILT_NOZZLE_OFFSET;
  int aimPan  = constrain((int)panOf(p) + PAN_NOZZLE_OFFSET, 0, 180);
  int aimTilt = constrain((int)TILT_LEVELS[t] + TILT_UP_SIGN * tiltComp, 0, 180);

  moveTo(aimPan, aimTilt);
  digitalWrite(PIN_PUMP, HIGH);
  delay(SHOT_MS);
  digitalWrite(PIN_PUMP, LOW);
  shotsTotal++;

  Serial.print(F("PSSHT! pan=")); Serial.print(aimPan);
  Serial.print(F(" tilt="));      Serial.print(aimTilt);
  Serial.print(F(" d="));         Serial.print(d);
  Serial.print(F("cm  total="));  Serial.println(shotsTotal);
}

// Повертає true, якщо відпрацювали ціль (щоб продовжити скан з нової позиції)
bool engage(uint8_t t, uint8_t p, uint16_t firstD) {
  // 1. Підтвердження: медіана з 3 має теж бути "переднім планом"
  uint16_t d = pingMedian3();
  if (!isForeground(d, background[t][p])) return false;
  (void)firstD;

  // 2. Фільтр великих об'єктів — рука/кіт/людина
  uint8_t wide = countWideNeighbors(t, p, d);
  if (wide >= WIDE_LIMIT) {
    Serial.print(F("Big object at ")); Serial.print(d);
    Serial.println(F("cm - holding fire"));
    cooldownUntil[t] = millis() + COOLDOWN_MS;
    return true;
  }

  if (!armed()) {
    Serial.print(F("[SAFE] target at ")); Serial.print(d); Serial.println(F("cm"));
    return true;
  }

  // 3. Постріли з повторною перевіркою
  for (uint8_t shot = 0; shot < MAX_SHOTS; shot++) {
    aimAndFire(t, p, d);
    moveTo(panOf(p), TILT_LEVELS[t]);            // повернути сонар точно на клітинку
    delay(150);                                  // бризки осіли, серво заспокоїлось
    d = pingMedian3();
    if (!isForeground(d, background[t][p])) {
      Serial.println(F("Target gone"));
      break;
    }
  }
  cooldownUntil[t] = millis() + COOLDOWN_MS;
  return true;
}

// ============ setup / loop ============

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_RECAL, INPUT_PULLUP);
  pinMode(PIN_ARM, INPUT_PULLUP);
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  pinMode(PIN_LED, OUTPUT);

  panServo.attach(PIN_PAN);
  tiltServo.attach(PIN_TILT);
  moveTo(90, TILT_LEVELS[0]);
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
    if (isForeground(d, background[t][p])) {
      engage(t, p, d);
    }
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
