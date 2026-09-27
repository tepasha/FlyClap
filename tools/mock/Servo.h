#pragma once
struct Servo {
  int us = 1472;
  void attach(int) {}
  void writeMicroseconds(int v) { us = v; }
  float deg() const { return (us - 544) * 180.0f / (2400 - 544); }
};
