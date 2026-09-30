#include "Sonar.h"
#include "Base.h"

namespace fly {

const uint8_t Sonar::TILT_LEVELS[Sonar::TILT_CELLS] = {80, 95, 110};

namespace {
uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  uint16_t t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  return b;
}

inline int16_t panOf10(uint8_t i) { return (Sonar::PAN_MIN + i * Sonar::PAN_STEP) * 10; }
inline int16_t tiltOf10(uint8_t t) { return Sonar::TILT_LEVELS[t] * 10; }
inline uint16_t absDiff(uint16_t a, uint16_t b) { return a > b ? a - b : b - a; }

bool isForeground(uint16_t d, uint16_t bg) {
  if (d == Sonar::NO_ECHO || d < Sonar::MIN_CM || d > Sonar::MAX_CM) return false;
  if (bg == Sonar::NO_ECHO) return true;  // раніше там була порожнеча
  return d + Sonar::DELTA_CM < bg;
}

// Ехо від тієї самої цілі (для дрібного скану, де фону по клітинці немає)
inline bool sameTarget(uint16_t dn, uint16_t d) {
  return dn != Sonar::NO_ECHO && dn >= Sonar::MIN_CM && absDiff(dn, d) <= Sonar::SIMILAR_CM;
}
}  // namespace

Sonar::Sonar(Turret &turret, uint8_t trigPin, uint8_t echoPin, uint8_t recalPin)
    : turret_(turret), trigPin_(trigPin), echoPin_(echoPin), recalPin_(recalPin) {}

bool Sonar::begin(Base &base) {
  if (!base.claimPin(trigPin_, name()) || !base.claimPin(echoPin_, name()) || !base.claimPin(recalPin_, name()))
    return false;
  pinMode(trigPin_, OUTPUT);
  digitalWrite(trigPin_, LOW);
  pinMode(echoPin_, INPUT);
  if (recalPin_ != Base::NO_PIN) pinMode(recalPin_, INPUT_PULLUP);
  calibrating_ = true;  // калібрування — у першому loop(), коли всі модулі готові
  return true;
}

void Sonar::holdAllRows(uint32_t until) {
  for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil_[t] = until;
}

void Sonar::loop(Base &b) {
  if (!calibrated_) {
    moveTo(b, 900, tiltOf10(0));
    b.wait(1000);
    calibrate(b);
    return;
  }
  if (recalPin_ != Base::NO_PIN && digitalRead(recalPin_) == LOW) {
    calibrate(b);
    p_ = 0;
    t_ = 0;
    dir_ = +1;
  }

  if (reached(millis(), cooldownUntil_[t_])) {
    uint16_t d = measureCell(b, t_, p_);
    if (isForeground(d, background_[t_][p_])) engage(b, t_, p_);
  }

  // наступна клітинка змійкою
  int8_t np = p_ + dir_;
  if (np < 0 || np >= PAN_CELLS) {
    dir_ = -dir_;
    t_ = (t_ + 1) % TILT_CELLS;
  } else {
    p_ = np;
  }
}

// ---------------- Вимірювання ----------------

uint16_t Sonar::pingCm(Base &b) {
  const uint32_t since = millis() - lastPing_;
  if (since < PING_GAP_MS) b.wait(PING_GAP_MS - since);  // чекаємо затухання відлуння

  // Якщо попередній пінг не отримав ехо, сенсор ще тримає ECHO у HIGH і проігнорує
  // новий тригер. Дочекаємось LOW, інакше кожен другий вимір губиться.
  const uint32_t waitStart = millis();
  while (digitalRead(echoPin_) == HIGH) {
    if (millis() - waitStart > ECHO_IDLE_TIMEOUT_MS) {  // сенсор завис/відключений
      lastPing_ = millis();
      return NO_ECHO;
    }
    b.idle();
  }
  lastPing_ = millis();

  digitalWrite(trigPin_, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin_, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin_, LOW);
  const unsigned long us = pulseIn(echoPin_, HIGH, ECHO_TIMEOUT_US);
  return us == 0 ? NO_ECHO : us / 58;
}

uint16_t Sonar::pingMedian3(Base &b) {
  uint16_t a = pingCm(b), c = pingCm(b), d = pingCm(b);
  return median3(a, c, d);
}

