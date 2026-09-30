#include "Turret.h"
#include "Base.h"

namespace fly {

const Nozzle::Profile Nozzle::PUMP = {120, 25};
const Nozzle::Profile Nozzle::VALVE = {50, 8};

Turret::Turret(PanTilt &head, Nozzle &nozzle, int16_t startPan10, int16_t startTilt10)
    : head_(head), nozzle_(nozzle), startPan10_(startPan10), startTilt10_(startTilt10) {
  policy_.cfg.shotMs = nozzle.profile().shotMs;
}

bool Turret::begin(Base &base) {
  base_ = &base;
  if (!base.claimPin(nozzle_.pin(), name()) || !head_.begin(base, name())) return false;
  pinMode(nozzle_.pin(), OUTPUT);
  digitalWrite(nozzle_.pin(), LOW);
  policy_.moved(millis(), head_.aim(startPan10_, startTilt10_));
  return true;
}

void Turret::tick(Base &base, uint32_t now) {
  policy_.update(now, base.armed());
  if (policy_.valveOn != valveShown_) {
    valveShown_ = policy_.valveOn;
    digitalWrite(nozzle_.pin(), valveShown_ ? HIGH : LOW);
  }
}

void Turret::loop(Base &) { printShots(); }

void Turret::printShots() {
  if (!base_) return;
  uint32_t shots;
  int16_t p, t;
  {
    Base::Lock l(*base_);
    shots = policy_.shots;
    p = head_.pan10();
    t = head_.tilt10();
  }
  if (shots == logged_) return;
  logged_ = shots;
  Print &log = base_->log();
  log.print(F("PSSHT! pan="));
  log.print(p / 10.0f, 1);
  log.print(F(" tilt="));
  log.print(t / 10.0f, 1);
  log.print(F(" total="));
  log.println(shots);
}

void Turret::aim(int pan10, int tilt10, bool fire) {
  if (!base_) return;
  Base::Lock l(*base_);
  const uint32_t now = millis();
  const uint16_t settle = head_.aim(pan10, tilt10);
  if (settle) policy_.moved(now, settle);
  policy_.request(fire, now);
}

uint16_t Turret::moveAndSettleMs(int pan10, int tilt10) {
  if (!base_) return 0;
  Base::Lock l(*base_);
  const uint32_t now = millis();
  const uint16_t settle = head_.aim(pan10, tilt10);
  if (settle) policy_.moved(now, settle);
  policy_.request(false, now);
  return settle;
}

bool Turret::testShot(uint16_t ms) {
  if (!base_) return false;
  Base::Lock l(*base_);
  return policy_.testShot(ms, millis(), base_->armed());
}

void Turret::bigObject() {
  if (!base_) return;
  Base::Lock l(*base_);
  policy_.bigObject(millis());
  digitalWrite(nozzle_.pin(), LOW);  // не чекаючи tick()
  valveShown_ = false;
}

void Turret::cancel() {
  if (!base_) return;
  Base::Lock l(*base_);
  policy_.cancel();
}

}  // namespace fly
