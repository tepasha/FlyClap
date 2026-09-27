// Хост-тест логіки турелі ESP32-версії (firmware/flysonar_esp32/turret.h).
// Ті самі сценарії безпеки, що й для Arduino-версії (flysonar_camera_test.cpp).
#include "turret.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fv::Turret T;
static uint32_t now = 1000;
static bool arm = false;
static int starts = 0;

// Крок 1 мс; зір шле кадр кожні 33 мс
static void run(uint32_t ms, bool frames, int pan = 900, int tilt = 900, bool fire = true) {
  for (uint32_t i = 0; i < ms; i++) {
    if (frames && i % 33 == 0) { T.aim(pan, tilt, now); T.request(fire, now); }
    if (T.update(now, arm)) starts++;
    now++;
  }
}

static uint32_t measureShot(int pan = 900, int tilt = 900) {
  uint32_t on = 0; int i = 0;
  for (; i < 4000 && !T.valveOn; i++) run(1, i % 33 == 0, pan, tilt);
  for (; i < 4000 && T.valveOn; i++) { on++; run(1, i % 33 == 0, pan, tilt); }
  return on;
}

int main() {
  run(1000, true);
  CHECK(starts == 0, "fired while SAFE: %d", starts);

  arm = true;
  run(100, true);
  CHECK(starts == 1, "expected first shot, got %d", starts);
  run(2000, true);
  CHECK(starts == 3, "burst should cap at 3, got %d", starts);
  run(1100, true);
  CHECK(starts == 3, "fired during cooldown: %d", starts);
  run(200, true);
  CHECK(starts == 4, "after cooldown expected 4th shot, got %d", starts);

  uint32_t len = measureShot();
  CHECK(len + 1 >= T.cfg.shotMs && len <= T.cfg.shotMs + 1u, "shot length %u, expected %u", len, T.cfg.shotMs);

  T.cfg.shotMs = 50;                                // профіль клапана
  run(3000, false);
  len = measureShot();
  CHECK(len >= 49 && len <= 51, "valve shot length %u, expected 50", len);

  // Великий об'єкт
  run(3000, false);
  T.bigObject(now);
  int s = starts;
  run(2900, true);
  CHECK(starts == s, "fired during big-object hold");
  run(400, true);
  CHECK(starts > s, "did not resume after hold");

  // Великий об'єкт посеред пострілу — клапан закривається одразу
  run(3000, false);
  { int i = 0; while (!T.valveOn && i++ < 500) run(1, true); }
  CHECK(T.valveOn, "no shot to interrupt");
  T.bigObject(now);
  CHECK(!T.valveOn, "valve still open after big object");

  // Поворот на 60°: чекаємо, поки серво доїде
  run(4000, false);
  s = starts;
  uint32_t t0 = now;
  while (starts == s && now - t0 < 1000) run(1, (now - t0) % 33 == 0, 300, 900);
  CHECK(now - t0 >= 130, "fired %u ms after a 60 deg move", now - t0);
  CHECK(T.pan10 == 300, "pan10=%d", T.pan10);

  // Застарілий дозвіл
  run(3000, false);
  T.aim(310, 900, now); T.request(true, now);
  run(200, false);                                  // один свіжий постріл допустимий
  s = starts;
  run(3000, false);
  CHECK(starts == s, "fired on a stale request");

  // Межі кутів
  T.aim(-50, 5000, now);
  CHECK(T.pan10 == 0 && T.tilt10 == 1800, "clamp %d/%d", T.pan10, T.tilt10);

  // ARM вимкнули посеред пострілу
  run(3000, false);
  { int i = 0; while (!T.valveOn && i++ < 500) run(1, true, 900, 900); }
  CHECK(T.valveOn, "no shot to interrupt (ARM)");
  arm = false;
  run(1, false);
  CHECK(!T.valveOn, "valve open after disarm");

  // Тестовий постріл
  run(500, false);
  CHECK(!T.testShot(100, now, false), "test shot while SAFE");
  arm = true;
  CHECK(T.testShot(5000, now, true), "test shot refused");
  { uint32_t on = 0; for (int i = 0; i < 600; i++) { if (T.valveOn) on++; run(1, false); }
    CHECK(on >= 199 && on <= 201, "test shot %u ms, expected 200", on); }

  printf(failures ? "turret: %d FAILURE(S)\n" : "turret: ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
