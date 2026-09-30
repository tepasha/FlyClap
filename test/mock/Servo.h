#pragma once
#include "Arduino.h"

extern int g_servoUs[NUM_DIGITAL_PINS];  // останній імпульс на піні; 0 — серво відключене

struct Servo {
  int pin = -1;
  int us = 1472;
  void attach(int p) {
    pin = p;
    g_servoUs[p] = us;
  }
  void detach() {
    if (pin >= 0) g_servoUs[pin] = 0;
    pin = -1;
  }
  bool attached() const { return pin >= 0; }
  void writeMicroseconds(int v) {
    us = v;
    if (pin >= 0) g_servoUs[pin] = v;
  }
};

// Кут серво за імпульсом, градуси
inline float servoDeg(int us) { return (us - 544) * 180.0f / (2400 - 544); }