void Sonar::moveTo(Base &b, int pan10, int tilt10) { b.wait(turret_.moveAndSettleMs(pan10, tilt10)); }

uint16_t Sonar::measureCell(Base &b, uint8_t t, uint8_t p) {
  moveTo(b, panOf10(p), tiltOf10(t));
  uint16_t d = pingCm(b);
  lastScan_[t][p] = d;
  lastScanAt_[t][p] = millis();
  return d;
}

void Sonar::calibrate(Base &b) {
  calibrating_ = true;
  b.log().println(F("Calibrating background... keep the area clear"));
  for (uint8_t t = 0; t < TILT_CELLS; t++) {
    for (uint8_t p = 0; p < PAN_CELLS; p++) {
      moveTo(b, panOf10(p), tiltOf10(t));
      uint16_t s[5];
      for (uint8_t k = 0; k < 5; k++) s[k] = pingCm(b);
      for (uint8_t i = 1; i < 5; i++) {  // медіана з 5 (вставками)
        uint16_t v = s[i];
        int8_t j = i - 1;
        while (j >= 0 && s[j] > v) {
          s[j + 1] = s[j];
          j--;
        }
        s[j + 1] = v;
      }
      background_[t][p] = s[2];
      lastScan_[t][p] = s[2];
      lastScanAt_[t][p] = millis() - FRESH_MS;  // одразу "несвіжий"
    }
    cooldownUntil_[t] = millis();
  }
  calibrated_ = true;
  calibrating_ = false;
  b.log().println(F("Background ready"));
}

// Ширина об'єкта: скільки клітинок поспіль (у обидва боки від p) бачать його на
// тій самій відстані. Рахуємо суцільну смугу, бо скан часто натрапляє на великий
// об'єкт з краю — симетрична перевірка сусідів тоді бачить лише половину.
// Клітинки, які змійка щойно пройшла, беремо з lastScan_ — серво не мотаються
// туди-сюди. Фізично перевимірюємо лише застарілі (зазвичай ті, що попереду).
uint8_t Sonar::objectSpan(Base &b, uint8_t t, uint8_t p, uint16_t d) {
  uint8_t span = 1;
  for (int8_t dir = -1; dir <= 1; dir += 2) {
    for (int8_t k = 1; k <= WIDE_RANGE; k++) {
      int8_t q = p + dir * k;
      if (q < 0 || q >= PAN_CELLS) break;
      uint16_t dn = (millis() - lastScanAt_[t][q] < FRESH_MS) ? lastScan_[t][q] : measureCell(b, t, q);
      if (!isForeground(dn, background_[t][q]) || absDiff(dn, d) > SIMILAR_CM) break;
      span++;
    }
  }
  return span;
}

// Дрібний скан однієї осі навколо center10. shift — зсув центроїду (0.1°);
// false, якщо влучань замало. Пелюстка симетрична, тому середнє кутів з ехо —
// це напрям на ціль. Якщо ехо є на краю вікна, вікно обрізало пелюстку з одного
// боку і середнє зміщене — пересуваємо вікно й повторюємо.
bool Sonar::fineAxis(Base &b, bool panAxis, int16_t center10, int16_t other10, uint16_t d, int16_t &shift) {
  shift = 0;
  for (uint8_t pass = 0; pass < FINE_PASSES; pass++) {
    int32_t sum = 0;
    uint8_t hits = 0;
    bool edge = false;
    for (int16_t off = -FINE_SPAN10; off <= FINE_SPAN10; off += FINE_STEP10) {
      if (panAxis) moveTo(b, center10 + shift + off, other10);
      else moveTo(b, other10, center10 + shift + off);
      if (sameTarget(pingCm(b), d)) {
        sum += off;
        hits++;
        if (off == -FINE_SPAN10 || off + FINE_STEP10 > FINE_SPAN10) edge = true;
      }
    }
    if (hits < FINE_MIN_HITS) return pass > 0;
    int16_t delta = (int16_t)(sum / hits);
    shift += delta;
    if (!edge || abs(delta) < FINE_STEP10 / 2) break;  // вікно вже накриває пелюстку
  }
  return true;
}

