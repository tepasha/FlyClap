/*
 * Передавач рішень сенсора на іншу плату по UART (роль "очі"): реалізує
 * AimTarget, тож CameraEyes не знає, чи стріляє турель поруч, чи Arduino
 * на іншому кінці дроту. Приймач — EyesLink.
 *
 *   fly::LinkOut link(Serial1, 14);   // ESP32: TX на GPIO14 → D0 Arduino
 */
#pragma once
#include "AimTarget.h"
#include "Module.h"
#include "Protocol.h"

namespace fly {

class LinkOut : public Module, public AimTarget {
 public:
  // txPin — лише для ESP32 (довільний GPIO); на AVR порт має фіксовані піни.
  LinkOut(HardwareSerial &port, uint8_t txPin, unsigned long baud = 115200)
      : port_(port), txPin_(txPin), baud_(baud) {}

  const __FlashStringHelper *name() const override { return F("LinkOut"); }
  bool begin(Base &base) override;

  void aim(int pan10, int tilt10, bool fire) override { send('A', pan10, tilt10, fire ? 1 : 0); }
  void moveTo(int pan10, int tilt10) override { send('M', pan10, tilt10, 0); }
  bool testShot(uint16_t ms) override {
    send('F', ms > 32767 ? 32767 : ms, 0, 0);
    return true;  // рішення за приймачем
  }
  void bigObject() override { send('B', 0, 0, 0); }

 private:
  void send(char kind, int a, int b, int c);

  HardwareSerial &port_;
  uint8_t txPin_;
  unsigned long baud_;
};

}  // namespace fly
