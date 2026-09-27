#pragma once
#include <stdio.h>
uint32_t g_ms = 1000;
bool g_autoTick = false;
int g_pins[20];
int g_shotStarts = 0;
std::deque<char> g_rx;
std::function<unsigned long()> g_pulseHook;
std::function<void()> g_onShot;
bool g_serialEcho = getenv("SERIAL_ECHO") != nullptr;
MockSerial Serial;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
