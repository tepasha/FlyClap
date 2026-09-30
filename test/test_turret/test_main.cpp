// Хост-тест модуля Turret: правила безпеки й таймінги пострілу (FirePolicy),
// які однаково діють для сонара, камери й команд по UART.
#include <FlyDefense.h>
#include <unity.h>
#include "fly_test.h"

static fly::Base base(A0, 13);
static fly::PanTilt head(9, 10);
static fly::Nozzle gun(5, fly::Nozzle::PUMP);
static fly::Turret T(head, gun);

static void arm(bool on) { g_pins[A0] = on ? LOW : HIGH; }

// Крок 1 мс; сенсор шле рішення кожні 33 мс (~30 кадр/с)
static void run(uint32_t ms, bool frames, int pan = 900, int tilt = 900, bool fire = true) {
  for (uint32_t i = 0; i < ms; i++) {
    if (frames && i % 33 == 0) T.aim(pan, tilt, fire);
    base.update();
    g_ms++;
  }
}

static uint32_t measureShot() {
  uint32_t on = 0;
  int i = 0;
  for (; i < 4000 && !T.valveOn(); i++) run(1, i % 33 == 0);
  for (; i < 4000 && T.valveOn(); i++) {
    on++;
    run(1, i % 33 == 0);
  }
  return on;
}

static void waitForShot() {
  for (int i = 0; !T.valveOn() && i < 500; i++) run(1, true);
}

static void test_turret() {
  g_shotPin = 5;
  arm(false);
  base.add(T);
  CHECK(base.begin(), "begin failed");

  run(1000, true);
  CHECK(g_shotStarts == 0, "fired while SAFE: %d", g_shotStarts);

  arm(true);
  run(100, true);
  CHECK(g_shotStarts == 1, "expected first shot, got %d", g_shotStarts);
  run(2000, true);
  CHECK(g_shotStarts == 3, "burst should cap at 3, got %d", g_shotStarts);
  run(1100, true);
  CHECK(g_shotStarts == 3, "fired during cooldown: %d", g_shotStarts);
  run(200, true);
  CHECK(g_shotStarts == 4, "after cooldown expected 4th shot, got %d", g_shotStarts);

  uint32_t len = measureShot();
  CHECK(len + 1 >= 120 && len <= 121, "pump shot length %u, expected 120", len);

  T.config().shotMs = 50;  // профіль клапана
  run(3000, false);
  len = measureShot();
  CHECK(len >= 49 && len <= 51, "valve shot length %u, expected 50", len);

  // Великий об'єкт
  run(3000, false);
  T.bigObject();
  int s = g_shotStarts;
  run(2900, true);
  CHECK(g_shotStarts == s, "fired during big-object hold");
  run(400, true);
  CHECK(g_shotStarts > s, "did not resume after hold");

  // Великий об'єкт посеред пострілу — клапан закривається одразу
  run(3000, false);
  waitForShot();
  CHECK(T.valveOn(), "no shot to interrupt");
  T.bigObject();
  CHECK(!T.valveOn() && g_pins[5] == LOW, "valve still open after big object");

  // Поворот на 60°: чекаємо, поки серво доїде
  run(4000, false);
  s = g_shotStarts;
  uint32_t t0 = g_ms;
  while (g_shotStarts == s && g_ms - t0 < 1000) run(1, (g_ms - t0) % 33 == 0, 300, 900);
  CHECK(g_ms - t0 >= 130, "fired %u ms after a 60 deg move", g_ms - t0);
  CHECK(head.pan10() == 300 && head.panUs() == fly::ServoOut::deg10ToUs(300), "pan10=%d", head.pan10());

  // Застарілий дозвіл
  run(3000, false);
  T.aim(310, 900, true);
  run(200, false);  // один свіжий постріл допустимий
  s = g_shotStarts;
  run(3000, false);
  CHECK(g_shotStarts == s, "fired on a stale request");

  // Межі кутів
  T.aim(-50, 5000, false);
  CHECK(head.pan10() == 0 && head.tilt10() == 1800, "clamp %d/%d", head.pan10(), head.tilt10());

  // ARM вимкнули посеред пострілу
  run(3000, false);
  waitForShot();
  CHECK(T.valveOn(), "no shot to interrupt (ARM)");
  arm(false);
  run(1, false);
  CHECK(!T.valveOn() && g_pins[5] == LOW, "valve open after disarm");

  // Тестовий постріл
  run(500, false);
  CHECK(!T.testShot(100), "test shot while SAFE");
  arm(true);
  run(1, false);
  CHECK(T.testShot(5000), "test shot refused");
  uint32_t on = 0;
  for (int i = 0; i < 600; i++) {
    if (T.valveOn()) on++;
    run(1, false);
  }
  CHECK(on >= 199 && on <= 201, "test shot %u ms, expected 200", on);

}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_turret);
  return UNITY_END();
}
