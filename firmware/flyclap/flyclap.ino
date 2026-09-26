/*
 * FlyClap — автоматична "хлопавка" для мух на Arduino Uno (ATmega328P)
 *
 * Принцип: між двома пружинними пластинами проходить ІЧ-завіса з 4 променів.
 * Муха перетинає промінь → прямо в ISR подаємо імпульс на соленоїд-засувку →
 * пружини схлопують пластини (~15–25 мс) → сервопривід знову взводить механізм.
 *
 * Пінаут:
 *   D4..D7  — входи з компаратора LM339 (промінь цілий = LOW, перекритий = HIGH)
 *   D8      — кінцевик "взведено" (INPUT_PULLUP, LOW = засувка зачеплена)
 *   D9      — затвор MOSFET соленоїда (IRLZ44N)
 *   D10     — сервопривід взведення (MG996R)
 *   D11     — п'єзо-бузер
 *   D13     — статусний світлодіод
 *   A0      — тумблер ARM (INPUT_PULLUP, LOW = озброєно)
 */

#include <Servo.h>
#include <EEPROM.h>

// ---------------- Налаштування ----------------
const uint8_t  BEAM_MASK          = 0b11110000;  // PD4..PD7
const uint16_t SOLENOID_PULSE_MS  = 40;    // час утримання засувки відкритою
const uint16_t CLAP_SETTLE_MS     = 300;   // чекаємо, поки пластини схлопнуться і заспокояться
const uint8_t  SERVO_REST_DEG     = 10;    // важіль відведений, не заважає пластинам
const uint8_t  SERVO_COCK_DEG     = 150;   // важіль розводить пластини до зачеплення засувки
const uint16_t COCK_TIMEOUT_MS    = 1500;  // не дочекались кінцевика → FAULT
const uint16_t SERVO_RETURN_MS    = 400;
const uint16_t BEAMS_CLEAR_MS     = 500;   // завіса має бути чистою стільки часу перед ARM
const uint16_t BEAMS_STUCK_MS     = 3000;  // довше перекрито (прилипла муха/сміття) → FAULT
const uint8_t  CONFIRM_US         = 30;    // повторне читання в ISR проти імпульсних завад

const uint8_t PIN_COCKED   = 8;
const uint8_t PIN_SOLENOID = 9;   // PB1
const uint8_t PIN_SERVO    = 10;
const uint8_t PIN_BUZZER   = 11;
const uint8_t PIN_LED      = 13;
const uint8_t PIN_ARM      = A0;

const int EEPROM_ADDR_KILLS = 0;

// ---------------- Стан ----------------
enum State : uint8_t { DISARMED, WAIT_CLEAR, ARMED, FIRED, COCKING, SERVO_RETURN, FAULT };

volatile bool     g_armedIsr   = false;  // дозвіл стріляти прямо з ISR
volatile bool     g_fired      = false;
volatile uint32_t g_fireMillis = 0;

State    state = DISARMED;
uint32_t stateSince = 0;
uint32_t clearSince = 0;
uint32_t blockedSince = 0;
uint32_t kills = 0;
Servo    cockServo;

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
  PORTD &= ~BEAM_MASK;                          // без внутрішніх підтяжок (підтяжки на платі компаратора)
  PCICR  |= _BV(PCIE2);                         // група PCINT16..23 (порт D)
  PCMSK2 |= _BV(PCINT20) | _BV(PCINT21) | _BV(PCINT22) | _BV(PCINT23);
}

// ---------------- Допоміжне ----------------
void beep(uint16_t f, uint16_t ms) { tone(PIN_BUZZER, f, ms); }

void saveKills() {
  EEPROM.put(EEPROM_ADDR_KILLS, kills);
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
  cockServo.write(SERVO_REST_DEG);
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

  cockServo.attach(PIN_SERVO);
  cockServo.write(SERVO_REST_DEG);

  EEPROM.get(EEPROM_ADDR_KILLS, kills);
  if (kills == 0xFFFFFFFF) kills = 0;           // чиста EEPROM

  setupBeamInterrupts();

  Serial.print(F("FlyClap ready. Total kills: "));
  Serial.println(kills);
  enterState(DISARMED);
}

void loop() {
  const uint32_t now = millis();

  // Тумблер ARM вимкнено — все зупиняємо в будь-якому стані (FAULT скидається так само)
  if (!armSwitchOn() && state != DISARMED) {
    g_armedIsr = false;
    solenoidOff();
    cockServo.write(SERVO_REST_DEG);
    Serial.println(F("Disarmed"));
    enterState(DISARMED);
  }

  // Страховка: соленоїд ніколи не тримаємо довше імпульсу (захист від перегріву)
  if (g_fired && (now - g_fireMillis) >= SOLENOID_PULSE_MS) solenoidOff();

  switch (state) {
    case DISARMED:
      if (armSwitchOn()) {
        beep(2000, 80);
        clearSince = now;
        enterState(isCocked() ? WAIT_CLEAR : COCKING);
        if (state == COCKING) cockServo.write(SERVO_COCK_DEG);
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
      if (g_fired) {
        kills++;
        saveKills();
        Serial.print(F("CLAP! #"));
        Serial.println(kills);
        enterState(FIRED);
      } else if (!isCocked()) {
        // засувка зірвалась сама (вібрація) — перевзводимо
        g_armedIsr = false;
        Serial.println(F("Latch lost, re-cocking"));
        cockServo.write(SERVO_COCK_DEG);
        enterState(COCKING);
      }
      break;

    case FIRED:
      if (now - stateSince >= CLAP_SETTLE_MS) {
        solenoidOff();
        cockServo.write(SERVO_COCK_DEG);
        enterState(COCKING);
      }
      break;

    case COCKING:
      if (isCocked()) {
        cockServo.write(SERVO_REST_DEG);
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
