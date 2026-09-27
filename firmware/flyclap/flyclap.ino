/*
 * FlyClap — автоматична "хлопавка" для мух на Arduino Uno (ATmega328P)
 *
 * Принцип: між двома пружинними пластинами проходить ІЧ-завіса з 4 променів.
 * Муха перетинає промінь → прямо в ISR подаємо імпульс на соленоїд-засувку →
 * пружини схлопують пластини (~15–25 мс) → сервопривід знову взводить механізм.
 *
 * Пінаут:
 *   D3      — (лише BEAM_SENSOR_TSSP) несуча 38 кГц на ключ ІЧ-світлодіодів
 *   D4..D7  — входи завіси (промінь цілий = LOW, перекритий = HIGH):
 *             LM339 з гістерезисом, або TSSP4038 (BEAM_SENSOR_TSSP = 1)
 *   D8      — кінцевик "взведено" (INPUT_PULLUP, LOW = засувка зачеплена)
 *   D9      — затвор MOSFET соленоїда (IRLZ44N)
 *   D10     — сервопривід взведення (MG996R)
 *   D11     — п'єзо-бузер (пасивний; для TSSP-варіанта — активний)
 *   D13     — статусний світлодіод
 *   A0      — тумблер ARM (INPUT_PULLUP, LOW = озброєно)
 */

#include <Servo.h>
#include <EEPROM.h>
#include <util/atomic.h>

// ---------------- Варіант сенсора ----------------
// 0 — фототранзистори + LM339 (базова схема з docs/flyclap.md)
// 1 — модульовані приймачі TSSP4038: Timer2 генерує 38 кГц на D3, LM339 і
//     підстроювальники не потрібні, завіса не боїться сонця. Timer2 зайнятий,
//     тому tone() недоступний і на D11 ставиться АКТИВНИЙ бузер.
#ifndef BEAM_SENSOR_TSSP
#define BEAM_SENSOR_TSSP 0
#endif

// ---------------- Налаштування ----------------
const uint8_t  BEAM_MASK          = 0b11110000;  // PD4..PD7
const uint16_t SOLENOID_PULSE_MS  = 40;    // час утримання засувки відкритою
const uint16_t CLAP_SETTLE_MS     = 300;   // чекаємо, поки пластини схлопнуться і заспокояться
const uint8_t  SERVO_REST_DEG     = 10;    // важіль відведений, не заважає пластинам
const uint8_t  SERVO_COCK_DEG     = 150;   // важіль розводить пластини до зачеплення засувки
const uint16_t COCK_TIMEOUT_MS    = 1500;  // не дочекались кінцевика → FAULT
const uint16_t SERVO_RETURN_MS    = 400;   // час на відведення важеля; потім серво відключаємо
const uint16_t BEAMS_CLEAR_MS     = 500;   // завіса має бути чистою стільки часу перед ARM
const uint16_t BEAMS_STUCK_MS     = 3000;  // довше перекрито (прилипла муха/сміття) → FAULT
const uint8_t  CONFIRM_US         = 30;    // повторне читання в ISR проти імпульсних завад

const uint8_t PIN_COCKED   = 8;
const uint8_t PIN_SOLENOID = 9;   // PB1
const uint8_t PIN_SERVO    = 10;
const uint8_t PIN_BUZZER   = 11;
const uint8_t PIN_LED      = 13;
const uint8_t PIN_ARM      = A0;

#if BEAM_SENSOR_TSSP
const uint8_t PIN_IR_CARRIER = 3;  // OC2B
#endif

const int EEPROM_ADDR_CLAPS = 0;

// ---------------- Стан ----------------
enum State : uint8_t { DISARMED, WAIT_CLEAR, ARMED, FIRED, COCKING, SERVO_RETURN, FAULT };

volatile bool     g_armedIsr   = false;  // дозвіл стріляти прямо з ISR
volatile bool     g_fired      = false;
volatile uint32_t g_fireMillis = 0;

State    state = DISARMED;
uint32_t stateSince = 0;
uint32_t clearSince = 0;
uint32_t blockedSince = 0;
uint32_t claps = 0;         // спрацювання, а не підтверджені мухи (пил теж рахується)
Servo    cockServo;
uint8_t  servoTarget = SERVO_REST_DEG;
uint32_t servoMovedAt = 0;
#if BEAM_SENSOR_TSSP
uint32_t buzzerOffAt = 0;
#endif