bool Sonar::refineCentroid(Base &b, uint8_t t, uint8_t p, uint16_t d, int16_t &pan10, int16_t &tilt10) {
  pan10 = panOf10(p);
  tilt10 = tiltOf10(t);
  int16_t shift;
  if (!fineAxis(b, true, pan10, tilt10, d, shift)) return false;
  pan10 += shift;
  if (fineAxis(b, false, tilt10, pan10, d, shift)) tilt10 += shift;  // по вертикалі може не вистачити — лишаємо рядок
  return true;
}

// Ціль "сидить": усі виміри є і розкид відстані малий. d — оновлюється медіаною.
bool Sonar::isStill(Base &b, int16_t pan10, int16_t tilt10, uint16_t &d) {
  moveTo(b, pan10, tilt10);
  uint16_t lo = 0xFFFF, hi = 0, s[STILL_SAMPLES];
  for (uint8_t i = 0; i < STILL_SAMPLES; i++) {
    s[i] = pingCm(b);
    if (!sameTarget(s[i], d)) return false;
    if (s[i] < lo) lo = s[i];
    if (s[i] > hi) hi = s[i];
  }
  d = median3(s[0], s[1], s[2]);
  return hi - lo <= STILL_CM;
}

// Попросити турель вистрілити в точку й дочекатися кінця пострілу.
// false — турель не стріляє (SAFE, кулдаун серії, великий об'єкт).
bool Sonar::fireAt(Base &b, int16_t pan10, int16_t tilt10) {
  const uint32_t s0 = turret_.shots(), t0 = millis();
  while (turret_.shots() == s0) {
    if (millis() - t0 > 800) return false;  // > доворот серв + пауза між пострілами серії
    turret_.aim(pan10, tilt10, true);
    b.wait(2);
  }
  while (turret_.valveOn()) b.wait(1);
  turret_.printShots();
  return true;
}

// true — ціль відпрацьовано (скан продовжується з нової позиції)
bool Sonar::engage(Base &b, uint8_t t, uint8_t p) {
  Print &log = b.log();

  // 1. Підтвердження: медіана з 3 має теж бути "переднім планом"
  uint16_t d = pingMedian3(b);
  if (!isForeground(d, background_[t][p])) return false;

  // 2. Фільтр великих об'єктів — рука/кіт/людина
  if (objectSpan(b, t, p, d) >= WIDE_SPAN) {
    log.print(F("Big object at "));
    log.print(d);
    log.println(F("cm - holding fire"));
    turret_.bigObject();
    cooldownUntil_[t] = millis() + COOLDOWN_MS;
    return true;
  }

  // 3. Точний напрям
  int16_t pan10, tilt10;
  if (!refineCentroid(b, t, p, d, pan10, tilt10)) {
    log.println(F("Lost during refine"));
    return true;
  }

  // 4. Сидить чи летить
  const bool still = isStill(b, pan10, tilt10, d);
  log.print(still ? F("Target (still) at ") : F("Target (moving) at "));
  log.print(pan10 / 10.0f, 1);
  log.print('/');
  log.print(tilt10 / 10.0f, 1);
  log.print(F(" deg, "));
  log.print(d);
  log.println(F("cm"));
  if (staticOnly && !still) return true;

  if (!b.armed()) {
    log.println(F("[SAFE] not firing"));
    return true;
  }

  // 5. Постріли з поправкою на сопло й балістику та перевіркою в точці центроїду
  for (uint8_t shot = 0; shot < MAX_SHOTS; shot++) {
    const int16_t lift10 = turret_.nozzle().ballisticLift10(d) + TILT_NOZZLE_OFFSET10;
    if (!fireAt(b, PanTilt::clamp10(pan10 + PAN_NOZZLE_OFFSET10), PanTilt::clamp10(tilt10 + TILT_UP_SIGN * lift10))) {
      log.println(F("Turret holds fire"));
      break;
    }
    log.print(F("  d="));
    log.print(d);
    log.println(F("cm"));
    moveTo(b, pan10, tilt10);
    b.wait(150);  // бризки осіли, серво заспокоїлось
    uint16_t dn = pingMedian3(b);
    if (!sameTarget(dn, d)) {
      log.println(F("Target gone"));
      break;
    }
    d = dn;
  }
  cooldownUntil_[t] = millis() + COOLDOWN_MS;
  return true;
}

}  // namespace fly
