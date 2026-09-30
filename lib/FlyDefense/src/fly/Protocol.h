/*
 * Протокол "очі" → "стрілок" (UART 115200 8N1, рядки ASCII, кути в 0.1°):
 *
 *   A <pan10> <tilt10> <fire>\n  — навестись, fire=1 — можна стріляти
 *   M <pan10> <tilt10>\n         — просто навестись (калібрування)
 *   F <ms>\n                     — тестовий постріл (калібрування)
 *   B\n                          — у зоні великий об'єкт: не стріляти
 *
 * Той самий код і формує команди (LinkOut), і розбирає їх (EyesLink).
 */
#pragma once
#include <Arduino.h>

namespace fly {

struct Command {
  char kind = 0;  // 'A', 'M', 'F', 'B'
  int16_t v[3] = {0, 0, 0};

  // Розібрати рядок без '\n'. false — сміття або не вистачає чисел.
  bool parse(const char *line);
  // Надіслати рядок з '\n'.
  void send(Print &out) const;
};

// Збирає рядки з байтів; задовгий рядок відкидається цілком.
class LineReader {
 public:
  // Подати байт. Повертає готовий непорожній рядок (без \r\n) або nullptr.
  const char *push(char c);

 private:
  char buf_[32];
  uint8_t len_ = 0;
  bool overflow_ = false;
};

}  // namespace fly
