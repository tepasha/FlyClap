#include "ClapLatch.h"
#include <EEPROM.h>
#include "Base.h"

#if defined(__AVR__)
#include <util/atomic.h>
#endif

namespace fly {

ClapLatch::ClapLatch(Trigger &trigger, uint8_t solenoidPin, uint8_t servoPin, uint8_t cockedPin, int eepromAddr)
    : trigger_(trigger), solenoidPin_(solenoidPin), cockedPin_(cockedPin), eepromAddr_(eepromAddr), servo_(servoPin) {}

bool ClapLatch::begin(Base &base) {
  if (!base.claimPin(solenoidPin_, name()) || !base.claimPin(cockedPin_, name()) || !servo_.begin(base, name()))
    return false;
  pinMode(solenoidPin_, OUTPUT);
#if defined(__AVR__)
  solPort_ = portOutputRegister(digitalPinToPort(solenoidPin_));
  solMask_ = digitalPinToBitMask(solenoidPin_);
#endif
  solenoid(false);
  pinMode(cockedPin_, INPUT_PULLUP);
  trigger_.setHandler(&ClapLatch::onCross, this);

#if defined(ESP32)
  EEPROM.begin(64);
#endif
  EEPROM.get(eepromAddr_, claps_);
  if (claps_ == 0xFFFFFFFFUL) claps_ = 0;  // чиста EEPROM

  servoTo(SERVO_REST_DEG, millis());  // відведе важіль і саме відключиться
  base.log().print(F("FlyClap ready. Total claps: "));
  base.log().println(claps_);
  return true;
}

// Переривання: стріляємо одразу, без очікування головного циклу.
void ClapLatch::onCross(void *self) {
  ClapLatch *c = static_cast<ClapLatch *>(self);
#if defined(__AVR__)
  *c->solPort_ |= c->solMask_;  // в ISR переривання вже вимкнені — запис атомарний
#else
  digitalWrite(c->solenoidPin_, HIGH);
#endif
  c->fireMillis_ = millis();
  c->fired_ = true;
}

void ClapLatch::solenoid(bool on) {
#if defined(__AVR__)
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {  // той самий порт пише ISR
    if (on) *solPort_ |= solMask_;
    else *solPort_ &= ~solMask_;
  }
#else
  digitalWrite(solenoidPin_, on ? HIGH : LOW);
#endif
}

void ClapLatch::enter(State s, uint32_t now) {
  state_ = s;
  stateSince_ = now;
}

void ClapLatch::servoTo(uint8_t deg, uint32_t now) {
  servo_.writeDeg(deg);
  servoTarget_ = deg;
  servoMovedAt_ = now;
}

void ClapLatch::fault(Base &base, const __FlashStringHelper *why, uint32_t now) {
  trigger_.cancelShot();
  solenoid(false);
  servoTo(SERVO_REST_DEG, now);
  base.log().print(F("FAULT: "));
  base.log().println(why);
  base.beep(400, 600);
  enter(FAULT, now);
}

void ClapLatch::disarm(Base &) {
  trigger_.cancelShot();
  solenoid(false);
  servoTo(SERVO_REST_DEG, millis());
  enter(DISARMED, millis());
}

Health ClapLatch::health() const {
  switch (state_) {
    case FAULT: return Health::Fault;
    case ARMED:
    case DISARMED: return Health::Ok;
    default: return Health::Busy;
  }
}

void ClapLatch::tick(Base &base, uint32_t now) {
  // Знімок з переривання: 32-бітна змінна — читаємо атомарно
  bool fired;
  uint32_t fireMillis;
#if defined(__AVR__)
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    fired = fired_;
    fireMillis = fireMillis_;
  }
#else
  fired = fired_;
  fireMillis = fireMillis_;
#endif

  // Страховка: соленоїд ніколи не тримаємо довше імпульсу (захист від перегріву)
  if (fired && now - fireMillis >= SOLENOID_PULSE_MS) solenoid(false);

  // Серво живе лише під час руху
  if (servo_.attached() && servoTarget_ == SERVO_REST_DEG && now - servoMovedAt_ >= SERVO_RETURN_MS)
    servo_.detach();

  switch (state_) {
    case DISARMED:
      if (base.armed()) {
        base.beep(2000, 80);
        clearSince_ = now;
        if (cocked()) {
          enter(WAIT_CLEAR, now);
        } else {
          servoTo(SERVO_COCK_DEG, now);
          enter(COCKING, now);
        }
      }
      break;

    case WAIT_CLEAR:  // озброюємося лише коли завіса стабільно чиста
      if (trigger_.blocked()) {
        clearSince_ = now;
        if (!blocked_) {
          blocked_ = true;
          blockedSince_ = now;
        }
        if (now - blockedSince_ > BEAMS_STUCK_MS) {
          blocked_ = false;
          fault(base, F("beam blocked too long - clean the gap"), now);
        }
      } else {
        blocked_ = false;
        if (now - clearSince_ >= BEAMS_CLEAR_MS) {
#if defined(__AVR__)
          ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            fired_ = false;
            trigger_.enableShot();
          }
#else
          fired_ = false;
          trigger_.enableShot();
#endif
          base.beep(3000, 30);
          base.log().println(F("ARMED"));
          enter(ARMED, now);
        }
      }
      break;

    case ARMED:
      if (fired) {
        claps_++;
        EEPROM.put(eepromAddr_, claps_);  // put() пише лише змінені байти
#if defined(ESP32)
        EEPROM.commit();
#endif
        base.log().print(F("CLAP! #"));
        base.log().println(claps_);
        enter(FIRED, now);
      } else if (!cocked()) {
        // засувка зірвалась сама (вібрація) — перевзводимо
        trigger_.cancelShot();
        base.log().println(F("Latch lost, re-cocking"));
        servoTo(SERVO_COCK_DEG, now);
        enter(COCKING, now);
      }
      break;

    case FIRED:
      if (now - stateSince_ >= CLAP_SETTLE_MS) {
        solenoid(false);
        servoTo(SERVO_COCK_DEG, now);
        enter(COCKING, now);
      }
      break;

    case COCKING:
      if (cocked()) {
        servoTo(SERVO_REST_DEG, now);
        enter(SERVO_RETURN, now);
      } else if (now - stateSince_ > COCK_TIMEOUT_MS) {
        fault(base, F("cocking timeout - check latch/servo"), now);
      }
      break;

    case SERVO_RETURN:
      if (now - stateSince_ >= SERVO_RETURN_MS) {
        clearSince_ = now;
        blocked_ = false;
        enter(WAIT_CLEAR, now);
      }
      break;

    case FAULT:  // вихід тільки через вимкнення ARM (disarm)
      break;
  }
}

}  // namespace fly
