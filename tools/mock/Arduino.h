// Мінімальний макет Arduino API для хост-тестів прошивок (див. tools/run_tests.sh).
// Час віртуальний: g_ms рухає тест; з g_autoTick кожен виклик millis() додає 1 мс,
// щоб активні очікування (while (millis() - t < X)) завершувались.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <deque>
#include <algorithm>
#include <functional>

#define F(s) s
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define A0 14

extern uint32_t g_ms;
extern bool g_autoTick;
extern int g_pins[20];
extern int g_shotStarts;                    // скільки разів D5 (помпа/клапан) перейшов у HIGH
extern std::deque<char> g_rx;               // що "надійшло" в Serial
extern std::function<unsigned long()> g_pulseHook;   // відповідь сонара, мкс
extern std::function<void()> g_onShot;               // викликається на фронті пострілу

inline uint32_t millis() { return g_autoTick ? g_ms++ : g_ms; }
inline void delay(uint32_t ms) { g_ms += ms; }
inline void delayMicroseconds(unsigned) {}
inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) {
  if (p == 5 && v && !g_pins[5]) { g_shotStarts++; if (g_onShot) g_onShot(); }
  g_pins[p] = v;
}
inline int digitalRead(int p) { return g_pins[p]; }
inline unsigned long pulseIn(int, int, unsigned long timeout) {
  unsigned long us = g_pulseHook ? g_pulseHook() : 0;
  if (us > timeout) us = 0;
  g_ms += (us ? us : timeout) / 1000;
  return us;
}

#include <iostream>
extern bool g_serialEcho;                   // true — дублювати лог прошивки в stdout
struct MockSerial {
  void begin(long) {}
  int available() { return !g_rx.empty(); }
  char read() { char c = g_rx.front(); g_rx.pop_front(); return c; }
  template <class T> void print(T v) { if (g_serialEcho) std::cout << v; }
  template <class T> void print(T v, int) { if (g_serialEcho) std::cout << v; }
  template <class T> void println(T v) { if (g_serialEcho) std::cout << v << "\n"; }
  void println() { if (g_serialEcho) std::cout << "\n"; }
};
extern MockSerial Serial;

using std::min;
using std::max;
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