inline void solenoidOn()  { PORTB |=  _BV(PB1); }
inline void solenoidOff() { PORTB &= ~_BV(PB1); }
inline bool beamsBlocked() { return (PIND & BEAM_MASK) != 0; }
inline bool isCocked()     { return digitalRead(PIN_COCKED) == LOW; }
inline bool armSwitchOn()  { return digitalRead(PIN_ARM) == LOW; }

void enterState(State s) {
  state = s;
  stateSince = millis();
}

// ---------------- ISR: мінімальна латентність спрацювання ----------------
ISR(PCINT2_vect) {
  if (!g_armedIsr) return;
  if ((PIND & BEAM_MASK) == 0) return;          // це був фронт відновлення променя
  delayMicroseconds(CONFIRM_US);                // короткий антидребезг проти ЕМ-завад
  if ((PIND & BEAM_MASK) == 0) return;

  solenoidOn();                                 // стріляємо одразу, без очікування loop()
  g_armedIsr   = false;
  g_fireMillis = millis();
  g_fired      = true;
}

void setupBeamInterrupts() {
  DDRD  &= ~BEAM_MASK;                          // входи
#if BEAM_SENSOR_TSSP
  PORTD |=  BEAM_MASK;                          // TSSP має слабку власну підтяжку — додаємо внутрішню
#else
  PORTD &= ~BEAM_MASK;                          // без внутрішніх підтяжок (підтяжки на платі компаратора)
#endif
  PCICR  |= _BV(PCIE2);                         // група PCINT16..23 (порт D)
  PCMSK2 |= _BV(PCINT20) | _BV(PCINT21) | _BV(PCINT22) | _BV(PCINT23);
}

#if BEAM_SENSOR_TSSP
// Timer2, Fast PWM з TOP = OCR2A: 16 МГц / 8 / (52 + 1) ≈ 37.7 кГц, шпаруватість ~50 % на OC2B (D3)
void setupIrCarrier() {
  pinMode(PIN_IR_CARRIER, OUTPUT);
  TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(WGM22) | _BV(CS21);
  OCR2A  = 52;
  OCR2B  = 26;
}
#endif

// ---------------- Допоміжне ----------------
#if BEAM_SENSOR_TSSP
// Активний бузер: частота фіксована, f ігнорується; вимикається з loop()
void beep(uint16_t, uint16_t ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerOffAt = millis() + ms;
}
#else
void beep(uint16_t f, uint16_t ms) { tone(PIN_BUZZER, f, ms); }
#endif

// Серво живе лише під час руху: у спокої відключене — не тремтить, не гуде,
// не їсть струм і не наводить завади на завісу.
void servoTo(uint8_t deg) {
  cockServo.write(deg);                         // спершу кут, щоб attach() не смикнув у 90°
  if (!cockServo.attached()) cockServo.attach(PIN_SERVO);
  servoTarget  = deg;
  servoMovedAt = millis();
}

void servoIdleDetach(uint32_t now) {
  if (cockServo.attached() && servoTarget == SERVO_REST_DEG &&
      now - servoMovedAt >= SERVO_RETURN_MS) {
    cockServo.detach();
    digitalWrite(PIN_SERVO, LOW);               // detach() посеред імпульсу лишає пін у HIGH
  }
}

void saveClaps() {
  EEPROM.put(EEPROM_ADDR_CLAPS, claps);         // put() пише лише змінені байти
}

void updateLed() {
  switch (state) {
    case ARMED:    digitalWrite(PIN_LED, HIGH); break;
    case FAULT:    digitalWrite(PIN_LED, (millis() / 100) % 2); break;   // швидке блимання
    case DISARMED: digitalWrite(PIN_LED, LOW); break;
    default:       digitalWrite(PIN_LED, (millis() / 400) % 2); break;   // повільне — зайнятий
  }
}

