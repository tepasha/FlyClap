// Хост-тест FlySonar у режимі камери: протокол з ESP32, серії, кулдаун,
// блокування великих об'єктів, втрата зв'язку, ARM. Запуск: tools/run_tests.sh
#define TARGET_SOURCE_CAMERA 1
#include <Arduino.h>
#include "mock_globals.h"
#include "../firmware/flysonar/flysonar.ino"

static void send(const char *s) { for (; *s; s++) g_rx.push_back(*s); }

// Крутимо loop() з кроком 1 мс; ESP шле aLine кожні 33 мс (~30 кадр/с)
static void run(uint32_t ms, const char *aLine) {
  for (uint32_t i = 0; i < ms; i++) {
    if (aLine && i % 33 == 0) send(aLine);
    loop();
    g_ms++;
  }
}

static int usOf(int deg10) { return 544 + (long)deg10 * (2400 - 544) / 1800; }

int main() {
  g_pins[A0] = HIGH;                              // ARM вимкнено
  setup();
  run(1000, "A 900 900 1\n");
  CHECK(g_shotStarts == 0, "fired while SAFE: %d", g_shotStarts);

  g_pins[A0] = LOW;                               // ARM
  run(100, "A 900 900 1\n");
  CHECK(g_shotStarts == 1, "expected first shot, got %d", g_shotStarts);
  run(2000, "A 900 900 1\n");
  CHECK(g_shotStarts == 3, "burst should cap at MAX_SHOTS=3, got %d", g_shotStarts);
  run(1100, "A 900 900 1\n");                     // серія ~0.7 с + кулдаун 2.5 с ще не минули
  CHECK(g_shotStarts == 3, "fired during cooldown: %d", g_shotStarts);
  run(200, "A 900 900 1\n");
  CHECK(g_shotStarts == 4, "after cooldown expected 4th shot, got %d", g_shotStarts);

  // Один постріл триває SHOT_MS
  {
    uint32_t on = 0; int i = 0;
    for (; i < 4000 && !g_pins[5]; i++) { if (i % 33 == 0) send("A 900 900 1\n"); loop(); g_ms++; }
    for (; i < 4000 && g_pins[5]; i++)  { if (i % 33 == 0) send("A 900 900 1\n"); on++; loop(); g_ms++; }
    CHECK(on >= SHOT_MS - 2 && on <= SHOT_MS + 2, "shot length %u ms, expected %u", on, SHOT_MS);
  }

  // Великий об'єкт: мовчимо BIG_HOLD_MS, навіть коли A просить стріляти
  run(3000, nullptr);
  send("B\n");
  int s = g_shotStarts;
  run(2900, "A 900 900 1\n");
  CHECK(g_shotStarts == s, "fired during big-object hold");
  run(400, "A 900 900 1\n");
  CHECK(g_shotStarts > s, "did not resume after hold");

  // Великий поворот: стріляємо лише після того, як серво доїхало
  run(3000, nullptr);
  s = g_shotStarts;
  send("A 300 900 1\n");
  uint32_t t0 = g_ms;
  while (g_shotStarts == s && g_ms - t0 < 1000) { if ((g_ms - t0) % 33 == 0) send("A 300 900 1\n"); loop(); g_ms++; }
  CHECK(g_ms - t0 >= 130, "fired %u ms after a 60 deg move, before servo settled", g_ms - t0);
  CHECK(panServo.us == usOf(300), "pan us=%d", panServo.us);

  // Застарілий дозвіл: одна команда A, далі тиша — не стріляємо
  run(3000, nullptr);
  send("A 300 900 1\n");
  run(10, nullptr);
  int after = g_shotStarts;
  run(3000, nullptr);
  CHECK(g_shotStarts == after, "fired on a stale request (%d extra)", g_shotStarts - after);

  // Сміття й задовгі рядки не ламають парсер і не рухають серво
  int before = panServo.us;
  send("A 12\nXYZ\nA 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\n");
  run(50, nullptr);
  CHECK(panServo.us == before, "garbage moved servo");
  send("M 1200 900\n");
  run(5, nullptr);
  CHECK(panServo.us == usOf(1200), "M not applied");

  // ARM вимкнули посеред пострілу — помпа/клапан вимикається одразу
  run(3000, nullptr);
  t0 = g_ms;
  while (!g_pins[5] && g_ms - t0 < 500) { if ((g_ms - t0) % 33 == 0) send("A 1200 900 1\n"); loop(); g_ms++; }
  CHECK(g_pins[5], "no shot to interrupt");
  g_pins[A0] = HIGH;
  loop();
  CHECK(!g_pins[5], "pump still on after disarm");

  // Тестовий постріл: лише з ARM і не довше TEST_SHOT_MAX_MS
  s = g_shotStarts;
  send("F 150\n");
  run(300, nullptr);
  CHECK(g_shotStarts == s, "test shot while SAFE");
  g_pins[A0] = LOW;
  send("F 5000\n");
  {
    uint32_t on = 0;
    for (int i = 0; i < 600; i++) { loop(); if (g_pins[5]) on++; g_ms++; }
    CHECK(on >= TEST_SHOT_MAX_MS - 2 && on <= TEST_SHOT_MAX_MS + 2, "test shot %u ms, expected %u", on, TEST_SHOT_MAX_MS);
  }

  printf(failures ? "flysonar camera: %d FAILURE(S)\n" : "flysonar camera: ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
