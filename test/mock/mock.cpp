// Глобальний стан макета Arduino (test/mock/Arduino.h)
#include "Arduino.h"
#include "EEPROM.h"
#include "Servo.h"

uint32_t g_ms = 1000;
bool g_autoTick = false;
int g_pins[NUM_DIGITAL_PINS];
int g_shotPin = -1;
int g_shotStarts = 0;
std::function<void()> g_onShot;
std::function<unsigned long()> g_pulseHook;
int g_toneHz = 0;
std::deque<char> g_rx;
std::string g_log;
int g_servoUs[NUM_DIGITAL_PINS];
MockEEPROM EEPROM;

static const bool g_serialEcho = getenv("SERIAL_ECHO") != nullptr;  // дублювати лог у stdout

size_t HardwareSerial::write(uint8_t c) {
  if (!console_) return 1;
  g_log += (char)c;
  if (g_serialEcho) putchar(c);
  return 1;
}

HardwareSerial Serial(true);
HardwareSerial Serial1(false);
