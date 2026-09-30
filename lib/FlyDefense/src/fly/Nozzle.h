/*
 * "Ствол" турелі: помпа або клапан на MOSFET (IRLZ44N). Профіль задає тривалість
 * пострілу і провисання струменя.
 *   PUMP  — помпа R385: їй потрібен час, щоб набрати тиск; струмінь провисає.
 *   VALVE — клапан 12 В + бак під тиском: відкривається за ~5–10 мс, струмінь прямий.
 * Свій ствол (обприскувач із серво на гачку тощо) — інший профіль або свій клас.
 */
#pragma once
#include <Arduino.h>

namespace fly {

class Nozzle {
 public:
  struct Profile {
    uint16_t shotMs;
    uint8_t ballisticX100;  // падіння струменя, сотих градуса на см відстані
  };
  static const Profile PUMP;
  static const Profile VALVE;

  Nozzle(uint8_t pin, const Profile &profile) : pin_(pin), profile_(profile) {}

  uint8_t pin() const { return pin_; }
  const Profile &profile() const { return profile_; }

  // Поправка прицілу вгору на провисання струменя, 0.1° (як (int)(k*d*10 + 0.5))
  int16_t ballisticLift10(uint16_t cm) const { return (int16_t)(((uint32_t)profile_.ballisticX100 * cm + 5) / 10); }

 private:
  uint8_t pin_;
  Profile profile_;
};

}  // namespace fly
