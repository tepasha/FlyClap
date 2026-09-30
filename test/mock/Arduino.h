// Мінімальний макет Arduino API для тестів бібліотеки на ПК (pio test -e native).
// Час віртуальний: g_ms рухає тест; з g_autoTick кожен виклик millis() додає 1 мс,
// щоб активні очікування (while (millis() - t < X)) завершувались.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <functional>
#include <string>

class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define A0 14
#define NUM_DIGITAL_PINS 20
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

extern uint32_t g_ms;
extern bool g_autoTick;
extern int g_pins[NUM_DIGITAL_PINS];      // рівні на пінах (входи задає тест)
extern int g_shotPin;                     // пін ствола/соленоїда, фронти якого рахуємо
extern int g_shotStarts;                  // скільки разів g_shotPin перейшов у HIGH
extern std::function<void()> g_onShot;    // викликається на фронті g_shotPin
extern std::function<unsigned long()> g_pulseHook;  // відповідь сонара, мкс
extern int g_toneHz;                      // останній tone(); 0 — noTone
extern std::deque<char> g_rx;             // що "надійшло" в Serial
extern std::string g_log;                 // що надруковано в Serial

inline uint32_t millis() { return g_autoTick ? g_ms++ : g_ms; }
inline void delay(uint32_t ms) { g_ms += ms; }
inline void delayMicroseconds(unsigned) {}
inline void yield() {}
inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) {
  if (p == g_shotPin && v && !g_pins[p]) {
    g_shotStarts++;
    if (g_onShot) g_onShot();
  }
  g_pins[p] = v ? HIGH : LOW;
}
inline int digitalRead(int p) { return g_pins[p]; }
inline unsigned long pulseIn(int, int, unsigned long timeout) {
  unsigned long us = g_pulseHook ? g_pulseHook() : 0;
  if (us > timeout) us = 0;
  g_ms += (us ? us : timeout) / 1000;
  return us;
}
inline void tone(int, unsigned hz, unsigned long = 0) { g_toneHz = (int)hz; }
inline void noTone(int) { g_toneHz = 0; }

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  size_t write(const char *s) {
    size_t n = 0;
    while (*s) n += write((uint8_t)*s++);
    return n;
  }
  size_t print(const char *s) { return write(s); }
  size_t print(const __FlashStringHelper *s) { return write(reinterpret_cast<const char *>(s)); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return fmt("%d", v); }
  size_t print(unsigned v) { return fmt("%u", v); }
  size_t print(long v) { return fmt("%ld", v); }
  size_t print(unsigned long v) { return fmt("%lu", v); }
  size_t print(double v, int digits = 2) {
    char b[32];
    snprintf(b, sizeof(b), "%.*f", digits, v);
    return write(b);
  }
  template <class T>
  size_t println(T v) {
    size_t n = print(v);
    return n + write("\r\n");
  }
  size_t println(double v, int digits) { return print(v, digits) + write("\r\n"); }
  size_t println() { return write("\r\n"); }

 private:
  template <class T>
  size_t fmt(const char *f, T v) {
    char b[24];
    snprintf(b, sizeof(b), f, v);
    return write(b);
  }
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
};

class HardwareSerial : public Stream {
 public:
  explicit HardwareSerial(bool isConsole) : console_(isConsole) {}
  void begin(unsigned long) {}
  int available() override { return console_ ? (int)g_rx.size() : 0; }
  int read() override {
    if (!console_ || g_rx.empty()) return -1;
    char c = g_rx.front();
    g_rx.pop_front();
    return (unsigned char)c;
  }
  size_t write(uint8_t c) override;
  using Print::write;

 private:
  bool console_;
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;
