/*
 * Логіка турелі для ESP32-версії FlySonar: коли можна стріляти.
 * Без залежностей від Arduino/ESP-IDF — тестується на ПК (tools/turret_test.cpp).
 * Правила ті самі, що в Arduino-версії (flysonar.ino, режим камери):
 *
 *   постріл, лише якщо  ARM увімкнено
 *                       серво доїхало (пауза залежить від кута повороту)
 *                       дозвіл від зору свіжий (<= fireFreshMs)
 *                       не було великого об'єкта bigHoldMs
 *                       не вичерпано серію maxShots / минув кулдаун
 *   вимкнення ARM або великий об'єкт зупиняють клапан посеред пострілу.
 *
 * Кути — у десятих градуса (0..1800). Клас лише рахує стан; застосовує його
 * до заліза (серви, клапан) викликаючий код.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>

namespace fv {

struct TurretConfig {
  uint16_t shotMs        = 120;   // помпа; для клапана під тиском ~50
  uint16_t shotGapMs     = 350;   // між пострілами серії
  uint16_t cooldownMs    = 2500;  // після серії
  uint8_t  maxShots      = 3;
  uint16_t bigHoldMs     = 3000;  // тиша після великого об'єкта
  uint16_t fireFreshMs   = 150;   // дозвіл старший за це — ціль уже деінде
  uint16_t testShotMaxMs = 200;
  uint8_t  settleBaseMs  = 12;    // мінімальна пауза після команди серво
  uint8_t  settleMsPerDeg = 2;    // ~0.1 с/60° + запас
  uint16_t settleMaxMs   = 300;
};

class Turret {
 public:
  TurretConfig cfg;
  int16_t  pan10 = 900, tilt10 = 900;
  bool     valveOn = false;
  uint32_t shots = 0;

  static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

  // Навести. true — кут змінився (треба оновити серви).
  bool aim(int pan, int tilt, uint32_t now) {
    int16_t p = clamp10(pan), t = clamp10(tilt);
    if (p == pan10 && t == tilt10) return false;
    int16_t dist = abs(p - pan10) > abs(t - tilt10) ? abs(p - pan10) : abs(t - tilt10);
    uint32_t ms = cfg.settleBaseMs + (uint32_t)dist * cfg.settleMsPerDeg / 10;
    settledAt_ = now + (ms > cfg.settleMaxMs ? cfg.settleMaxMs : ms);
    pan10 = p; tilt10 = t;
    return true;
  }

  // Дозвіл від зору (надходить з кожним кадром)
  void request(bool fire, uint32_t now) {
    wantFire_ = fire;
    if (fire) lastFireReqAt_ = now;
  }

  // Зір бачить великий об'єкт
  void bigObject(uint32_t now) {
    bigHoldUntil_ = now + cfg.bigHoldMs;
    wantFire_ = false;
    valveOn = false;
  }

  // Тестовий постріл (калібрування). true — почали.
  bool testShot(uint16_t ms, uint32_t now, bool armed) {
    if (!armed || valveOn || ms == 0 || !reached(now, settledAt_) || !reached(now, bigHoldUntil_)) return false;
    startShot(ms > cfg.testShotMaxMs ? cfg.testShotMaxMs : ms, now);
    return true;
  }

  // Викликати часто (~1 мс). true — цього разу почався постріл.
  bool update(uint32_t now, bool armed) {
    if (valveOn && (reached(now, valveOffAt_) || !armed)) valveOn = false;

    const bool fresh = wantFire_ && now - lastFireReqAt_ < cfg.fireFreshMs;
    if (!wantFire_ && now - lastFireReqAt_ > 1000) burst_ = 0;   // ціль пропала — нова серія

    if (fresh && armed && !valveOn && reached(now, settledAt_) &&
        reached(now, nextShotAt_) && reached(now, bigHoldUntil_)) {
      startShot(cfg.shotMs, now);
      if (++burst_ >= cfg.maxShots) { burst_ = 0; nextShotAt_ = now + cfg.cooldownMs; }
      else                          { nextShotAt_ = now + cfg.shotGapMs; }
      return true;
    }
    return false;
  }

  bool linkFresh(uint32_t now) const { return now - lastFireReqAt_ < cfg.fireFreshMs; }

 private:
  static int16_t clamp10(int v) { return (int16_t)(v < 0 ? 0 : v > 1800 ? 1800 : v); }
  void startShot(uint16_t ms, uint32_t now) {
    valveOn = true;
    valveOffAt_ = now + ms;
    shots++;
  }

  bool     wantFire_ = false;
  uint32_t lastFireReqAt_ = 0;
  uint32_t settledAt_ = 0, nextShotAt_ = 0, bigHoldUntil_ = 0, valveOffAt_ = 0;
  uint8_t  burst_ = 0;
};

}  // namespace fv