void fault(const __FlashStringHelper *why) {
  g_armedIsr = false;
  solenoidOff();
  servoTo(SERVO_REST_DEG);
  Serial.print(F("FAULT: "));
  Serial.println(why);
  beep(400, 600);
  enterState(FAULT);
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_SOLENOID, OUTPUT);
  solenoidOff();
  pinMode(PIN_COCKED, INPUT_PULLUP);
  pinMode(PIN_ARM, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  digitalWrite(PIN_BUZZER, LOW);

  servoTo(SERVO_REST_DEG);                      // відведе важіль і сама відключиться

  EEPROM.get(EEPROM_ADDR_CLAPS, claps);
  if (claps == 0xFFFFFFFF) claps = 0;           // чиста EEPROM

#if BEAM_SENSOR_TSSP
  setupIrCarrier();
  delay(50);                                    // АРУ приймачів встановлюється на несучу
#endif
  setupBeamInterrupts();

  Serial.print(F("FlyClap ready. Total claps: "));
  Serial.println(claps);
  enterState(DISARMED);
}

void loop() {
  const uint32_t now = millis();

  // Тумблер ARM вимкнено — все зупиняємо в будь-якому стані (FAULT скидається так само)
  if (!armSwitchOn() && state != DISARMED) {
    g_armedIsr = false;
    solenoidOff();
    servoTo(SERVO_REST_DEG);
    Serial.println(F("Disarmed"));
    enterState(DISARMED);
  }

  // Страховка: соленоїд ніколи не тримаємо довше імпульсу (захист від перегріву)
  bool     fired;
  uint32_t fireMillis;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {           // 32-бітна змінна з ISR — читаємо атомарно
    fired      = g_fired;
    fireMillis = g_fireMillis;
  }
  if (fired && (now - fireMillis) >= SOLENOID_PULSE_MS) solenoidOff();

  servoIdleDetach(now);
#if BEAM_SENSOR_TSSP
  if (buzzerOffAt && (int32_t)(now - buzzerOffAt) >= 0) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOffAt = 0;
  }
#endif

  switch (state) {
    case DISARMED:
      if (armSwitchOn()) {
        beep(2000, 80);
        clearSince = now;
        enterState(isCocked() ? WAIT_CLEAR : COCKING);
        if (state == COCKING) servoTo(SERVO_COCK_DEG);
      }
      break;

    case WAIT_CLEAR:
      // Озброюємося лише коли завіса стабільно чиста
      if (beamsBlocked()) {
        clearSince = now;
        if (blockedSince == 0) blockedSince = now;
        if (now - blockedSince > BEAMS_STUCK_MS) {
          blockedSince = 0;
          fault(F("beam blocked too long - clean the gap"));
        }
      } else {
        blockedSince = 0;
        if (now - clearSince >= BEAMS_CLEAR_MS) {
          noInterrupts();
          g_fired = false;
          g_armedIsr = true;
          interrupts();
          beep(3000, 30);
          Serial.println(F("ARMED"));
          enterState(ARMED);
        }
      }
      break;

    case ARMED:
      if (fired) {
        claps++;
        saveClaps();
        Serial.print(F("CLAP! #"));
        Serial.println(claps);
        enterState(FIRED);
      } else if (!isCocked()) {
        // засувка зірвалась сама (вібрація) — перевзводимо
        g_armedIsr = false;
        Serial.println(F("Latch lost, re-cocking"));
        servoTo(SERVO_COCK_DEG);
        enterState(COCKING);
      }
      break;

    case FIRED:
      if (now - stateSince >= CLAP_SETTLE_MS) {
        solenoidOff();
        servoTo(SERVO_COCK_DEG);
        enterState(COCKING);
      }
      break;

    case COCKING:
      if (isCocked()) {
        servoTo(SERVO_REST_DEG);
        enterState(SERVO_RETURN);
      } else if (now - stateSince > COCK_TIMEOUT_MS) {
        fault(F("cocking timeout - check latch/servo"));
      }
      break;

    case SERVO_RETURN:
      if (now - stateSince >= SERVO_RETURN_MS) {
        clearSince = now;
        blockedSince = 0;
        enterState(WAIT_CLEAR);
      }
      break;

    case FAULT:
      // вихід тільки через вимкнення тумблера ARM (оброблено вище)
      break;
  }

  updateLed();
}
