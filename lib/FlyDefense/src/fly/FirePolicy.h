/*
 * Політика пострілу: коли турель може стріляти. Одна для всіх джерел цілей
 * (сонар, камера, команди по UART). Без залежностей від Arduino — тестується на ПК
 * (pio test -e native).
 *
 *   постріл, лише якщо  ARM увімкнено
 *                       серво доїхало (пауза залежить від кута повороту)
 *                       дозвіл від сенсора свіжий (<= fireFreshMs)
 *                       не було великого об'єкта bigHoldMs
 *                       не вичерпано серію maxShots / минув кулдаун
 *   вимкнення ARM або великий об'єкт зупиняють клапан посеред пострілу.
 */
#pragma once
#include <stdint.h>

namespace fly {

struct FireConfig {
  uint16_t shotMs = 120;         // помпа; для клапана під тиском ~50
  uint16_t shotGapMs = 350;      // між пострілами серії
  uint16_t cooldownMs = 2500;    // після серії
  uint8_t maxShots = 3;
  uint16_t bigHoldMs = 3000;     // тиша після великого об'єкта
  uint16_t fireFreshMs = 150;    // дозвіл старший за це — ціль уже деінде
  uint16_t testShotMaxMs = 200;
};

class FirePolicy {
 public:
  FireConfig cfg;
  bool valveOn = false;
  uint32_t shots = 0;  // усього пострілів, включно з тестовими

  static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

  // Серво почало рух і доїде через settleMs.
  void moved(uint32_t now, uint16_t settleMs) { settledAt_ = now + settleMs; }

  // Дозвіл від сенсора (надходить з кожним виміром/кадром).
  void request(bool fire, uint32_t now) {
    wantFire_ = fire;
    if (fire) lastFireReqAt_ = now;
  }

  // Сенсор замовк (втрачено зв'язок): дозволу більше немає.
  void cancel() { wantFire_ = false; }

  // Великий об'єкт у зоні: клапан закривається одразу.
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
    if (!wantFire_ && now - lastFireReqAt_ > 1000) burst_ = 0;  // ціль пропала — нова серія

    if (fresh && armed && !valveOn && reached(now, settledAt_) && reached(now, nextShotAt_) &&
        reached(now, bigHoldUntil_)) {
      startShot(cfg.shotMs, now);
      if (++burst_ >= cfg.maxShots) {
        burst_ = 0;
        nextShotAt_ = now + cfg.cooldownMs;
      } else {
        nextShotAt_ = now + cfg.shotGapMs;
      }
      return true;
    }
    return false;
  }

 private:
  void startShot(uint16_t ms, uint32_t now) {
    valveOn = true;
    valveOffAt_ = now + ms;
    shots++;
  }

  bool wantFire_ = false;
  uint32_t lastFireReqAt_ = 0;
  uint32_t settledAt_ = 0, nextShotAt_ = 0, bigHoldUntil_ = 0, valveOffAt_ = 0;
  uint8_t burst_ = 0;
};

}  // namespace fly
