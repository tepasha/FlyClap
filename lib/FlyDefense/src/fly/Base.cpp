#include "Base.h"

namespace fly {

Base::Base(uint8_t armPin, uint8_t ledPin, bool ledActiveLow)
    : armPin_(armPin), ledPin_(ledPin), ledActiveLow_(ledActiveLow) {}

void Base::buzzer(uint8_t pin, bool active) {
  buzzerPin_ = pin;
  buzzerActive_ = active;
}

void Base::add(Module &m) {
  if (started_) return;  // модулі додаються лише до begin()
  Module **p = &first_;
  while (*p) p = &(*p)->next_;
  *p = &m;
  m.next_ = nullptr;
}

bool Base::begin(unsigned long baud) {
  Serial.begin(baud);
#if defined(__AVR__)
  claimPin(0, F("Serial"));  // D0/D1 — USB-лог
  claimPin(1, F("Serial"));
#endif
  claimPin(armPin_, F("ARM"));
  claimPin(ledPin_, F("LED"));
  if (armPin_ == NO_PIN) log().println(F("No ARM switch on this board: fine for eyes/sensor boards, never for shooters"));
  else pinMode(armPin_, INPUT_PULLUP);
  if (ledPin_ != NO_PIN) {
    pinMode(ledPin_, OUTPUT);
    digitalWrite(ledPin_, ledActiveLow_ ? HIGH : LOW);
  }
  if (buzzerPin_ != NO_PIN && !claimPin(buzzerPin_, F("buzzer"))) buzzerPin_ = NO_PIN;
  if (buzzerPin_ != NO_PIN) {
    pinMode(buzzerPin_, OUTPUT);
    digitalWrite(buzzerPin_, LOW);
    if (!buzzerActive_) {
#if defined(ESP32)
      // tone() на ESP32 бере LEDC-канал, яким тактується камера
      configError(F("buzzer"), F("passive buzzer is not supported on ESP32, use an active one"));
#elif defined(__AVR__)
      claim(Res::Timer2, F("buzzer (tone)"));
#endif
    }
  }

  for (Module *m = first_; m; m = m->next_)
    if (!m->begin(*this)) configError(m->name(), F("begin() failed"));

  if (configError_) log().println(F("Device stays SAFE until the configuration is fixed"));
  started_ = true;

#if defined(ESP32)
  mutex_ = xSemaphoreCreateRecursiveMutex();
  // Ядро 0: камера й loop() живуть на ядрі 1 (на одноядерних — просто окрема задача)
  xTaskCreatePinnedToCore(realtimeTask, "fly_rt", 4096, this, 3, nullptr, 0);
#endif
  return !configError_;
}

#if defined(ESP32)
void Base::realtimeTask(void *self) {
  Base &b = *static_cast<Base *>(self);
  for (;;) {
    {
      Lock l(b);
      b.tick(millis());
    }
    vTaskDelay(1);  // тік FreeRTOS в arduino-esp32 — 1 мс
  }
}
#endif

void Base::update() {
#if !defined(ESP32)
  tick(millis());
#endif
  if (configError_) return;
  for (Module *m = first_; m; m = m->next_) m->loop(*this);
}

void Base::wait(uint32_t ms) {
#if defined(ESP32)
  delay(ms);
#else
  const uint32_t end = millis() + ms;
  do {
    tick(millis());
  } while (!reached(millis(), end));
#endif
}

void Base::idle() {
#if defined(ESP32)
  yield();
#else
  tick(millis());
#endif
}

void Base::tick(uint32_t now) {
  if (buzzing_ && reached(now, buzzerOffAt_)) {
    digitalWrite(buzzerPin_, LOW);
    buzzing_ = false;
  }

  const bool arm = !configError_ && (armPin_ == NO_PIN || digitalRead(armPin_) == LOW);
  if (arm != armed_) {
    armed_ = arm;
    if (!arm)
      for (Module *m = first_; m; m = m->next_) m->disarm(*this);
    log().println(arm ? F("ARM on") : F("ARM off - SAFE"));
  }

  Health worst = configError_ ? Health::Fault : Health::Ok;
  if (!configError_) {
    for (Module *m = first_; m; m = m->next_) {
      m->tick(*this, now);
      Health h = m->health();
      if ((uint8_t)h > (uint8_t)worst) worst = h;
    }
  }
  updateLed(now, worst);
}

void Base::updateLed(uint32_t now, Health worst) {
  if (ledPin_ == NO_PIN) return;
  bool on;
  if (worst == Health::Fault) on = (now / 100) % 2;          // швидко — аварія
  else if (!armed_) on = now % 2000 < 60;                     // SAFE: живий
  else if (worst == Health::Busy) on = (now / 400) % 2;       // зайнятий
  else on = true;                                             // готовий
  if (on != ledOn_) {
    ledOn_ = on;
    digitalWrite(ledPin_, on != ledActiveLow_ ? HIGH : LOW);
  }
}

void Base::beep(uint16_t hz, uint16_t ms) {
  if (buzzerPin_ == NO_PIN) return;
  if (buzzerActive_) {
    digitalWrite(buzzerPin_, HIGH);
    buzzerOffAt_ = millis() + ms;
    buzzing_ = true;
  } else {
#if !defined(ESP32)
    tone(buzzerPin_, hz, ms);
#endif
  }
}

bool Base::claimPin(uint8_t pin, const __FlashStringHelper *who) {
  if (pin == NO_PIN) return true;
  if (pin >= FLY_MAX_PINS) {
    configError(who, F("pin number out of range"));
    return false;
  }
  if (pinOwner_[pin]) {
    log().print(F("CONFIG ERROR: pin "));
    log().print(pin);
    log().print(F(" wanted by "));
    log().print(who);
    log().print(F(", already used by "));
    log().println(pinOwner_[pin]);
    configError_ = true;
    return false;
  }
  pinOwner_[pin] = who;
  return true;
}

bool Base::claim(Res r, const __FlashStringHelper *who) {
  const __FlashStringHelper *&owner = resOwner_[(uint8_t)r];
  if (owner) {
    log().print(F("CONFIG ERROR: "));
    log().print(who);
    log().print(F(" needs a timer/port already used by "));
    log().println(owner);
    configError_ = true;
    return false;
  }
  owner = who;
  return true;
}

void Base::configError(const __FlashStringHelper *who, const __FlashStringHelper *what) {
  log().print(F("CONFIG ERROR ["));
  log().print(who);
  log().print(F("]: "));
  log().println(what);
  configError_ = true;
}

Base::Lock::Lock(Base &b) : b_(b) {
#if defined(ESP32)
  if (b_.mutex_) xSemaphoreTakeRecursive(b_.mutex_, portMAX_DELAY);
#endif
}

Base::Lock::~Lock() {
#if defined(ESP32)
  if (b_.mutex_) xSemaphoreGiveRecursive(b_.mutex_);
#endif
}

}  // namespace fly
