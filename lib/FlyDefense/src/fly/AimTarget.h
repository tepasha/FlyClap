/*
 * Куди сенсор віддає рішення "ціль тут". Сенсор (камера, сонар, прийом по UART)
 * не знає, хто стрілятиме:
 *   - Turret  — турель на цій самій платі;
 *   - LinkOut — інша плата по UART (роль "очі").
 * Кути — у десятих градуса (0..1800).
 */
#pragma once
#include <stdint.h>

namespace fly {

class AimTarget {
 public:
  // Навестись; fire — сенсор дозволяє стріляти (ціль підтверджена).
  virtual void aim(int pan10, int tilt10, bool fire) = 0;
  // Просто навестись без дозволу (калібрування, огляд).
  virtual void moveTo(int pan10, int tilt10) { aim(pan10, tilt10, false); }
  // Тестовий постріл для калібрування. false — відмовлено (SAFE, серво їде…).
  virtual bool testShot(uint16_t ms) = 0;
  // У зоні великий об'єкт (рука, кіт): не стріляти.
  virtual void bigObject() = 0;
  // Сенсор замовк: скасувати дозвіл.
  virtual void cancel() {}
};

}  // namespace fly
