/*
 * Турель-водомет: голова pan-tilt + ствол (помпа/клапан) + політика пострілу.
 * Сама не шукає ціль — її наводять сенсори через інтерфейс AimTarget
 * (Sonar, CameraEyes, EyesLink). Стріляє лише за правилами FirePolicy.
 *
 *   fly::PanTilt head(9, 10);
 *   fly::Nozzle  gun(5, fly::Nozzle::PUMP);
 *   fly::Turret  turret(head, gun);
 *   base.add(turret);
 */
#pragma once
#include "AimTarget.h"
#include "FirePolicy.h"
#include "Module.h"
#include "Nozzle.h"
#include "PanTilt.h"

namespace fly {

class Turret : public Module, public AimTarget {
 public:
  Turret(PanTilt &head, Nozzle &nozzle, int16_t startPan10 = 900, int16_t startTilt10 = 900);

  // Налаштування політики (кулдаун, серії…); shotMs береться з профілю ствола.
  FireConfig &config() { return policy_.cfg; }

  const __FlashStringHelper *name() const override { return F("Turret"); }
  bool begin(Base &base) override;
  void tick(Base &base, uint32_t now) override;
  void loop(Base &base) override;

  // AimTarget
  void aim(int pan10, int tilt10, bool fire) override;
  bool testShot(uint16_t ms) override;
  void bigObject() override;
  void cancel() override;

  // Навести без дозволу; повертає, скільки мс серво їхатиме.
  uint16_t moveAndSettleMs(int pan10, int tilt10);

  bool valveOn() const { return policy_.valveOn; }
  uint32_t shots() const { return policy_.shots; }
  PanTilt &head() { return head_; }
  const Nozzle &nozzle() const { return nozzle_; }

  // Надрукувати постріли, що відбулися (loop() робить це сам; блокуючі сенсори
  // викликають одразу після пострілу, щоб лог ішов по порядку).
  void printShots();

 private:
  PanTilt &head_;
  Nozzle &nozzle_;
  FirePolicy policy_;
  Base *base_ = nullptr;
  int16_t startPan10_, startTilt10_;
  bool valveShown_ = false;
  uint32_t logged_ = 0;
};

}  // namespace fly
