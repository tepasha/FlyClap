// Спільне для тестів на ПК (pio test -e native): Unity + макет Arduino API.
// Кожен тест — окрема програма з одного файла, тому реалізація макета
// підключається прямо сюди.
#pragma once
#include <stdio.h>
#include <unity.h>
#include "mock.cpp"

// Перевірка з повідомленням у стилі printf; провал зупиняє поточний тест.
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      char fly_msg_[256];                         \
      snprintf(fly_msg_, sizeof(fly_msg_), __VA_ARGS__); \
      TEST_FAIL_MESSAGE(fly_msg_);                \
    }                                             \
  } while (0)

void setUp() {}
void tearDown() {}
